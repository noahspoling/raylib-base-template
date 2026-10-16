#include "services/audio/synth_lua.h"

#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "services/audio/audio_device.h"

static const char *SYNTH_REGISTRY_KEY = "gramarye.audio.synth";

static const char *WAVE_NAMES[SYNTH_WAVE_COUNT]     = { "sine", "triangle", "square", "pulse", "saw" };
static const char *FILTER_NAMES[SYNTH_FILTER_COUNT] = { "off", "lp", "hp", "bp" };

static Synth *synth_from(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, SYNTH_REGISTRY_KEY);
    Synth *s = (Synth *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return s;
}

/* ---- desc <-> table -------------------------------------------------------- */

static void opt_float(lua_State *L, int32_t idx, const char *key, float *out) {
    lua_getfield(L, idx, key);
    if (lua_isnumber(L, -1)) *out = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
}

/* accepts a name ("saw") or a 0-based enum index */
static void opt_enum(lua_State *L, int32_t idx, const char *key, const char **names,
                     uint8_t count, uint8_t *out) {
    lua_getfield(L, idx, key);
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *s = lua_tostring(L, -1);
        for (uint8_t i = 0; i < count; i++)
            if (strcmp(s, names[i]) == 0) { *out = i; break; }
    } else if (lua_isinteger(L, -1)) {
        lua_Integer v = lua_tointeger(L, -1);
        if (v >= 0 && v < count) *out = (uint8_t)v;
    }
    lua_pop(L, 1);
}

