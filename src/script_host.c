#include "script_host.h"
#include <stdint.h>
#include "scene.h"
#include "raylib.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

#if defined(__ANDROID__)
#define ASSET_PREFIX ""
#else
#define ASSET_PREFIX "assets/"
#endif

#define SCRIPT_HOST_MAX_SYSTEMS 32
#define SCRIPT_HOST_NAME_MAX 64
#define SCRIPT_HOST_MAX_STACK 8

static const char *HOST_REGISTRY_KEY = "gramarye.host";
static const char *LOADED_REGISTRY_KEY = "gramarye.loaded";

typedef struct {
    char name[SCRIPT_HOST_NAME_MAX];
    SystemId id;
} RegisteredSystem;

typedef struct SceneEntry {
    char name[SCRIPT_HOST_NAME_MAX];
    int32_t scene_ref;
    int32_t update_ref;
    int32_t draw_ref;
    Scene scene;
} SceneEntry;

typedef enum {
    PENDING_NONE = 0,
    PENDING_CHANGE,
    PENDING_PUSH,
    PENDING_POP
} PendingOp;

struct ScriptHost {
    Arena_T arena;
    Arena_T scene_arena;
    ECS *ecs;
    GlobalState *global_state;
    lua_State *L;

    SceneEntry stack[SCRIPT_HOST_MAX_STACK];
    size_t depth;

    PendingOp pending;
    char pending_scene[SCRIPT_HOST_NAME_MAX];

    RegisteredSystem systems[SCRIPT_HOST_MAX_SYSTEMS];
    size_t system_count;

    bool script_error;
    char error_message[1024];
};

static void set_error(ScriptHost *host, const char *msg) {
    host->script_error = true;
    snprintf(host->error_message, sizeof(host->error_message), "%s", msg ? msg : "(unknown lua error)");
    TraceLog(LOG_ERROR, "SCRIPT: %s", host->error_message);
}

