#include "services/audio/synth.h"

#include <math.h>
#include <string.h>

#define CMD_QUEUE_SIZE   128               /* power of two */
#define CMD_QUEUE_MASK   (CMD_QUEUE_SIZE - 1)
#define SCOPE_MASK       (SYNTH_SCOPE_SIZE - 1)
#define CTRL_INTERVAL    16                /* samples between filter coefficient updates */
#define TWO_PI           6.28318530718f
#define PI_F             3.14159265359f
#define SILENCE          1e-5f
#define MAX_VOICE_SECS   20.0f
#define ENV_TIME_K       5.0f              /* exp coef reaches ~0.7% (-43 dB) in the given time */
#define T60_K            6.90776f          /* ln(1000): -60 dB */
#define MODE_INPUT_SCALE 0.35f             /* tuned so a few-ms noise burst peaks near 0.5 */

/* ---- SPSC atomics (game thread produces, audio thread consumes) -------- */

#if defined(_MSC_VER)
#include <intrin.h>
static inline uint32_t sy_load_acquire(const volatile uint32_t *p) { uint32_t v = *p; _ReadWriteBarrier(); return v; }
static inline void sy_store_release(volatile uint32_t *p, uint32_t v) { _ReadWriteBarrier(); *p = v; }
#else
static inline uint32_t sy_load_acquire(const volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void sy_store_release(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
#endif

/* ---- types --------------------------------------------------------------- */

typedef enum CmdType {
    CMD_PLAY = 0,
    CMD_UPDATE,
    CMD_RELEASE,
    CMD_STOP_ALL,
    CMD_MASTER,
} CmdType;

typedef struct Cmd {
    uint8_t        type;
    SynthHandle    handle;
    float          value;
    SynthVoiceDesc desc;
} Cmd;

typedef enum EnvStage {
    ENV_ATTACK = 0,
    ENV_DECAY,
    ENV_RELEASE,
    ENV_DONE,
} EnvStage;

typedef struct Voice {
    SynthHandle    handle;
    uint8_t        active;
    uint8_t        stage;
    SynthVoiceDesc d;
    uint32_t       rng;

    /* per-note random factors, kept so live updates don't lose variation */
    float j_freq, j_cut, j_gain;
    float j_mode[SYNTH_MAX_MODES];

    uint32_t delay_samples;
    uint32_t age;
    uint32_t hold_samples;           /* UINT32_MAX = infinite */
    uint32_t ctrl_count;

    float env, attack_inc, decay_coef, release_coef;
    float freq, phase, lfo_phase, lfo_inc;
    float pitch_mul, pitch_coef;
    float noise_lp, noise_a, noise_comp;
    float grain_env, grain_coef, grain_prob;
    float fenv, fenv_block_coef;
    float svf_ic1, svf_ic2, svf_a1, svf_a2, svf_a3, svf_k;
    float m_b1[SYNTH_MAX_MODES], m_b2[SYNTH_MAX_MODES], m_in[SYNTH_MAX_MODES];
    float m_y1[SYNTH_MAX_MODES], m_y2[SYNTH_MAX_MODES];
    float pan_l, pan_r;
    float drive_k, drive_norm;
} Voice;

struct Synth {
    uint32_t sample_rate;
    float    inv_sr;

    /* game-thread side */
    SynthHandle next_handle;
    float       master_shadow;

    Cmd               queue[CMD_QUEUE_SIZE];
    volatile uint32_t q_head;        /* written by producer */
    volatile uint32_t q_tail;        /* written by consumer */

    /* audio-thread side */
    float             master;
    Voice             voices[SYNTH_MAX_VOICES];
    volatile uint32_t active_count;
    float             scope[SYNTH_SCOPE_SIZE];
    volatile uint32_t scope_pos;
};

/* ---- small helpers ----------------------------------------------------- */

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static inline uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static inline float rng_bipolar(uint32_t *s) { return (float)(int32_t)rng_next(s) * (1.0f / 2147483648.0f); }
static inline float rng_unit(uint32_t *s)    { return (float)(rng_next(s) >> 8) * (1.0f / 16777216.0f); }

static inline float time_coef(float seconds, float sr) {
    return expf(-ENV_TIME_K / (fmaxf(seconds, 0.0005f) * sr));
}

/* rational tanh approximation; exact enough for saturation, clamps past |x| 3 */
static inline float soft_clip(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* PolyBLEP: smooths the discontinuity of a naive saw/pulse so it doesn't alias */
static inline float poly_blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline float osc_sample(uint8_t wave, float p, float dt, float duty) {
    switch (wave) {
    case SYNTH_WAVE_SINE:
        return sinf(TWO_PI * p);
    case SYNTH_WAVE_TRIANGLE:
        return 1.0f - 4.0f * fabsf(p - 0.5f);
    case SYNTH_WAVE_SQUARE:
        duty = 0.5f;
        /* fallthrough */
    case SYNTH_WAVE_PULSE: {
        float v = p < duty ? 1.0f : -1.0f;
        float p2 = p + (1.0f - duty);
        if (p2 >= 1.0f) p2 -= 1.0f;
        v += poly_blep(p, dt) - poly_blep(p2, dt);
        return v - (2.0f * duty - 1.0f);      /* remove DC of uneven duty */
    }
    case SYNTH_WAVE_SAW:
        return 2.0f * p - 1.0f - poly_blep(p, dt);
    default:
        return 0.0f;
    }
}

/* ---- voice setup --------------------------------------------------------- */

static void svf_update(Voice *v, float sr) {
    float fc = v->d.cutoff * v->j_cut * exp2f(v->d.filter_env * v->fenv);
    fc = clampf(fc, 20.0f, sr * 0.45f);
    float g = tanf(PI_F * fc / sr);
    v->svf_k  = 1.0f / clampf(v->d.q, 0.5f, 20.0f);
    v->svf_a1 = 1.0f / (1.0f + g * (g + v->svf_k));
    v->svf_a2 = g * v->svf_a1;
    v->svf_a3 = g * v->svf_a2;
}

/* derive every coefficient from the desc; safe to call on a live voice */
static void voice_apply_desc(Voice *v, const SynthVoiceDesc *d, float sr) {
    v->d = *d;
    SynthVoiceDesc *vd = &v->d;
    vd->duty = clampf(vd->duty, 0.05f, 0.95f);
    if (vd->mode_count > SYNTH_MAX_MODES) vd->mode_count = SYNTH_MAX_MODES;

    v->freq = clampf(vd->freq * v->j_freq, 0.0f, sr * 0.45f);
    v->lfo_inc = vd->lfo_rate / sr;
    v->pitch_coef = time_coef(vd->pitch_time, sr);

    float color = clampf(vd->noise_color, 0.0f, 1.0f);
    float noise_fc = 18000.0f * powf(0.004f, color);              /* 18 kHz .. 72 Hz */
    v->noise_a = 1.0f - expf(-TWO_PI * noise_fc / sr);
    v->noise_comp = fminf(1.0f / sqrtf(v->noise_a / (2.0f - v->noise_a)), 12.0f);
    v->grain_prob = fmaxf(vd->grain_rate, 0.0f) / sr;
    v->grain_coef = time_coef(fmaxf(vd->grain_len, 0.001f), sr);

    v->attack_inc   = 1.0f / (fmaxf(vd->attack, 0.0005f) * sr);
    v->decay_coef   = time_coef(vd->decay, sr);
    v->release_coef = time_coef(vd->release, sr);
    vd->sustain     = clampf(vd->sustain, 0.0f, 1.0f);
    v->hold_samples = vd->hold < 0.0f ? UINT32_MAX : (uint32_t)(vd->hold * sr);

    v->fenv_block_coef = expf(-ENV_TIME_K * CTRL_INTERVAL / (fmaxf(vd->filter_decay, 0.001f) * sr));

    for (uint8_t i = 0; i < vd->mode_count; i++) {
        float f = clampf(vd->modes[i].freq * v->j_mode[i], 20.0f, sr * 0.45f);
        float w = TWO_PI * f / sr;
        float r = expf(-T60_K / (fmaxf(vd->modes[i].decay, 0.005f) * sr));
        v->m_b1[i] = 2.0f * r * cosf(w);
        v->m_b2[i] = -r * r;
        v->m_in[i] = vd->modes[i].gain * sinf(w) * MODE_INPUT_SCALE;
    }

    float angle = (clampf(vd->pan, -1.0f, 1.0f) + 1.0f) * (PI_F * 0.25f);
    v->pan_l = cosf(angle);
    v->pan_r = sinf(angle);

    float drive = clampf(vd->drive, 0.0f, 1.0f);
    v->drive_k = 1.0f + drive * 12.0f;
    v->drive_norm = 1.0f / sqrtf(v->drive_k);

    svf_update(v, sr);
}

static void voice_start(Voice *v, SynthHandle h, const SynthVoiceDesc *d, float sr) {
    memset(v, 0, sizeof(*v));
    v->handle = h;
    v->active = 1;
    v->stage = ENV_ATTACK;
    v->rng = d->seed ? d->seed : (h * 2654435761u) ^ 0x9E3779B9u;
    if (!v->rng) v->rng = 1;

    float j = clampf(d->jitter, 0.0f, 0.5f);
    v->j_freq = 1.0f + j * rng_bipolar(&v->rng);
    v->j_cut  = 1.0f + j * 1.5f * rng_bipolar(&v->rng);
    v->j_gain = fmaxf(1.0f + j * 2.0f * rng_bipolar(&v->rng), 0.1f);
    for (uint8_t i = 0; i < SYNTH_MAX_MODES; i++) v->j_mode[i] = 1.0f + j * rng_bipolar(&v->rng);

    v->pitch_mul = d->pitch_start > 0.0f ? d->pitch_start : 1.0f;
    v->fenv = 1.0f;
    v->delay_samples = d->delay > 0.0f ? (uint32_t)(d->delay * sr) : 0;
    voice_apply_desc(v, d, sr);
}

static Voice *voice_alloc(Synth *s) {
    Voice *oldest = &s->voices[0];
    for (uint32_t i = 0; i < SYNTH_MAX_VOICES; i++) {
        Voice *v = &s->voices[i];
        if (!v->active) return v;
        if (v->age > oldest->age) oldest = v;
    }
    return oldest;                              /* steal */
}

static Voice *voice_find(Synth *s, SynthHandle h) {
    for (uint32_t i = 0; i < SYNTH_MAX_VOICES; i++)
        if (s->voices[i].active && s->voices[i].handle == h) return &s->voices[i];
    return NULL;
}

/* ---- per-sample voice processing ----------------------------------------- */

static void voice_render(Synth *s, Voice *v, float *out, uint32_t frames) {
    const float sr = (float)s->sample_rate;
    const float inv_sr = s->inv_sr;
    const SynthVoiceDesc *d = &v->d;
    const float gain = d->gain * v->j_gain;
    const uint8_t modes = d->mode_count;
    const int use_osc = d->osc_level > 0.0f;
    const int use_noise = d->noise_level > 0.0f;
    const int use_drive = d->drive > 0.001f;

    for (uint32_t i = 0; i < frames; i++) {
        if (v->delay_samples) { v->delay_samples--; continue; }

        if (v->ctrl_count-- == 0) {
            v->ctrl_count = CTRL_INTERVAL - 1;
            if (d->filter != SYNTH_FILTER_OFF) {
                svf_update(v, sr);
                v->fenv *= v->fenv_block_coef;
            }
        }

        /* amplitude envelope */
        switch (v->stage) {
        case ENV_ATTACK:
            v->env += v->attack_inc;
            if (v->env >= 1.0f) { v->env = 1.0f; v->stage = ENV_DECAY; }
            break;
        case ENV_DECAY:
            v->env = d->sustain + (v->env - d->sustain) * v->decay_coef;
            if (v->age >= v->hold_samples) v->stage = ENV_RELEASE;
            else if (d->sustain <= 0.0f && v->env < SILENCE) { v->env = 0.0f; v->stage = ENV_DONE; }
            break;
        case ENV_RELEASE:
            v->env *= v->release_coef;
            if (v->env < SILENCE) { v->env = 0.0f; v->stage = ENV_DONE; }
            break;
        default:
            break;
        }

        float src = 0.0f;
        if (v->stage != ENV_DONE) {
            if (use_osc) {
                float f = v->freq * v->pitch_mul;
                if (d->lfo_depth != 0.0f) {
                    f *= exp2f(d->lfo_depth * (1.0f / 12.0f) * sinf(TWO_PI * v->lfo_phase));
                    v->lfo_phase += v->lfo_inc;
                    if (v->lfo_phase >= 1.0f) v->lfo_phase -= 1.0f;
                }
                float dt = clampf(f * inv_sr, 1e-6f, 0.45f);
                src += osc_sample(d->wave, v->phase, dt, d->duty) * d->osc_level;
                v->phase += dt;
                if (v->phase >= 1.0f) v->phase -= 1.0f;
            }
            v->pitch_mul = 1.0f + (v->pitch_mul - 1.0f) * v->pitch_coef;

            if (use_noise) {
                v->noise_lp += (rng_bipolar(&v->rng) - v->noise_lp) * v->noise_a;
                float n = v->noise_lp * v->noise_comp;
                if (v->grain_prob > 0.0f) {
                    if (rng_unit(&v->rng) < v->grain_prob)
                        v->grain_env = 0.25f + 0.75f * rng_unit(&v->rng);
                    n *= v->grain_env;
                    v->grain_env *= v->grain_coef;
                }
                src += n * d->noise_level;
            }
            src *= v->env;
        }

        /* modal resonators: two-pole resonant filters ringing at each mode */
        float m = 0.0f;
        float ex = src * d->excite;
        for (uint8_t k = 0; k < modes; k++) {
            float y = v->m_b1[k] * v->m_y1[k] + v->m_b2[k] * v->m_y2[k] + v->m_in[k] * ex;
            v->m_y2[k] = v->m_y1[k];
            v->m_y1[k] = y;
            m += y;
        }

        float x = src * d->dry + m * d->modes_level;

        /* Simper/Cytomic state-variable filter */
        if (d->filter != SYNTH_FILTER_OFF) {
            float v3 = x - v->svf_ic2;
            float v1 = v->svf_a1 * v->svf_ic1 + v->svf_a2 * v3;
            float v2 = v->svf_ic2 + v->svf_a2 * v->svf_ic1 + v->svf_a3 * v3;
            v->svf_ic1 = 2.0f * v1 - v->svf_ic1;
            v->svf_ic2 = 2.0f * v2 - v->svf_ic2;
            if (d->filter == SYNTH_FILTER_LP)      x = v2;
            else if (d->filter == SYNTH_FILTER_BP) x = v1;
            else                                   x = x - v->svf_k * v1 - v2;
        }

        if (use_drive) x = soft_clip(x * v->drive_k) * v->drive_norm;

        x *= gain;
        out[2 * i]     += x * v->pan_l;
        out[2 * i + 1] += x * v->pan_r;
        v->age++;
    }

    if (v->stage == ENV_DONE) {
        float energy = 0.0f;
        if (d->modes_level > 0.0f)
            for (uint8_t k = 0; k < modes; k++) energy += fabsf(v->m_y1[k]) + fabsf(v->m_y2[k]);
        if (energy < SILENCE) v->active = 0;
    }
    if ((float)v->age > MAX_VOICE_SECS * sr && v->hold_samples != UINT32_MAX) v->active = 0;
}

/* ---- command queue --------------------------------------------------------- */

static int queue_push(Synth *s, const Cmd *c) {
    uint32_t head = s->q_head;
    uint32_t tail = sy_load_acquire(&s->q_tail);
    if (head - tail >= CMD_QUEUE_SIZE) return 0;
    s->queue[head & CMD_QUEUE_MASK] = *c;
    sy_store_release(&s->q_head, head + 1);
    return 1;
}

static void process_cmd(Synth *s, const Cmd *c) {
    const float sr = (float)s->sample_rate;
    switch (c->type) {
    case CMD_PLAY:
        voice_start(voice_alloc(s), c->handle, &c->desc, sr);
        break;
    case CMD_UPDATE: {
        Voice *v = voice_find(s, c->handle);
        if (v) voice_apply_desc(v, &c->desc, sr);
        break;
    }
    case CMD_RELEASE: {
        Voice *v = voice_find(s, c->handle);
        if (!v) break;
        if (v->delay_samples) { v->active = 0; break; }
        v->hold_samples = v->age;
        break;
    }
    case CMD_STOP_ALL:
        for (uint32_t i = 0; i < SYNTH_MAX_VOICES; i++) s->voices[i].active = 0;
        break;
    case CMD_MASTER:
        s->master = c->value;
        break;
    default:
        break;
    }
}

static void queue_drain(Synth *s) {
    uint32_t tail = s->q_tail;
    uint32_t head = sy_load_acquire(&s->q_head);
    while (tail != head) {
        process_cmd(s, &s->queue[tail & CMD_QUEUE_MASK]);
        tail++;
    }
    sy_store_release(&s->q_tail, tail);
}

/* ---- public API ------------------------------------------------------------ */

size_t synth_size(void) { return sizeof(Synth); }

void synth_init(Synth *s, uint32_t sample_rate) {
    memset(s, 0, sizeof(*s));
    s->sample_rate = sample_rate ? sample_rate : SYNTH_SAMPLE_RATE;
    s->inv_sr = 1.0f / (float)s->sample_rate;
    s->master = 0.6f;
    s->master_shadow = 0.6f;
}

void synth_voice_desc_default(SynthVoiceDesc *d) {
    memset(d, 0, sizeof(*d));
    d->wave = SYNTH_WAVE_SINE;
    d->freq = 220.0f;
    d->duty = 0.5f;
    d->osc_level = 1.0f;
    d->pitch_start = 1.0f;
    d->pitch_time = 0.05f;
    d->noise_color = 0.0f;
    d->grain_len = 0.004f;
    d->attack = 0.005f;
    d->decay = 0.25f;
    d->sustain = 0.0f;
    d->release = 0.1f;
    d->hold = 0.25f;
    d->excite = 1.0f;
    d->dry = 1.0f;
    d->modes_level = 1.0f;
    d->filter = SYNTH_FILTER_OFF;
    d->cutoff = 2000.0f;
    d->q = 0.707f;
    d->filter_decay = 0.1f;
    d->gain = 0.5f;
}

SynthHandle synth_play(Synth *s, const SynthVoiceDesc *d) {
    Cmd c;
    c.type = CMD_PLAY;
    c.handle = ++s->next_handle;
    if (c.handle == 0) c.handle = ++s->next_handle;
    c.value = 0.0f;
    c.desc = *d;
    return queue_push(s, &c) ? c.handle : 0;
}

void synth_update(Synth *s, SynthHandle h, const SynthVoiceDesc *d) {
    if (!h) return;
    Cmd c;
    c.type = CMD_UPDATE;
    c.handle = h;
    c.value = 0.0f;
    c.desc = *d;
    queue_push(s, &c);
}

static void push_simple(Synth *s, uint8_t type, SynthHandle h, float value) {
    Cmd c;
    memset(&c, 0, sizeof(c));
    c.type = type;
    c.handle = h;
    c.value = value;
    queue_push(s, &c);
}

void synth_release(Synth *s, SynthHandle h) { if (h) push_simple(s, CMD_RELEASE, h, 0.0f); }
void synth_stop_all(Synth *s)               { push_simple(s, CMD_STOP_ALL, 0, 0.0f); }

void synth_set_master(Synth *s, float gain) {
    s->master_shadow = clampf(gain, 0.0f, 2.0f);
    push_simple(s, CMD_MASTER, 0, s->master_shadow);
}

float    synth_master(const Synth *s)        { return s->master_shadow; }
uint32_t synth_sample_rate(const Synth *s)   { return s->sample_rate; }
uint32_t synth_active_voices(const Synth *s) { return sy_load_acquire(&s->active_count); }

uint32_t synth_scope_read(const Synth *s, float *out, uint32_t count) {
    if (count > SYNTH_SCOPE_SIZE) count = SYNTH_SCOPE_SIZE;
    uint32_t start = sy_load_acquire(&s->scope_pos) - count;
    for (uint32_t i = 0; i < count; i++) out[i] = s->scope[(start + i) & SCOPE_MASK];
    return count;
}

void synth_render(Synth *s, float *out, uint32_t frames) {
    queue_drain(s);
    memset(out, 0, (size_t)frames * 2 * sizeof(float));

    uint32_t active = 0;
    for (uint32_t i = 0; i < SYNTH_MAX_VOICES; i++) {
        Voice *v = &s->voices[i];
        if (!v->active) continue;
        voice_render(s, v, out, frames);
        if (v->active) active++;
    }

    const float master = s->master;
    uint32_t pos = s->scope_pos;
    for (uint32_t i = 0; i < frames; i++) {
        float l = soft_clip(out[2 * i] * master);
        float r = soft_clip(out[2 * i + 1] * master);
        out[2 * i] = l;
        out[2 * i + 1] = r;
        s->scope[pos & SCOPE_MASK] = 0.5f * (l + r);
        pos++;
    }
    sy_store_release(&s->scope_pos, pos);
    sy_store_release(&s->active_count, active);
}
