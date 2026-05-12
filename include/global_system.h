#ifndef GLOBAL_SYSTEM_H
#define GLOBAL_SYSTEM_H

#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

SystemId global_system_register(ECS *ecs, GlobalState *userData);

#endif
