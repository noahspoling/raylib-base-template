#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include "raylib.h"
#include "arena.h"
#include "gramarye_ecs/component.h"

// Shared state handed to C systems as userData. Also carries the arena tiers:
//   - persistent arena (main.c): engine lifetime, never reset
//   - scene_arena: rewound on every full scene change (owned by ScriptHost)
//   - frame_arena: rewound at the top of every render frame (owned by main.c);
//     systems can allocate per-frame scratch here with zero free cost.
typedef struct GlobalState {
    float total_time;    // accumulated sim time (fixed steps)
    int frame_count;     // sim tick count
    Camera2D camera;
    Arena_T frame_arena;
    Arena_T scene_arena;
    ComponentTypeId transform_type;
    ComponentTypeId sprite_type;
} GlobalState;

#endif
