#include "services/world/world_lua.h"
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

#include "services/world/planet.h"
#include "components/tile.h"

static const char *WORLD_REGISTRY_KEY = "gramarye.world.ptr";

static World *world_from(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, WORLD_REGISTRY_KEY);
    World *w = (World *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return w;
}

static int l_world_regenerate(lua_State *L) {
    World *w = world_from(L);
    if (w) world_apply(w);
    return 0;
}

static int l_world_reroll(lua_State *L) {
    World *w = world_from(L);
    if (w) world_reroll(w);
    return 0;
}

static int l_world_next_algorithm(lua_State *L) {
    World *w = world_from(L);
    if (!w) { lua_pushnil(L); return 1; }
    world_set_generator(w, world_generator_index(w) + 1);
    lua_pushstring(L, world_generator_name(w));
    return 1;
}

static int l_world_set_level(lua_State *L) {
    World *w = world_from(L);
    int32_t level = (int32_t)luaL_checkinteger(L, 1);
    if (w) world_set_pending_level(w, level);
    return 0;
}

static int l_world_level_up(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_pending_level(w, world_pending_level(w) + 1);
    return 0;
}

static int l_world_level_down(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_pending_level(w, world_pending_level(w) - 1);
    return 0;
}

static int l_world_level(lua_State *L) {
    World *w = world_from(L);
    lua_pushinteger(L, w ? world_pending_level(w) : 0);
    return 1;
}

static int l_world_cells(lua_State *L) {
    World *w = world_from(L);
    lua_pushinteger(L, w ? planet_cell_count_for_level(world_pending_level(w)) : 0);
    return 1;
}

static int l_world_dirty(lua_State *L) {
    World *w = world_from(L);
    lua_pushboolean(L, w ? world_dirty(w) : 0);
    return 1;
}

static int l_world_algorithm(lua_State *L) {
    World *w = world_from(L);
    lua_pushstring(L, w ? world_generator_name(w) : "");
    return 1;
}

static int l_world_seed(lua_State *L) {
    World *w = world_from(L);
    lua_pushinteger(L, w ? (lua_Integer)world_seed(w) : 0);
    return 1;
}

static int l_world_set_seed(lua_State *L) {
    World *w = world_from(L);
    lua_Integer seed = luaL_checkinteger(L, 1);
    if (seed < 0) seed = 0;
    if (w) world_set_seed(w, (uint32_t)seed);
    return 0;
}

static int l_world_sea_level(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->sea_level : 0.0);
    return 1;
}
static int l_world_set_sea_level(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_sea_level(w, (float)luaL_checknumber(L, 1));
    return 0;
}
static int l_world_warmth(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->warmth : 0.0);
    return 1;
}
static int l_world_set_warmth(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_warmth(w, (float)luaL_checknumber(L, 1));
    return 0;
}
static int l_world_mountain_level(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->mountain_level : 0.0);
    return 1;
}
static int l_world_set_mountain_level(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_mountain_level(w, (float)luaL_checknumber(L, 1));
    return 0;
}
static int l_world_noise_scale(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->noise_scale : 0.0);
    return 1;
}
static int l_world_set_noise_scale(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_noise_scale(w, (float)luaL_checknumber(L, 1));
    return 0;
}

static int l_world_rain_shadow(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->rain_shadow : 0.0);
    return 1;
}
static int l_world_set_rain_shadow(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_rain_shadow(w, (float)luaL_checknumber(L, 1));
    return 0;
}
static int l_world_moisture_reach(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->moisture_reach : 0.0);
    return 1;
}
static int l_world_set_moisture_reach(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_moisture_reach(w, (float)luaL_checknumber(L, 1));
    return 0;
}
static int l_world_hydrology(lua_State *L) {
    World *w = world_from(L);
    lua_pushboolean(L, w ? world_hydrology_enabled(w) : 0);
    return 1;
}
static int l_world_set_hydrology(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_hydrology(w, lua_toboolean(L, 1));
    return 0;
}
static int l_world_river_density(lua_State *L) {
    World *w = world_from(L);
    lua_pushnumber(L, w ? world_params(w)->river_density : 0.0);
    return 1;
}
static int l_world_set_river_density(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_river_density(w, (float)luaL_checknumber(L, 1));
    return 0;
}
static int l_world_plate_count(lua_State *L) {
    World *w = world_from(L);
    lua_pushinteger(L, w ? world_params(w)->plate_count : 0);
    return 1;
}
static int l_world_set_plate_count(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_plate_count(w, (int32_t)luaL_checkinteger(L, 1));
    return 0;
}

