#include "script_host.h"
#include "scene.h"
#include "raylib.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

// Android APK asset paths are relative to the assets root; everywhere else
// assets/ sits next to the binary (copied by CMake, preloaded on web).
#if defined(__ANDROID__)
#define ASSET_PREFIX ""
#else
#define ASSET_PREFIX "assets/"
#endif

#define SCRIPT_HOST_MAX_SYSTEMS 32
#define SCRIPT_HOST_NAME_MAX 64

static const char *HOST_REGISTRY_KEY = "gramarye.host";
static const char *LOADED_REGISTRY_KEY = "gramarye.loaded";

typedef struct {
    char name[SCRIPT_HOST_NAME_MAX];
    SystemId id;
} RegisteredSystem;

struct ScriptHost {
    Arena_T arena;
    ECS *ecs;
    GlobalState *global_state;
    lua_State *L;

    Scene *active_scene;
    int scene_ref;
    char current_scene[SCRIPT_HOST_NAME_MAX];
    char pending_scene[SCRIPT_HOST_NAME_MAX];
    bool has_pending;

    RegisteredSystem systems[SCRIPT_HOST_MAX_SYSTEMS];
    size_t system_count;

    bool script_error;
    char error_message[1024];
};

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Hook dispatch
// ---------------------------------------------------------------------------

static bool push_hook(ScriptHost *host, const char *hook) {
    if (host->scene_ref == LUA_NOREF || host->script_error) return false;
    lua_rawgeti(host->L, LUA_REGISTRYINDEX, host->scene_ref);
    lua_getfield(host->L, -1, hook);
    lua_remove(host->L, -2);
    if (!lua_isfunction(host->L, -1)) {
        lua_pop(host->L, 1);
        return false;
    }
    return true;
}

