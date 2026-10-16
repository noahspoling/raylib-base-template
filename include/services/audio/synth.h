#ifndef SYNTH_H
#define SYNTH_H

/*
 * Procedural sound synthesizer. Pure C, no raylib: the game thread queues
 * commands, the audio thread calls synth_render(). See
 * src/services/audio/README.md for the signal flow.
 *
 * Threading contract: synth_play/update/release/stop_all/set_master are
 * game-thread only (single producer); synth_render is audio-thread only
 * (single consumer). Read-side helpers (voices, scope) are safe anywhere.
 */

#include <stdint.h>
#include <stddef.h>

#define SYNTH_SAMPLE_RATE  48000
#define SYNTH_MAX_VOICES   32
#define SYNTH_MAX_MODES    6
#define SYNTH_SCOPE_SIZE   4096

typedef enum SynthWave {
    SYNTH_WAVE_SINE = 0,
    SYNTH_WAVE_TRIANGLE,
    SYNTH_WAVE_SQUARE,
    SYNTH_WAVE_PULSE,
    SYNTH_WAVE_SAW,
    SYNTH_WAVE_COUNT
} SynthWave;

typedef enum SynthFilter {
    SYNTH_FILTER_OFF = 0,
    SYNTH_FILTER_LP,
    SYNTH_FILTER_HP,
    SYNTH_FILTER_BP,
    SYNTH_FILTER_COUNT
} SynthFilter;

typedef struct SynthMode {
    float freq;   /* Hz */
    float decay;  /* seconds to fall 60 dB */
    float gain;   /* linear */
} SynthMode;

/*
 * Everything one voice needs, flat and self-contained so it can be copied
 * through the command queue. Times are seconds, levels linear 0..1.
 *
 *   source = amp_env * (osc * osc_level + noise * noise_level)
 *   out    = filter(source * dry + modes(source * excite) * modes_level)
 *            -> drive -> gain -> pan
 */
typedef struct SynthVoiceDesc {
    /* oscillator */
    uint8_t wave;          /* SynthWave */
    float   freq;          /* Hz */
    float   duty;          /* pulse width 0.05..0.95 (PULSE only) */
    float   osc_level;

    /* pitch envelope: starts at freq * pitch_start, glides to freq */
    float   pitch_start;   /* 1 = off, >1 falls, <1 rises */
    float   pitch_time;    /* glide time */

    /* vibrato */
    float   lfo_rate;      /* Hz */
    float   lfo_depth;     /* semitones */

    /* noise; grain_rate > 0 chops it into random bursts */
    float   noise_level;
    float   noise_color;   /* 0 white (bright) .. 1 dark rumble */
    float   grain_rate;    /* grains per second */
    float   grain_len;     /* grain decay time */

    /* amplitude envelope */
    float   attack, decay, sustain, release;
    float   hold;          /* gate time before release; < 0 = until synth_release */
    float   delay;         /* start offset, for layering / heel-toe */

    /* modal resonator bank, excited by the enveloped source */
    uint8_t   mode_count;
    SynthMode modes[SYNTH_MAX_MODES];
    float     excite;      /* source -> resonators */
    float     dry;         /* source -> output directly */
    float     modes_level; /* resonators -> output */

    /* state-variable filter on the voice output */
    uint8_t filter;        /* SynthFilter */
    float   cutoff;        /* Hz */
    float   q;             /* 0.5 .. 20 */
    float   filter_env;    /* octaves added at note start, decaying */
    float   filter_decay;

    /* output */
    float   drive;         /* 0 clean .. 1 heavy tanh saturation */
    float   gain;
    float   pan;           /* -1 left .. 1 right */

    /* variation: +-jitter fraction on pitch, modes, cutoff, gain */
    float    jitter;
    uint32_t seed;         /* 0 = derive from handle */
} SynthVoiceDesc;

typedef uint32_t SynthHandle;  /* 0 = invalid / queue full */

typedef struct Synth Synth;

size_t synth_size(void);
void   synth_init(Synth *s, uint32_t sample_rate);
void   synth_voice_desc_default(SynthVoiceDesc *d);

/* game thread */
SynthHandle synth_play(Synth *s, const SynthVoiceDesc *d);
/* re-apply continuous params (pitch, levels, filter, modes...) without retriggering */
void        synth_update(Synth *s, SynthHandle h, const SynthVoiceDesc *d);
void        synth_release(Synth *s, SynthHandle h);
void        synth_stop_all(Synth *s);
void        synth_set_master(Synth *s, float gain);
float       synth_master(const Synth *s);

/* any thread */
uint32_t synth_active_voices(const Synth *s);
uint32_t synth_sample_rate(const Synth *s);
/* copy the most recent `count` mono output samples (oldest first); returns count copied */
uint32_t synth_scope_read(const Synth *s, float *out, uint32_t count);

/* audio thread: interleaved stereo float */
void synth_render(Synth *s, float *out, uint32_t frames);

#endif
