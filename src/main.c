#include "raylib.h"
#include "arena.h"
#include "gramarye_ecs/ecs.h"

#include "game_config.h"
#include "global_system.h"
#include "script_host.h"
#include "entities_lua.h"
#include "components/sprite.h"
#include "services/texture_store.h"
#include "systems/sprite/sprite_system.h"

#include "gramarye_ui/ui.h"
#include "gramarye_ui/ui_lua.h"

// Asset paths: "" on Android (APK root), "assets/" elsewhere (copied next to the
// binary on desktop, preloaded into MEMFS on web). Matches script_host's prefix.
#if defined(__ANDROID__)
#define ASSET_PREFIX ""
#else
#define ASSET_PREFIX "assets/"
#endif

// Base pixel size the UI font is rasterized at. The renderer scales glyphs by
// requested/base size, so load high enough that the largest UI text (titles ~28)
// is crisp; smaller sizes downscale cleanly with bilinear filtering.
#define UI_FONT_BASE_SIZE 48

// Fixed simulation step: C systems run at a constant rate regardless of the
// render rate. Lua on_update/on_draw run once per render frame (see
// script_host.h for the timing model).
#define SIM_DT (1.0f / 60.0f)
// Clamp: a window drag, debugger pause, or scene load must not produce one
// giant catch-up step (spiral of death).
#define MAX_FRAME_DT 0.25f

int main(void) {
    Arena_T arena = Arena_new();
    ECS *ecs = ECS_new(arena);

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
    global_state->camera = (Camera2D){
        .offset = { GAME_WIDTH / 2.0f, GAME_HEIGHT / 2.0f },
        .target = { 0.0f, 0.0f },
        .rotation = 0.0f,
        .zoom = 1.0f
    };
    // Frame arena: per-frame scratch for C systems, rewound at the top of
    // every render frame. (scene_arena is set by ScriptHost_new.)
    global_state->frame_arena = Arena_new();
    global_state->scene_arena = NULL;

    TextureStore_init();
    global_state->transform_type = ECS_register_component_type(ecs, "transform2d", sizeof(Transform2D));
    global_state->sprite_type = ECS_register_component_type(ecs, "sprite", sizeof(SpriteComp));
    SystemId sprite_render_id = sprite_system_register(ecs, global_state);

    GramaryeUI_init(GAME_WIDTH, GAME_HEIGHT);
    // Font 0 is the default UI font (gramarye.ui text uses font=0). Load Roboto;
    // fall back to raylib's built-in font if the TTF is missing.
    // ASCII 32–126 plus the punctuation/symbols the UI uses that Roboto provides:
    // × (multiply), — (em dash), … (ellipsis), • (bullet). Glyphs not in this set
    // render as '?'. (Roboto lacks the ←↑→↓ arrows, so the demo uses </>.)
    int ui_codepoints[95 + 4];
    int ui_cp_count = 0;
    for (int c = 32; c <= 126; c++) ui_codepoints[ui_cp_count++] = c;
    const int ui_extra[] = { 0x00D7, 0x2014, 0x2026, 0x2022 };
    for (int k = 0; k < (int)(sizeof(ui_extra) / sizeof(ui_extra[0])); k++)
        ui_codepoints[ui_cp_count++] = ui_extra[k];

    Font ui_fonts[1];
    ui_fonts[0] = LoadFontEx(ASSET_PREFIX "fonts/Roboto-VariableFont_wdth,wght.ttf",
                             UI_FONT_BASE_SIZE, ui_codepoints, ui_cp_count);
    bool ui_font_loaded = ui_fonts[0].texture.id != 0;
    if (!ui_font_loaded) {
        TraceLog(LOG_WARNING, "UI font failed to load; using raylib default");
        ui_fonts[0] = GetFontDefault();
    } else {
        // Smooth glyph scaling for the non-base UI sizes.
        SetTextureFilter(ui_fonts[0].texture, TEXTURE_FILTER_BILINEAR);
    }
    GramaryeUI_set_fonts(ui_fonts, 1);

    ScriptHost *host = ScriptHost_new(arena, ecs, global_state);
    ScriptHost_register_system(host, "global", global_system_register(ecs, global_state));
    // Registered for the Lua enable-toggle only; never added to a scene's
    // update list (it draws, so main drives it from the render phase below).
    ScriptHost_register_system(host, "sprite_render", sprite_render_id);
    GramaryeUI_register_lua(ScriptHost_state(host));
    entities_lua_register(host, ecs, global_state);
    ScriptHost_load_scene(host, "splash");

    // NOTE: web builds run this plain while-loop; emscripten_set_main_loop
    // integration is a known gap, tracked separately.
    double last_time = GetTime();
    float accumulator = 0.0f;
    while (!WindowShouldClose()) {
        double now = GetTime();
        float frame_dt = (float)(now - last_time);
        last_time = now;
        if (frame_dt > MAX_FRAME_DT) frame_dt = MAX_FRAME_DT;
        accumulator += frame_dt;

        Arena_free(global_state->frame_arena);

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
        // Once per render frame (inside the sim loop it would retrigger on
        // every step of the same frame).
        if (IsKeyPressed(KEY_F5)) ScriptHost_reload_current(host);
#endif

        // Fixed-step simulation: 0..N constant-dt steps per render frame.
        while (accumulator >= SIM_DT) {
            ScriptHost_update_fixed(host, SIM_DT);
            accumulator -= SIM_DT;
        }
        // Lua orchestration once per render frame (input edges stay 1:1).
        ScriptHost_update(host, frame_dt);

        BeginDrawing();
        ClearBackground(BLACK);

        // World pass: sprite entities under the camera, below the UI.
        ECS_update_system(ecs, sprite_render_id, frame_dt);

        // GramaryeUI_begin opens a Clay layout so Lua's on_draw can call
        // gramarye.ui.render(tree) to build the UI this frame.
        GramaryeUI_begin(frame_dt);
        ScriptHost_draw(host);
        GramaryeUI_end_and_render();
        GramaryeUI_dispatch_events(ScriptHost_state(host));

        EndDrawing();
    }

    GramaryeUI_shutdown();
    ScriptHost_dispose(host);
    TextureStore_shutdown();
    if (ui_font_loaded) UnloadFont(ui_fonts[0]);
    CloseWindow();
    ECS_destroy(ecs);
    Arena_dispose(&global_state->frame_arena);
    Arena_dispose(&arena);
    return 0;
}
