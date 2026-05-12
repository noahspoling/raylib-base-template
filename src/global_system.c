#include "global_system.h"

static void global_system_update(ECS *ecs, float deltaTime, void *userData) {
    (void)ecs;
    GlobalState *state = (GlobalState *)userData;
    if (!state) return;
    state->total_time += deltaTime;
    state->frame_count++;
}

SystemId global_system_register(ECS *ecs, GlobalState *userData) {
    return ECS_register_system_with_priority(ecs, "global",
                                             NULL, 0,
                                             global_system_update, userData,
                                             0);
}
