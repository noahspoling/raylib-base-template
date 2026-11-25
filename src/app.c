#include "app.h"

#include "raylib.h"
#include "rlgl.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "arena.h"
#include "gramarye_ecs/ecs.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include "game_config.h"
#include "global_system.h"
#include "script_host.h"
#include "entities_lua.h"
#include "components/sprite.h"
#include "components/skinned_sprite.h"
#include "components/tile.h"
#include "services/stores/texture_store.h"
#include "services/mounts/mounts.h"
#include "services/mounts/packages.h"
#include "systems/sprite/sprite_system.h"
#include "systems/skinned_sprite/skinned_sprite_system.h"
#include "platform/spawn.h"

#include "services/world/world.h"
#include "services/world/world_lua.h"
#include "services/local_chunk/local_chunk_lua.h"
#include "services/audio/audio_device.h"
#include "services/audio/synth_lua.h"
#include "services/tilemap/tilemap_lua.h"
#if defined(GRAMARYE_DEV_TOOLS)
#include "dev/shader_playground.h"
#include "dev/file_watch.h"
#endif

#include "gramarye_ui/ui.h"
#include "gramarye_ui/ui_lua.h"

#ifndef GRAMARYE_APP_NAME
#define GRAMARYE_APP_NAME "app"
#endif

#define WORLD_START_LEVEL 4
#define UI_FONT_BASE_SIZE 48
#define SIM_DT (1.0f / 60.0f)
#define MAX_FRAME_DT 0.25f
#define APP_MAX_MODS 32
#define APP_SCENE_MAX 64

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#define APP_DESKTOP 1
#endif

#if defined(GRAMARYE_DEV_TOOLS)
#define DEV_BUILD 1
#else
#define DEV_BUILD 0
#endif

typedef struct AppState {
    const AppConfig *cfg;
    AppMode      mode;
    bool         quit;
    ScriptHost  *host;
    lua_State   *L;

    char         mods[APP_MAX_MODS][MOUNTS_ID_MAX];
    int32_t      mod_count;
    bool         mods_from_cli;
    char         launch_file[MOUNTS_PATH_MAX];
    char         cli_scene[APP_SCENE_MAX];
    char         stack_error[256];

    bool         pending_switch;
    AppMode      pending_mode;

    ChildProcess child;
    char         child_app[16];
    bool         child_reported;
} AppState;

static AppState g_app;

#if defined(APP_DESKTOP)
static void log_callback(int level, const char *text, va_list args) {
    static const char *const names[] = { "ALL", "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL" };
    const char *name = (level >= 0 && level <= LOG_FATAL) ? names[level] : "LOG";
    printf("[%s] %s: ", AppMode_name(g_app.mode), name);
    vprintf(text, args);
    putchar('\n');
    fflush(stdout);
}
#endif

static void add_mod(const char *id, size_t len) {
    if (len == 0 || g_app.mod_count >= APP_MAX_MODS) return;
    if (len >= MOUNTS_ID_MAX) len = MOUNTS_ID_MAX - 1;
    memcpy(g_app.mods[g_app.mod_count], id, len);
    g_app.mods[g_app.mod_count][len] = '\0';
    g_app.mod_count++;
}

static void parse_mods_csv(const char *csv) {
    g_app.mod_count = 0;
    const char *start = csv;
    for (const char *p = csv;; p++) {
        if (*p == ',' || *p == '\0') {
            add_mod(start, (size_t)(p - start));
            if (*p == '\0') break;
            start = p + 1;
        }
    }
}

static void parse_cli(int32_t argc, char **argv) {
    for (int32_t i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--mods") == 0 && next) {
            parse_mods_csv(next);
            g_app.mods_from_cli = true;
            i++;
        } else if (strcmp(a, "--launch-file") == 0 && next) {
            snprintf(g_app.launch_file, sizeof(g_app.launch_file), "%s", next);
            i++;
        } else if (strcmp(a, "--scene") == 0 && next) {
            snprintf(g_app.cli_scene, sizeof(g_app.cli_scene), "%s", next);
            i++;
        } else {
            TraceLog(LOG_WARNING, "APP: ignoring unknown argument '%s'", a);
        }
    }
}

