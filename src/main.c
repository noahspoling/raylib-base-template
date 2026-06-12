#include "raylib.h"
#include "arena.h"
#include "gramarye_ecs/ecs.h"

#include "game_config.h"
#include "global_system.h"
#include "script_host.h"

int main(void) {
    Arena_T arena = Arena_new();
    ECS *ecs = ECS_new(arena);

    InitWindow(GAME_WIDTH, GAME_HEIGHT, GAME_TITLE);
    if (!IsWindowReady()) {
        // GL context creation failed (e.g. broken/mismatched driver). Bail
        // cleanly instead of running the loop against an uninitialized window.
        TraceLog(LOG_ERROR, "window failed to initialize; aborting");
        ECS_destroy(ecs);
        Arena_dispose(&arena);
        return 1;
    }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    // run scripts may exec the binary from any cwd; assets/ sits next to it.
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

    ScriptHost *host = ScriptHost_new(arena, ecs, global_state);
    ScriptHost_register_system(host, "global", global_system_register(ecs, global_state));
    ScriptHost_load_scene(host, "splash");

    double last_time = GetTime();
    while (!WindowShouldClose()) {
        double now = GetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        ScriptHost_update(host, dt);

        BeginDrawing();
        ClearBackground(BLACK);
        ScriptHost_draw(host);
        EndDrawing();
    }

    ScriptHost_dispose(host);
    CloseWindow();
    ECS_destroy(ecs);
    Arena_dispose(&arena);
    return 0;
}