static int l_world_view(lua_State *L) {
    World *w = world_from(L);
    lua_pushinteger(L, w ? world_view(w) : 0);
    return 1;
}
static int l_world_set_view(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_view(w, (int32_t)luaL_checkinteger(L, 1));
    return 0;
}

static int l_world_generating(lua_State *L) {
    World *w = world_from(L);
    lua_pushboolean(L, w ? world_generating(w) : 0);
    return 1;
}
static int l_world_stage(lua_State *L) {
    World *w = world_from(L);
    if (!w) { lua_pushstring(L, ""); lua_pushinteger(L, 0); lua_pushinteger(L, 0); return 3; }
    lua_pushstring(L, world_gen_stage_name(w));
    lua_pushinteger(L, world_gen_stage(w));
    lua_pushinteger(L, world_gen_stage_count() - 1);
    return 3;
}

static void push_tile_info(lua_State *L, const WorldTileInfo *info) {
    lua_createtable(L, 0, 15);
    lua_pushinteger(L, info->cell);         lua_setfield(L, -2, "cell");
    lua_pushstring(L, tile_terrain_name(info->terrain)); lua_setfield(L, -2, "terrain");
    lua_pushnumber(L, info->elevation);     lua_setfield(L, -2, "elevation");
    lua_pushnumber(L, info->temperature);   lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, info->humidity);      lua_setfield(L, -2, "humidity");
    lua_pushnumber(L, info->rainfall);      lua_setfield(L, -2, "rainfall");
    lua_pushnumber(L, info->flow);          lua_setfield(L, -2, "flow");
    lua_pushinteger(L, info->river);        lua_setfield(L, -2, "river");
    lua_pushinteger(L, info->region);       lua_setfield(L, -2, "region");
    lua_pushinteger(L, info->plate);        lua_setfield(L, -2, "plate");
    lua_pushinteger(L, info->fault);        lua_setfield(L, -2, "fault");
    lua_pushnumber(L, info->stress);        lua_setfield(L, -2, "stress");
    lua_pushnumber(L, info->lat);           lua_setfield(L, -2, "lat");
    lua_pushnumber(L, info->lon);           lua_setfield(L, -2, "lon");
    lua_pushinteger(L, info->neighbors);    lua_setfield(L, -2, "neighbors");
}

static int l_world_selection(lua_State *L) {
    World *w = world_from(L);
    WorldTileInfo info;
    if (!w || !world_selected_info(w, &info)) { lua_pushnil(L); return 1; }
    push_tile_info(L, &info);
    return 1;
}
static int l_world_clear_selection(lua_State *L) {
    World *w = world_from(L);
    if (w) world_clear_selection(w);
    return 0;
}

static int l_world_neighbor_count(lua_State *L) {
    World *w = world_from(L);
    lua_pushinteger(L, w ? world_neighbor_count(w) : 0);
    return 1;
}

static int l_world_neighbor(lua_State *L) {
    World *w = world_from(L);
    int32_t slot = (int32_t)luaL_checkinteger(L, 1) - 1;
    WorldTileInfo info;
    if (!w || !world_neighbor_info(w, slot, &info)) { lua_pushnil(L); return 1; }
    push_tile_info(L, &info);
    return 1;
}

static int l_world_set_input_suppressed(lua_State *L) {
    World *w = world_from(L);
    if (w) world_set_input_suppressed(w, lua_toboolean(L, 1));
    return 0;
}

