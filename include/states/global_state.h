#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include "raylib.h"
#include "arena.h"
#include "gramarye_ecs/component.h"

typedef struct World World;

typedef struct GlobalState {
    float total_time;
    uint64_t frame_count;
    Camera2D camera;
    Arena_T frame_arena;
    Arena_T scene_arena;
    ComponentTypeId transform_type;
    ComponentTypeId sprite_type;
    ComponentTypeId skinned_sprite_type;
    ComponentTypeId tile_type;
    World *world;
} GlobalState;

#endif