static void read_launch_file(void) {
    if (!FileExists(g_app.launch_file)) return;
    char err[256];
    if (!Packages_load_data(g_app.L, g_app.launch_file, err, sizeof(err))) {
        TraceLog(LOG_WARNING, "APP: launch file ignored: %s", err);
        return;
    }
    g_app.mod_count = 0;
    lua_getfield(g_app.L, -1, "mods");
    if (lua_istable(g_app.L, -1)) {
        lua_Integer len = (lua_Integer)lua_rawlen(g_app.L, -1);
        for (lua_Integer i = 1; i <= len; i++) {
            lua_rawgeti(g_app.L, -1, i);
            if (lua_type(g_app.L, -1) == LUA_TSTRING) {
                const char *id = lua_tostring(g_app.L, -1);
                add_mod(id, strlen(id));
            }
            lua_pop(g_app.L, 1);
        }
    }
    lua_pop(g_app.L, 2);
    TraceLog(LOG_INFO, "APP: %d mod(s) from %s", (int)g_app.mod_count, g_app.launch_file);
}

static bool write_launch_file(void) {
    char text[4096];
    size_t len = (size_t)snprintf(text, sizeof(text),
        "-- Written by the launcher. game/editor read this when started without --mods.\n"
        "return {\n    mods = {\n");
    for (int32_t i = 0; i < g_app.mod_count && len < sizeof(text); i++) {
        len += (size_t)snprintf(text + len, sizeof(text) - len, "        \"%s\",\n", g_app.mods[i]);
    }
    if (len < sizeof(text)) snprintf(text + len, sizeof(text) - len, "    },\n}\n");
    return SaveFileText(g_app.launch_file, text);
}

static void register_native_modules(lua_State *L, const AppConfig *cfg) {
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    for (int32_t i = 0; i < cfg->native_count; i++) {
        lua_pushcfunction(L, (lua_CFunction)cfg->native[i].open);
        lua_setfield(L, -2, cfg->native[i].name);
    }
    lua_pop(L, 1);
}

static bool native_available(const char *module, void *user) {
    lua_State *L = (lua_State *)user;
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_getfield(L, -1, module);
    bool ok = !lua_isnil(L, -1);
    lua_pop(L, 2);
    if (ok) return true;
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    lua_getfield(L, -1, module);
    ok = !lua_isnil(L, -1);
    lua_pop(L, 2);
    return ok;
}

static bool apply_stack(AppMode mode) {
    const char *ids[APP_MAX_MODS];
    for (int32_t i = 0; i < g_app.mod_count; i++) ids[i] = g_app.mods[i];
    bool ok = Packages_build_stack(mode, ids, g_app.mod_count,
                                   native_available, g_app.L,
                                   g_app.stack_error, sizeof(g_app.stack_error));
    if (g_app.host) ScriptHost_clear_modules(g_app.host);
    return ok;
}

static const char *home_scene(void) {
    const char *home = Packages_home(g_app.mode);
    return home ? home : "splash";
}

static int l_app_mode(lua_State *L)      { lua_pushstring(L, AppMode_name(g_app.mode)); return 1; }
static int l_app_is_single(lua_State *L) { lua_pushboolean(L, g_app.cfg->single); return 1; }
static int l_app_is_dev(lua_State *L)    { lua_pushboolean(L, DEV_BUILD); return 1; }
static int l_app_home(lua_State *L)      { lua_pushstring(L, home_scene()); return 1; }

#if defined(GRAMARYE_DEV_TOOLS)
static char g_shot_path[512];

static int l_app_screenshot(lua_State *L) {
    snprintf(g_shot_path, sizeof(g_shot_path), "%s", luaL_checkstring(L, 1));
    return 0;
}

static void take_pending_screenshot(void) {
    if (!g_shot_path[0]) return;
    rlDrawRenderBatchActive();
    Image img = LoadImageFromScreen();
    if (img.data && !ExportImage(img, g_shot_path)) TraceLog(LOG_WARNING, "APP: screenshot to %s failed", g_shot_path);
    UnloadImage(img);
    g_shot_path[0] = 0;
}
#endif

static int l_app_can_spawn(lua_State *L) {
    lua_pushboolean(L, !g_app.cfg->single && ChildProcess_supported());
    return 1;
}

static int l_app_quit(lua_State *L) {
    (void)L;
    if (g_app.cfg->single && g_app.mode != APP_LAUNCHER) {
        g_app.pending_switch = true;
        g_app.pending_mode = APP_LAUNCHER;
    } else {
        g_app.quit = true;
    }
    return 0;
}