static int l_world_screen_pos(lua_State *L) {
    World *w = world_from(L);
    float x, y;
    if (!w || !world_selected_screen_pos(w, &x, &y)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, y); lua_setfield(L, -2, "y");
    return 1;
}

void world_lua_register(ScriptHost *host, World *world) {
    lua_State *L = ScriptHost_state(host);
    if (!L) return;

    lua_pushlightuserdata(L, world);
    lua_setfield(L, LUA_REGISTRYINDEX, WORLD_REGISTRY_KEY);

    ScriptHost_register_function(host, "world", "regenerate",     l_world_regenerate);
    ScriptHost_register_function(host, "world", "reroll",         l_world_reroll);
    ScriptHost_register_function(host, "world", "next_algorithm", l_world_next_algorithm);
    ScriptHost_register_function(host, "world", "set_level",      l_world_set_level);
    ScriptHost_register_function(host, "world", "level_up",       l_world_level_up);
    ScriptHost_register_function(host, "world", "level_down",     l_world_level_down);
    ScriptHost_register_function(host, "world", "level",          l_world_level);
    ScriptHost_register_function(host, "world", "cells",          l_world_cells);
    ScriptHost_register_function(host, "world", "algorithm",      l_world_algorithm);
    ScriptHost_register_function(host, "world", "seed",           l_world_seed);
    ScriptHost_register_function(host, "world", "set_seed",       l_world_set_seed);

    ScriptHost_register_function(host, "world", "sea_level",          l_world_sea_level);
    ScriptHost_register_function(host, "world", "set_sea_level",      l_world_set_sea_level);
    ScriptHost_register_function(host, "world", "warmth",             l_world_warmth);
    ScriptHost_register_function(host, "world", "set_warmth",         l_world_set_warmth);
    ScriptHost_register_function(host, "world", "mountain_level",     l_world_mountain_level);
    ScriptHost_register_function(host, "world", "set_mountain_level", l_world_set_mountain_level);
    ScriptHost_register_function(host, "world", "noise_scale",        l_world_noise_scale);
    ScriptHost_register_function(host, "world", "set_noise_scale",    l_world_set_noise_scale);

    ScriptHost_register_function(host, "world", "rain_shadow",        l_world_rain_shadow);
    ScriptHost_register_function(host, "world", "set_rain_shadow",    l_world_set_rain_shadow);
    ScriptHost_register_function(host, "world", "moisture_reach",     l_world_moisture_reach);
    ScriptHost_register_function(host, "world", "set_moisture_reach", l_world_set_moisture_reach);
    ScriptHost_register_function(host, "world", "hydrology",          l_world_hydrology);
    ScriptHost_register_function(host, "world", "set_hydrology",      l_world_set_hydrology);
    ScriptHost_register_function(host, "world", "river_density",      l_world_river_density);
    ScriptHost_register_function(host, "world", "set_river_density",  l_world_set_river_density);
    ScriptHost_register_function(host, "world", "plate_count",        l_world_plate_count);
    ScriptHost_register_function(host, "world", "set_plate_count",    l_world_set_plate_count);

    ScriptHost_register_function(host, "world", "view",               l_world_view);
    ScriptHost_register_function(host, "world", "set_view",           l_world_set_view);

    ScriptHost_register_function(host, "world", "generating",         l_world_generating);
    ScriptHost_register_function(host, "world", "stage",              l_world_stage);

    ScriptHost_register_function(host, "world", "selection",          l_world_selection);
    ScriptHost_register_function(host, "world", "clear_selection",    l_world_clear_selection);
    ScriptHost_register_function(host, "world", "neighbor_count",     l_world_neighbor_count);
    ScriptHost_register_function(host, "world", "neighbor",           l_world_neighbor);
    ScriptHost_register_function(host, "world", "screen_pos",         l_world_screen_pos);
    ScriptHost_register_function(host, "world", "set_input_suppressed", l_world_set_input_suppressed);

    ScriptHost_register_function(host, "world", "dirty",              l_world_dirty);
}
