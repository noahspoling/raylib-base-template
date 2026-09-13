#ifndef ENTITIES_LUA_H
#define ENTITIES_LUA_H

#include "script_host.h"

void entities_lua_register(ScriptHost *host, ECS *ecs, GlobalState *state);

#endif
