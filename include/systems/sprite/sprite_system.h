#ifndef SYSTEMS_SPRITE_SYSTEM_H
#define SYSTEMS_SPRITE_SYSTEM_H

#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

// Sprite render system. Registered as a normal ECS system so Lua can toggle
// it (gramarye.systems.enable("sprite_render", ...)), but it is NOT added to
// scenes: drawing must happen inside BeginDrawing, so main.c drives it from
// the render phase via ECS_update_system, under the GlobalState camera and
// before the UI pass. Iterates sprite components densely; the full entity
// set is never scanned.
SystemId sprite_system_register(ECS *ecs, GlobalState *state);

#endif
