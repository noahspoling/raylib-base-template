#include "services/local_chunk/local_chunk_lua.h"

#include "lua.h"
#include "lauxlib.h"
#include "raylib.h"

#define LOCAL_CHUNK_DEBUG_RES 72

static World *g_world = NULL;
static LocalChunkGroup g_current_group = {0};

const LocalChunkGroup *local_chunk_lua_current(void) { return &g_current_group; }

static int l_local_chunk_generate(lua_State *L) {
    int cell = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, 0);
    if (!g_world) return 1;

    const Planet *planet = world_planet(g_world);
    WorldFields   fields  = world_fields(g_world);

    LocalChunkGroup next = local_chunk_create_group(planet, &fields, world_seed(g_world),
                                                     world_generator_index(g_world),
                                                     cell, LOCAL_CHUNK_DEBUG_RES);
    if (next.count == 0) {
        TraceLog(LOG_WARNING, "local_chunk: create_group failed for cell %d", cell);
        return 1;
    }

    local_chunk_destroy_group(&g_current_group);
    g_current_group = next;
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_local_chunk_clear(lua_State *L) {
    (void)L;
    local_chunk_destroy_group(&g_current_group);
    return 0;
}

static int l_local_chunk_current_cell(lua_State *L) {
    if (g_current_group.count > 0) lua_pushinteger(L, g_current_group.tiles[0]->cell);
    else                           lua_pushnil(L);
    return 1;
}

void local_chunk_lua_register(ScriptHost *host, World *world) {
    g_world = world;

    ScriptHost_register_function(host, "local_chunk", "generate",     l_local_chunk_generate);
    ScriptHost_register_function(host, "local_chunk", "clear",        l_local_chunk_clear);
    ScriptHost_register_function(host, "local_chunk", "current_cell", l_local_chunk_current_cell);

    if (world) {
        int mismatches = 0;
        int total = local_chunk_debug_check_reciprocity(world_planet(world), &mismatches);
        TraceLog(LOG_INFO, "local_chunk: edge-role reciprocity check: %d/%d candidate-staggered "
                             "edges forced to clean (disagreement)", mismatches, total);
    }
}