static int l_traceback(lua_State *L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

static ScriptHost *host_from(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, HOST_REGISTRY_KEY);
    ScriptHost *host = (ScriptHost *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return host;
}

static SceneEntry *top(ScriptHost *host) {
    return host->depth > 0 ? &host->stack[host->depth - 1] : NULL;
}

static void pcall_hook(ScriptHost *host, int32_t nargs) {
    lua_State *L = host->L;
    int32_t func_idx = lua_gettop(L) - nargs;
    lua_pushcfunction(L, l_traceback);
    lua_insert(L, func_idx);
    if (lua_pcall(L, nargs, 0, func_idx) != LUA_OK) {
        set_error(host, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_remove(L, func_idx);
}

static void call_hook0(ScriptHost *host, SceneEntry *entry, const char *hook) {
    if (!entry || entry->scene_ref == LUA_NOREF || host->script_error) return;
    lua_rawgeti(host->L, LUA_REGISTRYINDEX, entry->scene_ref);
    lua_getfield(host->L, -1, hook);
    lua_remove(host->L, -2);
    if (!lua_isfunction(host->L, -1)) {
        lua_pop(host->L, 1);
        return;
    }
    pcall_hook(host, 0);
}

static bool run_file(ScriptHost *host, const char *path) {
    lua_State *L = host->L;
    char *text = LoadFileText(path);
    if (!text) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot read script: %s", path);
        set_error(host, msg);
        return false;
    }
    char chunkname[224];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);
    int32_t rc = luaL_loadbuffer(L, text, strlen(text), chunkname);
    UnloadFileText(text);
    if (rc != LUA_OK) {
        set_error(host, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    lua_pushcfunction(L, l_traceback);
    lua_insert(L, -2);
    if (lua_pcall(L, 0, 1, -2) != LUA_OK) {
        set_error(host, lua_tostring(L, -1));
        lua_pop(L, 2);
        return false;
    }
    lua_remove(L, -2);
    return true;
}

static int32_t cache_hook_ref(lua_State *L, const char *hook) {
    lua_getfield(L, -1, hook);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void entry_unref(ScriptHost *host, SceneEntry *entry) {
    lua_State *L = host->L;
    if (entry->scene_ref != LUA_NOREF)  luaL_unref(L, LUA_REGISTRYINDEX, entry->scene_ref);
    if (entry->update_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, entry->update_ref);
    if (entry->draw_ref != LUA_NOREF)   luaL_unref(L, LUA_REGISTRYINDEX, entry->draw_ref);
    entry->scene_ref = entry->update_ref = entry->draw_ref = LUA_NOREF;
    entry->name[0] = '\0';
}

static bool load_into_entry(ScriptHost *host, SceneEntry *entry, const char *name) {
    lua_State *L = host->L;
    entry->scene_ref = entry->update_ref = entry->draw_ref = LUA_NOREF;

    char path[192];
    snprintf(path, sizeof(path), "%sscripts/scenes/%s.lua", ASSET_PREFIX, name);
    if (!run_file(host, path)) return false;

    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        char msg[160];
        snprintf(msg, sizeof(msg), "scene '%s' must return a table of hooks", name);
        set_error(host, msg);
        return false;
    }

    snprintf(entry->name, sizeof(entry->name), "%s", name);
    Scene_init(&entry->scene, entry->name);

    entry->update_ref = cache_hook_ref(L, "on_update");
    entry->draw_ref = cache_hook_ref(L, "on_draw");
    entry->scene_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

static bool do_change(ScriptHost *host, const char *name) {
    while (host->depth > 0) {
        SceneEntry *e = top(host);
        call_hook0(host, e, "on_exit");
        entry_unref(host, e);
        host->depth--;
    }
    host->script_error = false;
    host->error_message[0] = '\0';
    Arena_free(host->scene_arena);

    SceneEntry *entry = &host->stack[0];
    if (!load_into_entry(host, entry, name)) return false;
    host->depth = 1;
    call_hook0(host, entry, "on_enter");
    return !host->script_error;
}

static bool do_push(ScriptHost *host, const char *name) {
    if (host->depth >= SCRIPT_HOST_MAX_STACK) {
        TraceLog(LOG_WARNING, "SCRIPT: scene stack full (%d), ignoring push('%s')",
                 SCRIPT_HOST_MAX_STACK, name);
        return false;
    }
    SceneEntry *below = top(host);
    call_hook0(host, below, "on_pause");

    SceneEntry *entry = &host->stack[host->depth];
    if (!load_into_entry(host, entry, name)) {
        call_hook0(host, below, "on_resume");
        return false;
    }
    host->depth++;
    call_hook0(host, entry, "on_enter");
    return !host->script_error;
}

static bool do_pop(ScriptHost *host) {
    if (host->depth <= 1) {
        TraceLog(LOG_WARNING, "SCRIPT: cannot pop the last scene");
        return false;
    }
    SceneEntry *e = top(host);
    call_hook0(host, e, "on_exit");
    entry_unref(host, e);
    host->depth--;
    host->script_error = false;
    host->error_message[0] = '\0';
    call_hook0(host, top(host), "on_resume");
    return !host->script_error;
}

static int l_log(lua_State *L) {
    TraceLog(LOG_INFO, "LUA: %s", luaL_checkstring(L, 1));
    return 0;
}

static int l_require(lua_State *L) {
    const char *mod = luaL_checkstring(L, 1);

    if (strncmp(mod, "gramarye.", 9) == 0) {
        lua_getglobal(L, "require");
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        return 1;
    }

    lua_getfield(L, LUA_REGISTRYINDEX, LOADED_REGISTRY_KEY);
    lua_getfield(L, -1, mod);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 1);

    char path[192];
    snprintf(path, sizeof(path), "%sscripts/%s.lua", ASSET_PREFIX, mod);
    char *text = LoadFileText(path);
    if (!text) return luaL_error(L, "module '%s' not found (%s)", mod, path);
    char chunkname[224];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);
    int32_t rc = luaL_loadbuffer(L, text, strlen(text), chunkname);
    UnloadFileText(text);
    if (rc != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, mod);
    lua_remove(L, -2);
    return 1;
}

static int l_scene_change(lua_State *L) {
    ScriptHost_request_scene(host_from(L), luaL_checkstring(L, 1));
    return 0;
}

static int l_scene_push(lua_State *L) {
    ScriptHost_request_push(host_from(L), luaL_checkstring(L, 1));
    return 0;
}

static int l_scene_pop(lua_State *L) {
    ScriptHost_request_pop(host_from(L));
    return 0;
}

static int l_scene_current(lua_State *L) {
    SceneEntry *e = top(host_from(L));
    lua_pushstring(L, e ? e->name : "");
    return 1;
}

static int l_scene_depth(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)host_from(L)->depth);
    return 1;
}

static RegisteredSystem *find_system(ScriptHost *host, const char *name) {
    for (size_t i = 0; i < host->system_count; i++) {
        if (strcmp(host->systems[i].name, name) == 0) return &host->systems[i];
    }
    return NULL;
}

