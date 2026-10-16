#include "services/audio/audio_device.h"

#include <math.h>
#include <stdint.h>
#include "raylib.h"

#define SCOPE_WINDOW   1024          /* samples shown (~21 ms at 48 kHz) */
#define FFT_SIZE       2048
#define FFT_BINS       (FFT_SIZE / 2)
#define SPEC_MIN_HZ    20.0f
#define SPEC_MAX_HZ    20000.0f
#define SPEC_FLOOR_DB  -90.0f
#define SPEC_FALL      0.85f         /* per-frame decay of the displayed peaks */

static Synth      *g_synth;
static AudioStream g_stream;
static bool        g_ready;

/* game-thread scratch for the debug views */
static float   g_samples[SYNTH_SCOPE_SIZE];
static float   g_re[FFT_SIZE], g_im[FFT_SIZE], g_window[FFT_SIZE];
static float   g_spec[FFT_BINS];
static Vector2 g_points[FFT_BINS];
static bool    g_window_ready;

/* raylib's callback carries no user pointer, hence the static synth */
static void audio_callback(void *buffer, unsigned int frames) {
    synth_render(g_synth, (float *)buffer, (uint32_t)frames);
}

void audio_device_init(Arena_T arena) {
    g_synth = (Synth *)Arena_alloc(arena, (long)synth_size(), __FILE__, __LINE__);
    synth_init(g_synth, SYNTH_SAMPLE_RATE);

    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        TraceLog(LOG_WARNING, "audio: device unavailable; synth is silent");
        return;
    }
    g_stream = LoadAudioStream(SYNTH_SAMPLE_RATE, 32, 2);
    if (!IsAudioStreamValid(g_stream)) {
        TraceLog(LOG_WARNING, "audio: stream creation failed; synth is silent");
        CloseAudioDevice();
        return;
    }
    SetAudioStreamCallback(g_stream, audio_callback);
    PlayAudioStream(g_stream);
    g_ready = true;
}

void audio_device_shutdown(void) {
    if (g_ready) {
        StopAudioStream(g_stream);
        UnloadAudioStream(g_stream);
        CloseAudioDevice();
    }
    g_ready = false;
    g_synth = NULL;
}

bool   audio_device_ready(void) { return g_ready; }
Synth *audio_device_synth(void) { return g_synth; }

/* ---- debug views ----------------------------------------------------------- */

static const Color VIEW_BG    = { 12, 14, 22, 255 };
static const Color VIEW_GRID  = { 40, 46, 64, 255 };
static const Color VIEW_LABEL = { 110, 120, 145, 255 };
static const Color SCOPE_LINE = { 120, 220, 160, 255 };
static const Color SPEC_LINE  = { 110, 160, 255, 255 };

void audio_device_draw_scope(float x, float y, float w, float h) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, VIEW_BG);
    float mid = y + h * 0.5f;
    DrawLine((int)x, (int)mid, (int)(x + w), (int)mid, VIEW_GRID);
    if (!g_synth || w < 2.0f) return;

    uint32_t n = synth_scope_read(g_synth, g_samples, SYNTH_SCOPE_SIZE);

    /* trigger on the latest rising zero crossing that still leaves a full
     * window after it, so periodic waves stand still on screen */
    uint32_t start = n - SCOPE_WINDOW;
    for (uint32_t i = n - SCOPE_WINDOW; i > 1; i--) {
        if (g_samples[i - 1] < 0.0f && g_samples[i] >= 0.0f) { start = i; break; }
    }

    int32_t count = (int32_t)w;
    if (count > FFT_BINS) count = FFT_BINS;
    float half = h * 0.45f;
    for (int32_t i = 0; i < count; i++) {
        uint32_t idx = start + (uint32_t)((float)i / (float)(count - 1) * (SCOPE_WINDOW - 1));
        float s = g_samples[idx];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        g_points[i] = (Vector2){ x + (float)i * w / (float)(count - 1), mid - s * half };
    }
    DrawLineStrip(g_points, count, SCOPE_LINE);
    DrawText("scope  ~21 ms", (int)x + 6, (int)y + 4, 10, VIEW_LABEL);
}

static void fft(float *re, float *im, uint32_t n) {
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        float ang = -6.28318530718f / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (uint32_t i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (uint32_t k = 0; k < len / 2; k++) {
                uint32_t a = i + k, b = a + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

static float spec_x(float hz, float x, float w) {
    return x + logf(hz / SPEC_MIN_HZ) / logf(SPEC_MAX_HZ / SPEC_MIN_HZ) * w;
}

void audio_device_draw_spectrum(float x, float y, float w, float h) {
    DrawRectangle((int)x, (int)y, (int)w, (int)h, VIEW_BG);

    static const float marks[] = { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    static const char *labels[] = { "50", "100", "200", "500", "1k", "2k", "5k", "10k" };
    for (int32_t i = 0; i < (int32_t)(sizeof(marks) / sizeof(marks[0])); i++) {
        int32_t mx = (int32_t)spec_x(marks[i], x, w);
        DrawLine(mx, (int)y, mx, (int)(y + h), VIEW_GRID);
        DrawText(labels[i], mx + 3, (int)(y + h) - 12, 10, VIEW_LABEL);
    }
    DrawText("spectrum  (log Hz, 90 dB range)", (int)x + 6, (int)y + 4, 10, VIEW_LABEL);
    if (!g_synth) return;

    if (!g_window_ready) {
        for (uint32_t i = 0; i < FFT_SIZE; i++)
            g_window[i] = 0.5f - 0.5f * cosf(6.28318530718f * (float)i / (float)(FFT_SIZE - 1));
        g_window_ready = true;
    }
    synth_scope_read(g_synth, g_samples, FFT_SIZE);
    for (uint32_t i = 0; i < FFT_SIZE; i++) { g_re[i] = g_samples[i] * g_window[i]; g_im[i] = 0.0f; }
    fft(g_re, g_im, FFT_SIZE);

    float sr = (float)synth_sample_rate(g_synth);
    float norm = 4.0f / (float)FFT_SIZE;       /* 2/N for one-sided, x2 for Hann loss */
    int32_t count = 0;
    for (uint32_t k = 1; k < FFT_BINS; k++) {
        float hz = (float)k * sr / (float)FFT_SIZE;
        float mag = sqrtf(g_re[k] * g_re[k] + g_im[k] * g_im[k]) * norm;
        g_spec[k] = fmaxf(mag, g_spec[k] * SPEC_FALL);
        if (hz < SPEC_MIN_HZ || hz > SPEC_MAX_HZ) continue;
        float db = 20.0f * log10f(g_spec[k] + 1e-9f);
        float t = (db - SPEC_FLOOR_DB) / -SPEC_FLOOR_DB;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        g_points[count++] = (Vector2){ spec_x(hz, x, w), y + h - t * (h - 16.0f) };
    }
    if (count > 1) DrawLineStrip(g_points, count, SPEC_LINE);
}
