#ifndef SERVICES_AUDIO_DEVICE_H
#define SERVICES_AUDIO_DEVICE_H

#include <stdbool.h>
#include "arena.h"
#include "services/audio/synth.h"

/* custom UI node kinds (see GramaryeUI_set_custom_draw in main.c) */
#define AUDIO_SCOPE_KIND    5
#define AUDIO_SPECTRUM_KIND 6

/* Opens the raylib audio device and a callback stream feeding the synth.
 * The synth is always allocated, so callers never need a NULL check; if the
 * device fails, audio_device_ready() is false and commands simply queue. */
void   audio_device_init(Arena_T arena);
void   audio_device_shutdown(void);
bool   audio_device_ready(void);
Synth *audio_device_synth(void);

/* debug views of the synth's output, drawn inside a UI rect */
void audio_device_draw_scope(float x, float y, float w, float h);
void audio_device_draw_spectrum(float x, float y, float w, float h);

#endif
