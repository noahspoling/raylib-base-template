#ifndef SYSTEMS_SPRITE_SYSTEM_H
#define SYSTEMS_SPRITE_SYSTEM_H

#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

SystemId sprite_system_register(ECS *ecs, GlobalState *state);

#endif