// Expects the function and its nargs arguments on top of the stack.
static void pcall_hook(ScriptHost *host, int nargs) {
    lua_State *L = host->L;
    int func_idx = lua_gettop(L) - nargs;
    lua_pushcfunction(L, l_traceback);
    lua_insert(L, func_idx);
    if (lua_pcall(L, nargs, 0, func_idx) != LUA_OK) {
        set_error(host, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_remove(L, func_idx);
}

static void call_hook0(ScriptHost *host, const char *hook) {
    if (push_hook(host, hook)) pcall_hook(host, 0);
}

// ---------------------------------------------------------------------------
// Chunk loading (raylib file IO -> works on desktop, web MEMFS, android APK)
// ---------------------------------------------------------------------------

// Loads and runs <path>, leaving its single return value on the stack.
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
    int rc = luaL_loadbuffer(L, text, strlen(text), chunkname);
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

static bool do_load_scene(ScriptHost *host, const char *name) {
    lua_State *L = host->L;

    call_hook0(host, "on_exit");
    if (host->scene_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, host->scene_ref);
        host->scene_ref = LUA_NOREF;
    }
    host->script_error = false;
    host->error_message[0] = '\0';

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

    snprintf(host->current_scene, sizeof(host->current_scene), "%s", name);
    // Scene keeps the name pointer; copy it into arena memory that outlives us.
    size_t len = strlen(host->current_scene) + 1;
    char *scene_name = (char *)Arena_alloc(host->arena, len, __FILE__, __LINE__);
    memcpy(scene_name, host->current_scene, len);
    host->active_scene = Scene_new(host->arena, scene_name);

    host->scene_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    call_hook0(host, "on_enter");
    return !host->script_error;
}

// ---------------------------------------------------------------------------
// Lua bindings: the `gramarye` table (orchestration-level API only)
// ---------------------------------------------------------------------------

static int l_log(lua_State *L) {
    TraceLog(LOG_INFO, "LUA: %s", luaL_checkstring(L, 1));
    return 0;
}

static int l_require(lua_State *L) {
    const char *mod = luaL_checkstring(L, 1);
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
    int rc = luaL_loadbuffer(L, text, strlen(text), chunkname);
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
    ScriptHost *host = host_from(L);
    ScriptHost_request_scene(host, luaL_checkstring(L, 1));
    return 0;
}

static int l_scene_current(lua_State *L) {
    lua_pushstring(L, host_from(L)->current_scene);
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
    Scene_add_system(host->active_scene, sys->id);
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

static int key_from_name(const char *name) {
    if (!name || !name[0]) return 0;
    if (!name[1]) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return KEY_ZERO + (c - '0');
        return 0;
    }
    static const struct { const char *n; int k; } named[] = {
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

static int checkkey(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int key = key_from_name(name);
    if (key == 0) return luaL_error(L, "unknown key name '%s'", name);
    return key;
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
    int button = (int)luaL_optinteger(L, 1, MOUSE_BUTTON_LEFT);
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

static Color opt_color(lua_State *L, int idx, Color def) {
    if (lua_gettop(L) < idx) return def;
    Color c;
    c.r = (unsigned char)luaL_checkinteger(L, idx);
    c.g = (unsigned char)luaL_checkinteger(L, idx + 1);
    c.b = (unsigned char)luaL_checkinteger(L, idx + 2);
    c.a = (unsigned char)luaL_optinteger(L, idx + 3, 255);
    return c;
}

static int l_draw_text(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int size = (int)luaL_checkinteger(L, 4);
    DrawText(text, x, y, size, opt_color(L, 5, RAYWHITE));
    return 0;
}

static int l_draw_rect(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
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

static void install_module(lua_State *L, const char *module, const luaL_Reg *fns) {
    // gramarye table on stack top
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
        {"current", l_scene_current},
        {NULL, NULL}
    };
    static const luaL_Reg systems_fns[] = {
        {"add", l_systems_add},
        {"enable", l_systems_enable},
        {NULL, NULL}
    };
    static const luaL_Reg input_fns[] = {
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

    lua_newtable(L);
    luaL_setfuncs(L, root_fns, 0);
    install_module(L, "scene", scene_fns);
    install_module(L, "systems", systems_fns);
    install_module(L, "input", input_fns);
    install_module(L, "draw", draw_fns);
    install_module(L, "time", time_fns);
    lua_setglobal(L, "gramarye");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ScriptHost *ScriptHost_new(Arena_T arena, ECS *ecs, GlobalState *global_state) {
    if (!arena || !ecs) return NULL;
    ScriptHost *host = (ScriptHost *)Arena_alloc(arena, sizeof(ScriptHost), __FILE__, __LINE__);
    if (!host) return NULL;
    memset(host, 0, sizeof(*host));
    host->arena = arena;
    host->ecs = ecs;
    host->global_state = global_state;
    host->scene_ref = LUA_NOREF;

    host->L = luaL_newstate();
    luaL_openlibs(host->L);

    lua_pushlightuserdata(host->L, host);
    lua_setfield(host->L, LUA_REGISTRYINDEX, HOST_REGISTRY_KEY);
    lua_newtable(host->L);
    lua_setfield(host->L, LUA_REGISTRYINDEX, LOADED_REGISTRY_KEY);

    install_bindings(host->L);
    return host;
}

void ScriptHost_dispose(ScriptHost *host) {
    if (!host || !host->L) return;
    lua_close(host->L);
    host->L = NULL;
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
    return do_load_scene(host, scene_name);
}

void ScriptHost_request_scene(ScriptHost *host, const char *scene_name) {
    if (!host || !scene_name) return;
    snprintf(host->pending_scene, sizeof(host->pending_scene), "%s", scene_name);
    host->has_pending = true;
}

bool ScriptHost_reload_current(ScriptHost *host) {
    if (!host || !host->current_scene[0]) return false;
    TraceLog(LOG_INFO, "SCRIPT: reloading scene '%s'", host->current_scene);
    host->script_error = false;
    return do_load_scene(host, host->current_scene);
}

void ScriptHost_update(ScriptHost *host, float dt) {
    if (!host) return;
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    if (IsKeyPressed(KEY_F5)) ScriptHost_reload_current(host);
#endif
    if (host->active_scene) Scene_run(host->active_scene, host->ecs, dt);
    if (push_hook(host, "on_update")) {
        lua_pushnumber(host->L, dt);
        pcall_hook(host, 1);
    }
    if (host->has_pending) {
        host->has_pending = false;
        char next[SCRIPT_HOST_NAME_MAX];
        snprintf(next, sizeof(next), "%s", host->pending_scene);
        do_load_scene(host, next);
    }
}

void ScriptHost_draw(ScriptHost *host) {
    if (!host) return;
    if (host->script_error) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){ 80, 8, 8, 255 });
        DrawText("Lua error (F5 to reload on desktop):", 16, 16, 20, RAYWHITE);
        DrawText(host->error_message, 16, 48, 10, (Color){ 255, 200, 200, 255 });
        return;
    }
    call_hook0(host, "on_draw");
}

lua_State *ScriptHost_state(ScriptHost *host) {
    return host ? host->L : NULL;
}
