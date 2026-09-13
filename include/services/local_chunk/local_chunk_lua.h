#ifndef SERVICES_LOCAL_CHUNK_LUA_H
#define SERVICES_LOCAL_CHUNK_LUA_H

#include "script_host.h"
#include "services/world/world.h"
#include "services/local_chunk/local_chunk.h"

#define LOCAL_CHUNK_DEBUG_KIND 3
#define LOCAL_CHUNK_GROUP_DEBUG_KIND 4

void local_chunk_lua_register(ScriptHost *host, World *world);

const LocalChunkGroup *local_chunk_lua_current(void);

#endif