static int l_systems_add(lua_State *L) {
    ScriptHost *host = host_from(L);
    const char *name = luaL_checkstring(L, 1);
    RegisteredSystem *sys = find_system(host, name);
    if (!sys) return luaL_error(L, "unknown system '%s' (register it from C first)", name);
    SceneEntry *e = top(host);
    if (!e) return luaL_error(L, "systems.add('%s') with no active scene", name);
    Scene_add_system(&e->scene, sys->id);
    return 0;
}

static int l_systems_enable(lua_State *L) {
    ScriptHost *host = host_from(L);
    const char *name = luaL_checkstring(L, 1);
    RegisteredSystem *sys = find_system(host, name);
    if (!sys) return luaL_error(L, "unknown system '%s'", name);
    ECS_set_system_enabled(host->ecs, sys->id, lua_toboolean(L, 2));
    return 0;
}

static int32_t key_from_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return KEY_ZERO + (c - '0');
        return 0;
    }
    static const struct { const char *n; int32_t k; } named[] = {
        {"space", KEY_SPACE}, {"enter", KEY_ENTER}, {"escape", KEY_ESCAPE},
        {"tab", KEY_TAB}, {"backspace", KEY_BACKSPACE},
        {"left", KEY_LEFT}, {"right", KEY_RIGHT}, {"up", KEY_UP}, {"down", KEY_DOWN},
        {"lshift", KEY_LEFT_SHIFT}, {"rshift", KEY_RIGHT_SHIFT},
        {"lctrl", KEY_LEFT_CONTROL}, {"rctrl", KEY_RIGHT_CONTROL},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strcmp(named[i].n, name) == 0) return named[i].k;
    }
    return 0;
}

static int32_t checkkey(lua_State *L) {
    if (lua_type(L, 1) == LUA_TNUMBER) return (int32_t)lua_tointeger(L, 1);
    const char *name = luaL_checkstring(L, 1);
    int32_t key = key_from_name(name);
    if (key == 0) return luaL_error(L, "unknown key name '%s'", name);
    return key;
}

static int l_input_key(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int32_t key = key_from_name(name);
    if (key == 0) return luaL_error(L, "unknown key name '%s'", name);
    lua_pushinteger(L, key);
    return 1;
}

static int l_input_key_pressed(lua_State *L) {
    lua_pushboolean(L, IsKeyPressed(checkkey(L)));
    return 1;
}

static int l_input_key_down(lua_State *L) {
    lua_pushboolean(L, IsKeyDown(checkkey(L)));
    return 1;
}

static int l_input_mouse_pressed(lua_State *L) {
    int32_t button = (int32_t)luaL_optinteger(L, 1, MOUSE_BUTTON_LEFT);
    lua_pushboolean(L, IsMouseButtonPressed(button));
    return 1;
}

static int l_input_mouse_pos(lua_State *L) {
    Vector2 p = GetMousePosition();
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    return 2;
}

static int l_input_touch_pos(lua_State *L) {
    Vector2 p = GetTouchPosition(0);
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    return 2;
}

static Color opt_color(lua_State *L, int32_t idx, Color def) {
    if (lua_gettop(L) < idx) return def;
    Color c;
    c.r = (uint8_t)luaL_checkinteger(L, idx);
    c.g = (uint8_t)luaL_checkinteger(L, idx + 1);
    c.b = (uint8_t)luaL_checkinteger(L, idx + 2);
    c.a = (uint8_t)luaL_optinteger(L, idx + 3, 255);
    return c;
}

static int l_draw_text(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2);
    int32_t y = (int32_t)luaL_checkinteger(L, 3);
    int32_t size = (int32_t)luaL_checkinteger(L, 4);
    DrawText(text, x, y, size, opt_color(L, 5, RAYWHITE));
    return 0;
}

static int l_draw_rect(lua_State *L) {
    int32_t x = (int32_t)luaL_checkinteger(L, 1);
    int32_t y = (int32_t)luaL_checkinteger(L, 2);
    int32_t w = (int32_t)luaL_checkinteger(L, 3);
    int32_t h = (int32_t)luaL_checkinteger(L, 4);
    DrawRectangle(x, y, w, h, opt_color(L, 5, RAYWHITE));
    return 0;
}

static int l_draw_clear(lua_State *L) {
    ClearBackground(opt_color(L, 1, BLACK));
    return 0;
}

static int l_time_total(lua_State *L) {
    lua_pushnumber(L, host_from(L)->global_state->total_time);
    return 1;
}

static int l_time_frames(lua_State *L) {
    lua_pushinteger(L, host_from(L)->global_state->frame_count);
    return 1;
}

#define DEMO_ROW_COUNT 500

