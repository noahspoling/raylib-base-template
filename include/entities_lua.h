#ifndef ENTITIES_LUA_H
#define ENTITIES_LUA_H

#include "script_host.h"

// Installs gramarye.entities.* (spawn/despawn/set_transform/set_sprite) and
// gramarye.textures.load. Entity handles are full userdata wrapping EntityId.
void entities_lua_register(ScriptHost *host, ECS *ecs, GlobalState *state);

#endif