static float mode_field(lua_State *L, int32_t idx, const char *key, int32_t pos, float fallback) {
    float v = fallback;
    lua_getfield(L, idx, key);
    if (lua_isnumber(L, -1)) v = (float)lua_tonumber(L, -1);
    else {
        lua_pop(L, 1);
        lua_rawgeti(L, idx, pos);
        if (lua_isnumber(L, -1)) v = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

/* modes = { {freq=, decay=, gain=}, ... } or positional { {f, d, g}, ... } */
static void read_modes(lua_State *L, int32_t idx, SynthVoiceDesc *d) {
    lua_getfield(L, idx, "modes");
    if (lua_istable(L, -1)) {
        int32_t t = lua_gettop(L);
        int32_t n = (int32_t)luaL_len(L, t);
        if (n > SYNTH_MAX_MODES) n = SYNTH_MAX_MODES;
        d->mode_count = 0;
        for (int32_t i = 1; i <= n; i++) {
            lua_rawgeti(L, t, i);
            if (lua_istable(L, -1)) {
                int32_t m = lua_gettop(L);
                SynthMode *mode = &d->modes[d->mode_count++];
                mode->freq  = mode_field(L, m, "freq", 1, 440.0f);
                mode->decay = mode_field(L, m, "decay", 2, 0.3f);
                mode->gain  = mode_field(L, m, "gain", 3, 1.0f);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

static void read_desc(lua_State *L, int32_t idx, SynthVoiceDesc *d) {
    synth_voice_desc_default(d);
    luaL_checktype(L, idx, LUA_TTABLE);

    opt_enum(L, idx, "wave", WAVE_NAMES, SYNTH_WAVE_COUNT, &d->wave);
    opt_float(L, idx, "freq", &d->freq);
    opt_float(L, idx, "duty", &d->duty);
    opt_float(L, idx, "osc_level", &d->osc_level);
    opt_float(L, idx, "pitch_start", &d->pitch_start);
    opt_float(L, idx, "pitch_time", &d->pitch_time);
    opt_float(L, idx, "lfo_rate", &d->lfo_rate);
    opt_float(L, idx, "lfo_depth", &d->lfo_depth);
    opt_float(L, idx, "noise_level", &d->noise_level);
    opt_float(L, idx, "noise_color", &d->noise_color);
    opt_float(L, idx, "grain_rate", &d->grain_rate);
    opt_float(L, idx, "grain_len", &d->grain_len);
    opt_float(L, idx, "attack", &d->attack);
    opt_float(L, idx, "decay", &d->decay);
    opt_float(L, idx, "sustain", &d->sustain);
    opt_float(L, idx, "release", &d->release);
    opt_float(L, idx, "hold", &d->hold);
    opt_float(L, idx, "delay", &d->delay);
    read_modes(L, idx, d);
    opt_float(L, idx, "excite", &d->excite);
    opt_float(L, idx, "dry", &d->dry);
    opt_float(L, idx, "modes_level", &d->modes_level);
    opt_enum(L, idx, "filter", FILTER_NAMES, SYNTH_FILTER_COUNT, &d->filter);
    opt_float(L, idx, "cutoff", &d->cutoff);
    opt_float(L, idx, "q", &d->q);
    opt_float(L, idx, "filter_env", &d->filter_env);
    opt_float(L, idx, "filter_decay", &d->filter_decay);
    opt_float(L, idx, "drive", &d->drive);
    opt_float(L, idx, "gain", &d->gain);
    opt_float(L, idx, "pan", &d->pan);
    opt_float(L, idx, "jitter", &d->jitter);

    lua_getfield(L, idx, "seed");
    if (lua_isinteger(L, -1)) d->seed = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
}

static void set_num(lua_State *L, const char *key, float v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, key);
}

static void push_desc(lua_State *L, const SynthVoiceDesc *d) {
    lua_newtable(L);
    lua_pushstring(L, WAVE_NAMES[d->wave]);
    lua_setfield(L, -2, "wave");
    set_num(L, "freq", d->freq);
    set_num(L, "duty", d->duty);
    set_num(L, "osc_level", d->osc_level);
    set_num(L, "pitch_start", d->pitch_start);
    set_num(L, "pitch_time", d->pitch_time);
    set_num(L, "lfo_rate", d->lfo_rate);
    set_num(L, "lfo_depth", d->lfo_depth);
    set_num(L, "noise_level", d->noise_level);
    set_num(L, "noise_color", d->noise_color);
    set_num(L, "grain_rate", d->grain_rate);
    set_num(L, "grain_len", d->grain_len);
    set_num(L, "attack", d->attack);
    set_num(L, "decay", d->decay);
    set_num(L, "sustain", d->sustain);
    set_num(L, "release", d->release);
    set_num(L, "hold", d->hold);
    set_num(L, "delay", d->delay);
    lua_createtable(L, d->mode_count, 0);
    for (uint8_t i = 0; i < d->mode_count; i++) {
        lua_createtable(L, 0, 3);
        set_num(L, "freq", d->modes[i].freq);
        set_num(L, "decay", d->modes[i].decay);
        set_num(L, "gain", d->modes[i].gain);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "modes");
    set_num(L, "excite", d->excite);
    set_num(L, "dry", d->dry);
    set_num(L, "modes_level", d->modes_level);
    lua_pushstring(L, FILTER_NAMES[d->filter]);
    lua_setfield(L, -2, "filter");
    set_num(L, "cutoff", d->cutoff);
    set_num(L, "q", d->q);
    set_num(L, "filter_env", d->filter_env);
    set_num(L, "filter_decay", d->filter_decay);
    set_num(L, "drive", d->drive);
    set_num(L, "gain", d->gain);
    set_num(L, "pan", d->pan);
    set_num(L, "jitter", d->jitter);
}

static void push_names(lua_State *L, const char **names, int32_t count) {
    lua_createtable(L, count, 0);
    for (int32_t i = 0; i < count; i++) {
        lua_pushstring(L, names[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

/* ---- bindings ---------------------------------------------------------------- */

static int l_audio_play(lua_State *L) {
    Synth *s = synth_from(L);
    SynthVoiceDesc d;
    read_desc(L, 1, &d);
    SynthHandle h = s ? synth_play(s, &d) : 0;
    if (h) lua_pushinteger(L, (lua_Integer)h);
    else   lua_pushnil(L);
    return 1;
}

static int l_audio_update(lua_State *L) {
    Synth *s = synth_from(L);
    SynthHandle h = (SynthHandle)luaL_checkinteger(L, 1);
    SynthVoiceDesc d;
    read_desc(L, 2, &d);
    if (s) synth_update(s, h, &d);
    return 0;
}

static int l_audio_release(lua_State *L) {
    Synth *s = synth_from(L);
    if (s && lua_isinteger(L, 1)) synth_release(s, (SynthHandle)lua_tointeger(L, 1));
    return 0;
}

static int l_audio_stop_all(lua_State *L) {
    Synth *s = synth_from(L);
    if (s) synth_stop_all(s);
    return 0;
}

static int l_audio_set_master(lua_State *L) {
    Synth *s = synth_from(L);
    if (s) synth_set_master(s, (float)luaL_checknumber(L, 1));
    return 0;
}

static int l_audio_master(lua_State *L) {
    Synth *s = synth_from(L);
    lua_pushnumber(L, s ? synth_master(s) : 0.0);
    return 1;
}

static int l_audio_voices(lua_State *L) {
    Synth *s = synth_from(L);
    lua_pushinteger(L, s ? (lua_Integer)synth_active_voices(s) : 0);
    return 1;
}

static int l_audio_max_voices(lua_State *L) {
    lua_pushinteger(L, SYNTH_MAX_VOICES);
    return 1;
}

static int l_audio_ready(lua_State *L) {
    lua_pushboolean(L, audio_device_ready());
    return 1;
}

static int l_audio_defaults(lua_State *L) {
    SynthVoiceDesc d;
    synth_voice_desc_default(&d);
    push_desc(L, &d);
    return 1;
}

void synth_lua_register(ScriptHost *host, Synth *synth) {
    lua_State *L = ScriptHost_state(host);
    if (!L) return;

    lua_pushlightuserdata(L, synth);
    lua_setfield(L, LUA_REGISTRYINDEX, SYNTH_REGISTRY_KEY);

    ScriptHost_register_function(host, "audio", "play", l_audio_play);
    ScriptHost_register_function(host, "audio", "update", l_audio_update);
    ScriptHost_register_function(host, "audio", "release", l_audio_release);
    ScriptHost_register_function(host, "audio", "stop_all", l_audio_stop_all);
    ScriptHost_register_function(host, "audio", "set_master", l_audio_set_master);
    ScriptHost_register_function(host, "audio", "master", l_audio_master);
    ScriptHost_register_function(host, "audio", "voices", l_audio_voices);
    ScriptHost_register_function(host, "audio", "max_voices", l_audio_max_voices);
    ScriptHost_register_function(host, "audio", "ready", l_audio_ready);
    ScriptHost_register_function(host, "audio", "defaults", l_audio_defaults);

    lua_getglobal(L, "gramarye");
    lua_getfield(L, -1, "audio");
    push_names(L, WAVE_NAMES, SYNTH_WAVE_COUNT);
    lua_setfield(L, -2, "WAVES");
    push_names(L, FILTER_NAMES, SYNTH_FILTER_COUNT);
    lua_setfield(L, -2, "FILTERS");
    lua_pushinteger(L, SYNTH_MAX_MODES);
    lua_setfield(L, -2, "MAX_MODES");
    lua_pushinteger(L, AUDIO_SCOPE_KIND);
    lua_setfield(L, -2, "SCOPE_KIND");
    lua_pushinteger(L, AUDIO_SPECTRUM_KIND);
    lua_setfield(L, -2, "SPECTRUM_KIND");
    lua_pop(L, 2);
}