static int l_demo_count(lua_State *L) {
    lua_pushinteger(L, DEMO_ROW_COUNT);
    return 1;
}

static int l_demo_row(lua_State *L) {
    int32_t i = (int32_t)luaL_checkinteger(L, 1);
    if (i < 1 || i > DEMO_ROW_COUNT) { lua_pushnil(L); lua_pushnil(L); return 2; }
    char name[32], value[16];
    snprintf(name, sizeof(name), "Unit %03d", i);
    snprintf(value, sizeof(value), "%d hp", (i * 37) % 100 + 1);
    lua_pushstring(L, name);
    lua_pushstring(L, value);
    return 2;
}

static void install_module(lua_State *L, const char *module, const luaL_Reg *fns) {
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, module);
}

static void install_bindings(lua_State *L) {
    static const luaL_Reg root_fns[] = {
        {"log", l_log},
        {"require", l_require},
        {NULL, NULL}
    };
    static const luaL_Reg scene_fns[] = {
        {"change", l_scene_change},
        {"push", l_scene_push},
        {"pop", l_scene_pop},
        {"current", l_scene_current},
        {"depth", l_scene_depth},
        {NULL, NULL}
    };
    static const luaL_Reg systems_fns[] = {
        {"add", l_systems_add},
        {"enable", l_systems_enable},
        {NULL, NULL}
    };
    static const luaL_Reg input_fns[] = {
        {"key", l_input_key},
        {"key_pressed", l_input_key_pressed},
        {"key_down", l_input_key_down},
        {"mouse_pressed", l_input_mouse_pressed},
        {"mouse_pos", l_input_mouse_pos},
        {"touch_pos", l_input_touch_pos},
        {NULL, NULL}
    };
    static const luaL_Reg draw_fns[] = {
        {"text", l_draw_text},
        {"rect", l_draw_rect},
        {"clear", l_draw_clear},
        {NULL, NULL}
    };
    static const luaL_Reg time_fns[] = {
        {"total", l_time_total},
        {"frames", l_time_frames},
        {NULL, NULL}
    };
    static const luaL_Reg demo_fns[] = {
        {"count", l_demo_count},
        {"row", l_demo_row},
        {NULL, NULL}
    };

    lua_newtable(L);
    luaL_setfuncs(L, root_fns, 0);
    lua_pushstring(L, ASSET_PREFIX);
    lua_setfield(L, -2, "asset_prefix");
    install_module(L, "scene", scene_fns);
    install_module(L, "systems", systems_fns);
    install_module(L, "input", input_fns);
    install_module(L, "draw", draw_fns);
    install_module(L, "time", time_fns);
    install_module(L, "demo", demo_fns);
    lua_setglobal(L, "gramarye");
}

ScriptHost *ScriptHost_new(Arena_T arena, ECS *ecs, GlobalState *global_state) {
    if (!arena || !ecs) return NULL;
    ScriptHost *host = (ScriptHost *)Arena_alloc(arena, sizeof(ScriptHost), __FILE__, __LINE__);
    if (!host) return NULL;
    memset(host, 0, sizeof(*host));
    host->arena = arena;
    host->scene_arena = Arena_new();
    host->ecs = ecs;
    host->global_state = global_state;
    if (global_state) global_state->scene_arena = host->scene_arena;

    host->L = luaL_newstate();
    luaL_openlibs(host->L);
    lua_gc(host->L, LUA_GCGEN, 0, 0);

    lua_pushlightuserdata(host->L, host);
    lua_setfield(host->L, LUA_REGISTRYINDEX, HOST_REGISTRY_KEY);
    lua_newtable(host->L);
    lua_setfield(host->L, LUA_REGISTRYINDEX, LOADED_REGISTRY_KEY);

    install_bindings(host->L);
    return host;
}

void ScriptHost_dispose(ScriptHost *host) {
    if (!host) return;
    if (host->L) {
        lua_close(host->L);
        host->L = NULL;
    }
    if (host->scene_arena) {
        Arena_dispose(&host->scene_arena);
    }
}

Arena_T ScriptHost_scene_arena(ScriptHost *host) {
    return host ? host->scene_arena : NULL;
}

void ScriptHost_register_system(ScriptHost *host, const char *name, SystemId id) {
    if (!host || !name || id == SYSTEM_INVALID) return;
    if (host->system_count >= SCRIPT_HOST_MAX_SYSTEMS) {
        TraceLog(LOG_WARNING, "SCRIPT: system registry full, dropping '%s'", name);
        return;
    }
    RegisteredSystem *slot = &host->systems[host->system_count++];
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->id = id;
}

