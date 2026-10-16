#ifndef SERVICES_AUDIO_SYNTH_LUA_H
#define SERVICES_AUDIO_SYNTH_LUA_H

#include "script_host.h"
#include "services/audio/synth.h"

/* registers gramarye.audio.* against the given synth */
void synth_lua_register(ScriptHost *host, Synth *synth);

#endif
