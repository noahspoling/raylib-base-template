#include "services/local_chunk/local_chunk_lua.h"
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"
#include "raylib.h"

static World *g_world = NULL;
static LocalChunkGroup g_current_group = {0};
static int32_t g_view = LOCAL_CHUNK_VIEW_TERRAIN;

const LocalChunkGroup *local_chunk_lua_current(void) { return &g_current_group; }
int32_t local_chunk_lua_view(void) { return g_view; }

static int l_local_chunk_generate(lua_State *L) {
    int32_t cell = (int32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, 0);
    if (!g_world) return 1;

    const Planet *planet = world_planet(g_world);
    WorldFields   fields  = world_fields(g_world);

    LocalChunkGroup next = local_chunk_create_group(planet, &fields, world_seed(g_world),
                                                     world_generator_index(g_world),
                                                     cell);
    if (next.count == 0) {
        TraceLog(LOG_WARNING, "local_chunk: create_group failed for cell %d", cell);
        return 1;
    }

    if (next.owner_mismatches > 0) {
        TraceLog(LOG_WARNING, "local_chunk: cell %d has %d view samples whose owner depends on the "
                                "query hint or falls outside the owner's raster", cell, next.owner_mismatches);
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

static int l_local_chunk_view(lua_State *L) {
    lua_pushinteger(L, g_view);
    return 1;
}

static int l_local_chunk_set_view(lua_State *L) {
    int32_t v = (int32_t)luaL_checkinteger(L, 1);
    if (v >= 0 && v < LOCAL_CHUNK_VIEW_COUNT) g_view = v;
    return 0;
}

static int l_local_chunk_view_count(lua_State *L) {
    lua_pushinteger(L, LOCAL_CHUNK_VIEW_COUNT);
    return 1;
}

static int l_local_chunk_view_name(lua_State *L) {
    lua_pushstring(L, local_chunk_view_name((int32_t)luaL_checkinteger(L, 1)));
    return 1;
}

void local_chunk_lua_register(ScriptHost *host, World *world) {
    g_world = world;

    ScriptHost_register_function(host, "local_chunk", "generate",     l_local_chunk_generate);
    ScriptHost_register_function(host, "local_chunk", "clear",        l_local_chunk_clear);
    ScriptHost_register_function(host, "local_chunk", "current_cell", l_local_chunk_current_cell);
    ScriptHost_register_function(host, "local_chunk", "view",         l_local_chunk_view);
    ScriptHost_register_function(host, "local_chunk", "set_view",     l_local_chunk_set_view);
    ScriptHost_register_function(host, "local_chunk", "view_count",   l_local_chunk_view_count);
    ScriptHost_register_function(host, "local_chunk", "view_name",    l_local_chunk_view_name);
}