void ScriptHost_register_function(ScriptHost *host, const char *module,
                                  const char *name, int (*fn)(lua_State *)) {
    if (!host || !name || !fn) return;
    lua_State *L = host->L;
    lua_getglobal(L, "gramarye");
    if (module && module[0]) {
        lua_getfield(L, -1, module);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, module);
        }
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
        lua_pop(L, 2);
    } else {
        lua_pushcfunction(L, fn);
        lua_setfield(L, -2, name);
        lua_pop(L, 1);
    }
}

bool ScriptHost_load_scene(ScriptHost *host, const char *scene_name) {
    if (!host || !scene_name) return false;
    return do_change(host, scene_name);
}

void ScriptHost_request_scene(ScriptHost *host, const char *scene_name) {
    if (!host || !scene_name) return;
    snprintf(host->pending_scene, sizeof(host->pending_scene), "%s", scene_name);
    host->pending = PENDING_CHANGE;
}

void ScriptHost_request_push(ScriptHost *host, const char *scene_name) {
    if (!host || !scene_name) return;
    snprintf(host->pending_scene, sizeof(host->pending_scene), "%s", scene_name);
    host->pending = PENDING_PUSH;
}

void ScriptHost_request_pop(ScriptHost *host) {
    if (!host) return;
    host->pending = PENDING_POP;
}

bool ScriptHost_reload_current(ScriptHost *host) {
    SceneEntry *e = host ? top(host) : NULL;
    if (!e) return false;
    TraceLog(LOG_INFO, "SCRIPT: reloading scene '%s'", e->name);

    lua_newtable(host->L);
    lua_setfield(host->L, LUA_REGISTRYINDEX, LOADED_REGISTRY_KEY);

    char name[SCRIPT_HOST_NAME_MAX];
    snprintf(name, sizeof(name), "%s", e->name);

    call_hook0(host, e, "on_exit");
    entry_unref(host, e);
    host->script_error = false;
    host->error_message[0] = '\0';

    if (!load_into_entry(host, e, name)) return false;
    call_hook0(host, e, "on_enter");
    return !host->script_error;
}

void ScriptHost_update_fixed(ScriptHost *host, float dt) {
    if (!host) return;
    SceneEntry *e = top(host);
    if (e) Scene_run(&e->scene, host->ecs, dt);
}

void ScriptHost_update(ScriptHost *host, float dt) {
    if (!host) return;
    SceneEntry *e = top(host);
    if (e && e->update_ref != LUA_NOREF && !host->script_error) {
        lua_rawgeti(host->L, LUA_REGISTRYINDEX, e->update_ref);
        lua_pushnumber(host->L, dt);
        pcall_hook(host, 1);
    }
    if (host->pending != PENDING_NONE) {
        PendingOp op = host->pending;
        host->pending = PENDING_NONE;
        char next[SCRIPT_HOST_NAME_MAX];
        snprintf(next, sizeof(next), "%s", host->pending_scene);
        switch (op) {
            case PENDING_CHANGE: do_change(host, next); break;
            case PENDING_PUSH:   do_push(host, next);   break;
            case PENDING_POP:    do_pop(host);          break;
            default: break;
        }
    }
}

static void draw_wrapped_text(const char *text, int32_t x, int32_t y, int32_t font_size, int32_t max_w) {
    char line[256];
    int32_t line_h = font_size + 4;
    const char *p = text;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n < sizeof(line) - 1) {
            line[n] = p[n];
            n++;
            line[n] = '\0';
            if (MeasureText(line, font_size) > max_w && n > 1) {
                n--;
                line[n] = '\0';
                break;
            }
        }
        DrawText(line, x, y, font_size, (Color){ 255, 200, 200, 255 });
        y += line_h;
        p += n;
        if (*p == '\n') p++;
    }
}

void ScriptHost_draw(ScriptHost *host) {
    if (!host) return;
    if (host->script_error) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 80, 8, 8, 255 });
        DrawText("Lua error (F5 to reload on desktop):", 16, 16, 20, RAYWHITE);
        draw_wrapped_text(host->error_message, 16, 48, 12, GetScreenWidth() - 32);
        return;
    }
    SceneEntry *e = top(host);
    if (e && e->draw_ref != LUA_NOREF) {
        lua_rawgeti(host->L, LUA_REGISTRYINDEX, e->draw_ref);
        pcall_hook(host, 0);
    }
}

lua_State *ScriptHost_state(ScriptHost *host) {
    return host ? host->L : NULL;
}