static void child_exe_path(const char *target, char *out, size_t cap) {
#if defined(_WIN32)
    const char *ext = ".exe";
#else
    const char *ext = "";
#endif
    if (strcmp(target, "launcher") == 0) {
        snprintf(out, cap, "%s%s%s", GetApplicationDirectory(), GRAMARYE_APP_NAME, ext);
    } else {
        snprintf(out, cap, "%s%s_%s%s", GetApplicationDirectory(), GRAMARYE_APP_NAME, target, ext);
    }
}

static int l_app_launch(lua_State *L) {
    const char *target = luaL_checkstring(L, 1);
    AppMode mode;
    if (!AppMode_parse(target, &mode)) return luaL_error(L, "app.launch: unknown app '%s'", target);

    if (!g_app.cfg->single && ChildProcess_poll(&g_app.child)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is already running", g_app.child_app);
        return 2;
    }

    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "mods");
        if (lua_istable(L, -1)) {
            g_app.mod_count = 0;
            lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
            for (lua_Integer i = 1; i <= len; i++) {
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING && Packages_find(lua_tostring(L, -1))) {
                    const char *id = lua_tostring(L, -1);
                    add_mod(id, strlen(id));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    if (!write_launch_file()) {
        TraceLog(LOG_WARNING, "APP: could not write %s", g_app.launch_file);
    }

    if (g_app.cfg->single) {
        g_app.pending_switch = true;
        g_app.pending_mode = mode;
        lua_pushboolean(L, 1);
        return 1;
    }

    char exe[MOUNTS_PATH_MAX];
    child_exe_path(target, exe, sizeof(exe));
    if (!FileExists(exe)) {
        lua_pushnil(L);
        lua_pushfstring(L, "not built: %s", exe);
        return 2;
    }
    const char *argv[6];
    int32_t argc = 0;
    argv[argc++] = exe;
    argv[argc++] = "--launch-file";
    argv[argc++] = g_app.launch_file;
    argv[argc] = NULL;
    if (!ChildProcess_spawn(&g_app.child, exe, argv)) {
        lua_pushnil(L);
        lua_pushfstring(L, "failed to start %s", exe);
        return 2;
    }
    snprintf(g_app.child_app, sizeof(g_app.child_app), "%s", target);
    g_app.child_reported = false;
    TraceLog(LOG_INFO, "APP: started %s (%s)", target, exe);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_app_child(lua_State *L) {
    if (!g_app.child_app[0]) return 0;
    ChildProcess_poll(&g_app.child);
    lua_createtable(L, 0, 3);
    lua_pushstring(L, g_app.child_app);
    lua_setfield(L, -2, "app");
    lua_pushboolean(L, g_app.child.running);
    lua_setfield(L, -2, "running");
    lua_pushinteger(L, g_app.child.exit_code);
    lua_setfield(L, -2, "exit_code");
    return 1;
}

static void push_string_list(lua_State *L, const char (*list)[MOUNTS_ID_MAX], int32_t n) {
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        lua_pushstring(L, list[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

static void push_package(lua_State *L, const Package *p, AppMode check_mode) {
    lua_createtable(L, 0, 12);
    lua_pushstring(L, p->id);       lua_setfield(L, -2, "id");
    lua_pushstring(L, p->name);     lua_setfield(L, -2, "name");
    lua_pushstring(L, p->version);  lua_setfield(L, -2, "version");
    lua_pushstring(L, p->folder);   lua_setfield(L, -2, "folder");
    lua_pushstring(L, p->root);     lua_setfield(L, -2, "root");
    lua_pushboolean(L, p->builtin); lua_setfield(L, -2, "builtin");
    push_string_list(L, p->replaces, p->replaces_count);
    lua_setfield(L, -2, "replaces");
    push_string_list(L, p->depends, p->depends_count);
    lua_setfield(L, -2, "depends");

    lua_createtable(L, APP_MODE_COUNT, 0);
    int32_t n = 0;
    for (int32_t m = 0; m < APP_MODE_COUNT; m++) {
        if (p->apps & (1u << m)) {
            lua_pushstring(L, AppMode_name((AppMode)m));
            lua_rawseti(L, -2, ++n);
        }
    }
    lua_setfield(L, -2, "apps");

    char reason[160];
    bool ok = Packages_check(p, check_mode, native_available, L, reason, sizeof(reason));
    lua_pushboolean(L, ok);
    lua_setfield(L, -2, "ok");
    if (!ok) {
        lua_pushstring(L, reason);
        lua_setfield(L, -2, "reason");
    }
}

static int l_mods_available(lua_State *L) {
    AppMode mode = APP_GAME;
    if (lua_isstring(L, 1) && !AppMode_parse(lua_tostring(L, 1), &mode)) {
        return luaL_error(L, "mods.available: unknown app '%s'", lua_tostring(L, 1));
    }
    int32_t n = Packages_count();
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        push_package(L, Packages_get(i), mode);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_mods_active(lua_State *L) {
    int32_t n = Packages_active_count();
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        push_package(L, Packages_active(i), g_app.mode);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_mods_selected(lua_State *L) {
    push_string_list(L, (const char (*)[MOUNTS_ID_MAX])g_app.mods, g_app.mod_count);
    return 1;
}

static int l_mods_problem(lua_State *L) {
    if (!g_app.stack_error[0]) return 0;
    lua_pushstring(L, g_app.stack_error);
    return 1;
}

static void register_app_lua(ScriptHost *host) {
    ScriptHost_register_function(host, "app", "mode", l_app_mode);
    ScriptHost_register_function(host, "app", "is_single", l_app_is_single);
    ScriptHost_register_function(host, "app", "is_dev", l_app_is_dev);
    ScriptHost_register_function(host, "app", "home", l_app_home);
#if defined(GRAMARYE_DEV_TOOLS)
    ScriptHost_register_function(host, "app", "screenshot", l_app_screenshot);
#endif
    ScriptHost_register_function(host, "app", "can_spawn", l_app_can_spawn);
    ScriptHost_register_function(host, "app", "launch", l_app_launch);
    ScriptHost_register_function(host, "app", "child", l_app_child);
    ScriptHost_register_function(host, "app", "quit", l_app_quit);
    ScriptHost_register_function(host, "mods", "available", l_mods_available);
    ScriptHost_register_function(host, "mods", "active", l_mods_active);
    ScriptHost_register_function(host, "mods", "selected", l_mods_selected);
    ScriptHost_register_function(host, "mods", "problem", l_mods_problem);
}

static void world_custom_draw(int32_t kind, float x, float y, float w, float h, void *user) {
    World *world = (World *)user;
    if (kind == WORLD_GLOBE_KIND)              world_draw_in_rect(world, x, y, w, h);
    else if (kind == WORLD_CONTROLS_KIND)      world_draw_controls_in_rect(world, x, y, w, h);
    else if (kind == LOCAL_CHUNK_DEBUG_KIND) {
        local_chunk_draw_connected_in_rect(local_chunk_lua_current(), local_chunk_lua_view(), x, y, w, h);
    } else if (kind == LOCAL_CHUNK_GROUP_DEBUG_KIND) {
        local_chunk_draw_group_in_rect(local_chunk_lua_current(), local_chunk_lua_view(), x, y, w, h);
    } else if (kind == AUDIO_SCOPE_KIND)       audio_device_draw_scope(x, y, w, h);
    else if (kind == AUDIO_SPECTRUM_KIND)      audio_device_draw_spectrum(x, y, w, h);
    else if (tilemap_lua_draw(kind, x, y, w, h)) {}
#if defined(GRAMARYE_DEV_TOOLS)
    else if (kind == SHADER_PLAYGROUND_KIND)   shader_playground_draw_in_rect(x, y, w, h);
    else if (kind == SHADER_EDITOR_KIND)       shader_playground_draw_editor_in_rect(x, y, w, h);
#endif
}

typedef struct FrameLoop {
    GlobalState *global_state;
    ECS *ecs;
    ScriptHost *host;
    SystemId sprite_render_id;
    SystemId skinned_sprite_render_id;
    double last_time;
    float accumulator;
#if defined(APP_DESKTOP)
    double last_reload_time;
#endif
} FrameLoop;

static void apply_pending_switch(ScriptHost *host) {
    if (!g_app.pending_switch) return;
    g_app.pending_switch = false;
    ScriptHost_unload_all(host);
    g_app.mode = g_app.pending_mode;
    apply_stack(g_app.mode);
    ScriptHost_load_scene(host, home_scene());
}

static void poll_child(void) {
    if (!g_app.child_app[0] || g_app.child_reported) return;
    if (!ChildProcess_poll(&g_app.child)) {
        g_app.child_reported = true;
        TraceLog(g_app.child.exit_code == 0 ? LOG_INFO : LOG_WARNING,
                 "APP: %s exited with code %d", g_app.child_app, (int)g_app.child.exit_code);
    }
}

static void update_draw_frame(void *arg) {
    FrameLoop *loop = (FrameLoop *)arg;
    GlobalState *global_state = loop->global_state;

    double now = GetTime();
    float frame_dt = (float)(now - loop->last_time);
    loop->last_time = now;
    if (frame_dt > MAX_FRAME_DT) frame_dt = MAX_FRAME_DT;
    loop->accumulator += frame_dt;

    Arena_free(global_state->frame_arena);

    apply_pending_switch(loop->host);
    poll_child();

#if defined(APP_DESKTOP)
    if (IsKeyPressed(KEY_F5) && (now - loop->last_reload_time) > 0.3) {
        ScriptHost_reload_current(loop->host);
        tilemap_lua_reload_shaders();
        loop->last_reload_time = now;
    }
#if defined(GRAMARYE_DEV_TOOLS)
    FileWatch_update(loop->host, frame_dt);
    tilemap_lua_watch_shaders(frame_dt);
#endif
#endif

    while (loop->accumulator >= SIM_DT) {
        ScriptHost_update_fixed(loop->host, SIM_DT);
        loop->accumulator -= SIM_DT;
    }
    ScriptHost_update(loop->host, frame_dt);

    BeginDrawing();
    ClearBackground(BLACK);

    ECS_update_system(loop->ecs, loop->sprite_render_id, frame_dt);
    ECS_update_system(loop->ecs, loop->skinned_sprite_render_id, frame_dt);

    GramaryeUI_begin(frame_dt);
    ScriptHost_draw(loop->host);
    GramaryeUI_end_and_render();
    GramaryeUI_dispatch_events(ScriptHost_state(loop->host));
#if defined(GRAMARYE_DEV_TOOLS)
    take_pending_screenshot();
#endif

    EndDrawing();
}

static Font load_ui_font(bool *loaded) {
    int ui_codepoints[95 + 4];
    int32_t ui_cp_count = 0;
    for (int32_t c = 32; c <= 126; c++) ui_codepoints[ui_cp_count++] = c;
    const int32_t ui_extra[] = { 0x00D7, 0x2014, 0x2026, 0x2022 };
    for (int32_t k = 0; k < (int32_t)(sizeof(ui_extra) / sizeof(ui_extra[0])); k++)
        ui_codepoints[ui_cp_count++] = ui_extra[k];

    Font font = { 0 };
    char path[MOUNTS_PATH_MAX];
    if (Mounts_resolve("fonts/Roboto-VariableFont_wdth,wght.ttf", path, sizeof(path)) >= 0) {
        font = LoadFontEx(path, UI_FONT_BASE_SIZE, ui_codepoints, ui_cp_count);
    }
    *loaded = font.texture.id != 0;
    if (!*loaded) {
        TraceLog(LOG_WARNING, "UI font failed to load; using raylib default");
        return GetFontDefault();
    }
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    return font;
}

int gramarye_app_run(const AppConfig *cfg) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.cfg = cfg;
    g_app.mode = cfg->mode;

#if defined(APP_DESKTOP)
    SetTraceLogCallback(log_callback);
#endif
    parse_cli(cfg->argc, cfg->argv);

    /******************************************************************************
    ================================| Initialization |=============================
    *******************************************************************************/
    Arena_T arena = Arena_new();
    ECS *ecs = ECS_new(arena);

    //TODO: Make a config it consumes on load. Current Params of Init Window will be for default values if deleted
    const char *title = cfg->mode == APP_EDITOR ? GAME_TITLE " Editor" : GAME_TITLE;
    InitWindow(GAME_WIDTH, GAME_HEIGHT, title);
    if (!IsWindowReady()) {
        TraceLog(LOG_ERROR, "window failed to initialize; aborting");
        ECS_destroy(ecs);
        Arena_dispose(&arena);
        return 1;
    }
#if defined(APP_DESKTOP)
    ChangeDirectory(GetApplicationDirectory());
#endif
    SetTargetFPS(60);

    Mounts_init();
    if (!g_app.launch_file[0]) {
        snprintf(g_app.launch_file, sizeof(g_app.launch_file), "%slast_launch.lua", Mounts_user_dir());
    }

    audio_device_init(arena);

    GlobalState *global_state = (GlobalState *)Arena_alloc(arena, sizeof(GlobalState), __FILE__, __LINE__);
    global_state->total_time = 0.0f;
    global_state->frame_count = 0;

    //TODO: When we resize window with a config we would need to call that instead, it may just be a header for config and runs a function to load from a json persistance layer file
    global_state->camera = (Camera2D){
        .offset = { GAME_WIDTH / 2.0f, GAME_HEIGHT / 2.0f },
        .target = { 0.0f, 0.0f },
        .rotation = 0.0f,
        .zoom = 1.0f
    };
    global_state->frame_arena = Arena_new();
    global_state->scene_arena = NULL;

    TextureStore_init();
    global_state->transform_type = ECS_register_component_type(ecs, "transform2d", sizeof(Transform2D));
    global_state->sprite_type = ECS_register_component_type(ecs, "sprite", sizeof(SpriteComp));
    global_state->skinned_sprite_type = ECS_register_component_type(ecs, "skinned_sprite", sizeof(SkinnedSpriteComp));
    global_state->tile_type = ECS_register_component_type(ecs, "tile", sizeof(TileComp));

    //TODO: Also adjust to use the config
    GramaryeUI_init(GAME_WIDTH, GAME_HEIGHT);

    ScriptHost *host = ScriptHost_new(arena, ecs, global_state);
    g_app.host = host;
    g_app.L = ScriptHost_state(host);
    GramaryeUI_register_lua(g_app.L);
    register_native_modules(g_app.L, cfg);

    Packages_scan(g_app.L);
    if (!g_app.mods_from_cli) read_launch_file();
    apply_stack(g_app.mode);

    SystemId sprite_render_id = sprite_system_register(ecs, global_state);
    SystemId skinned_sprite_render_id = skinned_sprite_system_register(ecs, global_state);
    global_state->world = world_create(ecs, global_state->tile_type, WORLD_START_LEVEL);

    bool ui_font_loaded = false;
    Font ui_fonts[1];
    ui_fonts[0] = load_ui_font(&ui_font_loaded);
    GramaryeUI_set_fonts(ui_fonts, 1);
    GramaryeUI_set_custom_draw(world_custom_draw, global_state->world);

    ScriptHost_register_system(host, "global", global_system_register(ecs, global_state));
    ScriptHost_register_system(host, "sprite_render", sprite_render_id);
    ScriptHost_register_system(host, "skinned_sprite_render", skinned_sprite_render_id);
    entities_lua_register(host, ecs, global_state);
    world_lua_register(host, global_state->world);
    local_chunk_lua_register(host, global_state->world);
    synth_lua_register(host, audio_device_synth());
    tilemap_lua_register(host);
#if defined(GRAMARYE_DEV_TOOLS)
    shader_playground_lua_register(host);
#endif
    register_app_lua(host);

    const char *start = g_app.cli_scene[0] ? g_app.cli_scene
                      : cfg->start_scene   ? cfg->start_scene
                      : home_scene();
    TraceLog(LOG_INFO, "APP: %s starting in scene '%s'", AppMode_name(g_app.mode), start);
    ScriptHost_load_scene(host, start);

    FrameLoop loop = {
        .global_state = global_state,
        .ecs = ecs,
        .host = host,
        .sprite_render_id = sprite_render_id,
        .skinned_sprite_render_id = skinned_sprite_render_id,
        .last_time = GetTime(),
        .accumulator = 0.0f,
#if defined(APP_DESKTOP)
        .last_reload_time = -1.0,
#endif
    };

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop_arg(update_draw_frame, &loop, 0, 1);
#else
    while (!WindowShouldClose() && !g_app.quit) {
        update_draw_frame(&loop);
    }
#endif

    GramaryeUI_shutdown();
    ScriptHost_dispose(host);
    tilemap_lua_shutdown();
#if defined(GRAMARYE_DEV_TOOLS)
    shader_playground_unload();
#endif
    world_destroy(global_state->world);
    skinned_sprite_system_shutdown();
    TextureStore_shutdown();
    if (ui_font_loaded) UnloadFont(ui_fonts[0]);
    audio_device_shutdown();
    CloseWindow();
    ECS_destroy(ecs);
    Arena_dispose(&global_state->frame_arena);
    Arena_dispose(&arena);
    return 0;
}
