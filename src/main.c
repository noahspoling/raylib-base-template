#include "raylib.h"
#include "arena.h"
#include "gramarye_ecs/ecs.h"

#include "game_config.h"
#include "global_system.h"
#include "scene.h"

int main(void) {
    Arena_T arena = Arena_new();
    ECS *ecs = ECS_new(arena);

    InitWindow(GAME_WIDTH, GAME_HEIGHT, __PROJECT_NAME__);
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

    Scene *base_scene = Scene_new(arena, "base");
    Scene_add_system(base_scene, global_system_register(ecs, global_state));

    double last_time = GetTime();
    while (!WindowShouldClose()) {
        double now = GetTime();
        float dt = (float)(now - last_time);
        last_time = now;

        Scene_run(base_scene, ecs, dt);

        BeginDrawing();
        ClearBackground(BLACK);
        DrawText("Template loop running", 24, 24, 24, RAYWHITE);
        EndDrawing();
    }

    CloseWindow();
    ECS_destroy(ecs);
    Arena_dispose(&arena);
    return 0;
}
