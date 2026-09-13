#ifndef SYSTEMS_SKINNED_SPRITE_SYSTEM_H
#define SYSTEMS_SKINNED_SPRITE_SYSTEM_H

#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

SystemId skinned_sprite_system_register(ECS *ecs, GlobalState *state);
void skinned_sprite_system_shutdown(void);

#endif
