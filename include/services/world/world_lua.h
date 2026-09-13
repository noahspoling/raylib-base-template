#ifndef WORLD_LUA_H
#define WORLD_LUA_H

#include "script_host.h"
#include "services/world/world.h"

void world_lua_register(ScriptHost *host, World *world);

#endif
