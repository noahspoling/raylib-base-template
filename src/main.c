#include "raylib.h"
#include <stdint.h>
#include "arena.h"
#include "gramarye_ecs/ecs.h"

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
#include "systems/sprite/sprite_system.h"
#include "systems/skinned_sprite/skinned_sprite_system.h"

#include "services/world/world.h"
#include "services/world/world_lua.h"
#include "services/local_chunk/local_chunk_lua.h"

#include "gramarye_ui/ui.h"
#include "gramarye_ui/ui_lua.h"

#define WORLD_START_LEVEL 4

static void world_custom_draw(int32_t kind, float x, float y, float w, float h, void *user) {
    World *world = (World *)user;
    if (kind == WORLD_GLOBE_KIND)              world_draw_in_rect(world, x, y, w, h);
    else if (kind == WORLD_CONTROLS_KIND)      world_draw_controls_in_rect(world, x, y, w, h);
    else if (kind == LOCAL_CHUNK_DEBUG_KIND) {
        local_chunk_draw_connected_in_rect(local_chunk_lua_current(), x, y, w, h);
    } else if (kind == LOCAL_CHUNK_GROUP_DEBUG_KIND) {
        local_chunk_draw_group_in_rect(local_chunk_lua_current(), x, y, w, h);
    }
}

#if defined(__ANDROID__)
#define ASSET_PREFIX ""
#else
#define ASSET_PREFIX "assets/"
#endif

#define UI_FONT_BASE_SIZE 48

#define SIM_DT (1.0f / 60.0f)
#define MAX_FRAME_DT 0.25f

typedef struct FrameLoop {
    GlobalState *global_state;
    ECS *ecs;
    ScriptHost *host;
    SystemId sprite_render_id;
    SystemId skinned_sprite_render_id;
    double last_time;
    float accumulator;
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    double last_reload_time;
#endif
} FrameLoop;

static void update_draw_frame(void *arg) {
    FrameLoop *loop = (FrameLoop *)arg;
    GlobalState *global_state = loop->global_state;

    double now = GetTime();
    float frame_dt = (float)(now - loop->last_time);
    loop->last_time = now;
    if (frame_dt > MAX_FRAME_DT) frame_dt = MAX_FRAME_DT;
    loop->accumulator += frame_dt;

    Arena_free(global_state->frame_arena);

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    if (IsKeyPressed(KEY_F5) && (now - loop->last_reload_time) > 0.3) {
        ScriptHost_reload_current(loop->host);
        loop->last_reload_time = now;
    }
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

    EndDrawing();
}

int main(void) {

    /******************************************************************************
    ================================| Initialization |=============================
    *******************************************************************************/
    Arena_T arena = Arena_new();
    ECS *ecs = ECS_new(arena);

    //TODO: Make a config it consumes on load. Current Params of Init Window will be for default values if deleted

    InitWindow(GAME_WIDTH, GAME_HEIGHT, GAME_TITLE);
    if (!IsWindowReady()) {
        TraceLog(LOG_ERROR, "window failed to initialize; aborting");
        ECS_destroy(ecs);
        Arena_dispose(&arena);
        return 1;
    }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    ChangeDirectory(GetApplicationDirectory());
#endif
    SetTargetFPS(60);

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
    SystemId sprite_render_id = sprite_system_register(ecs, global_state);
    SystemId skinned_sprite_render_id = skinned_sprite_system_register(ecs, global_state);

    global_state->world = world_create(ecs, global_state->tile_type, WORLD_START_LEVEL);

    //TODO: Also adjust to use the config
    GramaryeUI_init(GAME_WIDTH, GAME_HEIGHT);

    //limits the characters rendered
    int ui_codepoints[95 + 4];
    int32_t ui_cp_count = 0;
    for (int32_t c = 32; c <= 126; c++) ui_codepoints[ui_cp_count++] = c;
    const int32_t ui_extra[] = { 0x00D7, 0x2014, 0x2026, 0x2022 }; // x, —, …, • special characters
    for (int32_t k = 0; k < (int32_t)(sizeof(ui_extra) / sizeof(ui_extra[0])); k++)
        ui_codepoints[ui_cp_count++] = ui_extra[k];

    Font ui_fonts[1];
    ui_fonts[0] = LoadFontEx(ASSET_PREFIX "fonts/Roboto-VariableFont_wdth,wght.ttf",
                             UI_FONT_BASE_SIZE, ui_codepoints, ui_cp_count);
    bool ui_font_loaded = ui_fonts[0].texture.id != 0;
    if (!ui_font_loaded) {
        TraceLog(LOG_WARNING, "UI font failed to load; using raylib default");
        ui_fonts[0] = GetFontDefault();
    } else {
        SetTextureFilter(ui_fonts[0].texture, TEXTURE_FILTER_BILINEAR);
    }
    GramaryeUI_set_fonts(ui_fonts, 1);
    GramaryeUI_set_custom_draw(world_custom_draw, global_state->world);

    ScriptHost *host = ScriptHost_new(arena, ecs, global_state);
    ScriptHost_register_system(host, "global", global_system_register(ecs, global_state));
    ScriptHost_register_system(host, "sprite_render", sprite_render_id);
    ScriptHost_register_system(host, "skinned_sprite_render", skinned_sprite_render_id);
    GramaryeUI_register_lua(ScriptHost_state(host));
    entities_lua_register(host, ecs, global_state);
    world_lua_register(host, global_state->world);
    local_chunk_lua_register(host, global_state->world);
    ScriptHost_load_scene(host, "splash");

    FrameLoop loop = {
        .global_state = global_state,
        .ecs = ecs,
        .host = host,
        .sprite_render_id = sprite_render_id,
        .skinned_sprite_render_id = skinned_sprite_render_id,
        .last_time = GetTime(),
        .accumulator = 0.0f,
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
        .last_reload_time = -1.0,
#endif
    };

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop_arg(update_draw_frame, &loop, 0, 1);
#else
    while (!WindowShouldClose()) {
        update_draw_frame(&loop);
    }
#endif

    GramaryeUI_shutdown();
    ScriptHost_dispose(host);
    world_destroy(global_state->world);
    skinned_sprite_system_shutdown();
    TextureStore_shutdown();
    if (ui_font_loaded) UnloadFont(ui_fonts[0]);
    CloseWindow();
    ECS_destroy(ecs);
    Arena_dispose(&global_state->frame_arena);
    Arena_dispose(&arena);
    return 0;
}
