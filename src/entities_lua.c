#include "entities_lua.h"
#include "components/sprite.h"
#include "services/texture_store.h"
#include "raylib.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

#if defined(__ANDROID__)
#define ASSET_PREFIX ""
#else
#define ASSET_PREFIX "assets/"
#endif

static const char *ENTITY_MT = "gramarye.entity";
static const char *ECS_REGISTRY_KEY = "gramarye.entities.ecs";
static const char *STATE_REGISTRY_KEY = "gramarye.entities.state";

static ECS *ecs_from(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, ECS_REGISTRY_KEY);
    ECS *ecs = (ECS *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ecs;
}

static GlobalState *state_from(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, STATE_REGISTRY_KEY);
    GlobalState *state = (GlobalState *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return state;
}

static EntityId *check_entity(lua_State *L, int idx) {
    return (EntityId *)luaL_checkudata(L, idx, ENTITY_MT);
}

static int l_entity_eq(lua_State *L) {
    EntityId *a = check_entity(L, 1);
    EntityId *b = check_entity(L, 2);
    lua_pushboolean(L, a->high == b->high && a->low == b->low);
    return 1;
}

static int l_entity_tostring(lua_State *L) {
    EntityId *e = check_entity(L, 1);
    lua_pushfstring(L, "entity(%p:%p)", (void *)(uintptr_t)e->high, (void *)(uintptr_t)e->low);
    return 1;
}

// gramarye.entities.spawn() -> entity
static int l_entities_spawn(lua_State *L) {
    ECS *ecs = ecs_from(L);
    EntityId id = Entity_create(ECS_get_entity_registry(ecs));
    EntityId *ud = (EntityId *)lua_newuserdatauv(L, sizeof(EntityId), 0);
    *ud = id;
    luaL_setmetatable(L, ENTITY_MT);
    return 1;
}

// gramarye.entities.despawn(entity)
static int l_entities_despawn(lua_State *L) {
    ECS *ecs = ecs_from(L);
    GlobalState *state = state_from(L);
    EntityId *e = check_entity(L, 1);
    if (ECS_has_component(ecs, *e, state->sprite_type))
        ECS_remove_component(ecs, *e, state->sprite_type);
    if (ECS_has_component(ecs, *e, state->transform_type))
        ECS_remove_component(ecs, *e, state->transform_type);
    Entity_destroy(ECS_get_entity_registry(ecs), *e);
    return 0;
}

// gramarye.entities.set_transform(entity, x, y [, rot [, scale]])
// Updates in place when the component exists — the hot path (per-frame moves
// from Lua) does no ECS allocation.
static int l_entities_set_transform(lua_State *L) {
    ECS *ecs = ecs_from(L);
    GlobalState *state = state_from(L);
    EntityId *e = check_entity(L, 1);
    Transform2D tf;
    tf.x = (float)luaL_checknumber(L, 2);
    tf.y = (float)luaL_checknumber(L, 3);
    tf.rot = (float)luaL_optnumber(L, 4, 0.0);
    tf.scale = (float)luaL_optnumber(L, 5, 1.0);

    Transform2D *existing = (Transform2D *)ECS_get_component(ecs, *e, state->transform_type);
    if (existing) {
        *existing = tf;
    } else {
        ECS_add_component(ecs, *e, state->transform_type, &tf);
    }
    return 0;
}

// gramarye.entities.set_sprite(entity, tex_id, w, h [, r,g,b,a [, sx,sy,sw,sh]])
static int l_entities_set_sprite(lua_State *L) {
    ECS *ecs = ecs_from(L);
    GlobalState *state = state_from(L);
    EntityId *e = check_entity(L, 1);
    SpriteComp sp;
    memset(&sp, 0, sizeof(sp));
    sp.texture = (int)luaL_checkinteger(L, 2);
    sp.w = (float)luaL_checknumber(L, 3);
    sp.h = (float)luaL_checknumber(L, 4);
    sp.tint.r = (unsigned char)luaL_optinteger(L, 5, 255);
    sp.tint.g = (unsigned char)luaL_optinteger(L, 6, 255);
    sp.tint.b = (unsigned char)luaL_optinteger(L, 7, 255);
    sp.tint.a = (unsigned char)luaL_optinteger(L, 8, 255);
    if (lua_gettop(L) >= 12) {
        sp.src.x = (float)luaL_checknumber(L, 9);
        sp.src.y = (float)luaL_checknumber(L, 10);
        sp.src.width = (float)luaL_checknumber(L, 11);
        sp.src.height = (float)luaL_checknumber(L, 12);
    } else if (sp.texture != 0) {
        Texture2D tex = TextureStore_get(sp.texture);
        sp.src = (Rectangle){ 0, 0, (float)tex.width, (float)tex.height };
    }

    SpriteComp *existing = (SpriteComp *)ECS_get_component(ecs, *e, state->sprite_type);
    if (existing) {
        *existing = sp;
    } else {
        ECS_add_component(ecs, *e, state->sprite_type, &sp);
    }
    return 0;
}

// gramarye.textures.load(path) -> id (0 on failure; path relative to assets/)
static int l_textures_load(lua_State *L) {
    const char *rel = luaL_checkstring(L, 1);
    char path[256];
    snprintf(path, sizeof(path), "%s%s", ASSET_PREFIX, rel);
    lua_pushinteger(L, TextureStore_load(path));
    return 1;
}

void entities_lua_register(ScriptHost *host, ECS *ecs, GlobalState *state) {
    lua_State *L = ScriptHost_state(host);
    if (!L) return;

    lua_pushlightuserdata(L, ecs);
    lua_setfield(L, LUA_REGISTRYINDEX, ECS_REGISTRY_KEY);
    lua_pushlightuserdata(L, state);
    lua_setfield(L, LUA_REGISTRYINDEX, STATE_REGISTRY_KEY);

    if (luaL_newmetatable(L, ENTITY_MT)) {
        lua_pushcfunction(L, l_entity_eq);
        lua_setfield(L, -2, "__eq");
        lua_pushcfunction(L, l_entity_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_pop(L, 1);

    ScriptHost_register_function(host, "entities", "spawn", l_entities_spawn);
    ScriptHost_register_function(host, "entities", "despawn", l_entities_despawn);
    ScriptHost_register_function(host, "entities", "set_transform", l_entities_set_transform);
    ScriptHost_register_function(host, "entities", "set_sprite", l_entities_set_sprite);
    ScriptHost_register_function(host, "textures", "load", l_textures_load);
}
