#ifndef SCRIPT_HOST_H
#define SCRIPT_HOST_H

#include <stdbool.h>
#include "arena.h"
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

typedef struct lua_State lua_State;
typedef struct ScriptHost ScriptHost;

ScriptHost *ScriptHost_new(Arena_T arena, ECS *ecs, GlobalState *global_state);
void ScriptHost_dispose(ScriptHost *host);

Arena_T ScriptHost_scene_arena(ScriptHost *host);

void ScriptHost_register_system(ScriptHost *host, const char *name, SystemId id);

void ScriptHost_register_function(ScriptHost *host, const char *module,
                                  const char *name, int (*fn)(lua_State *));

bool ScriptHost_load_scene(ScriptHost *host, const char *scene_name);
void ScriptHost_request_scene(ScriptHost *host, const char *scene_name);
void ScriptHost_request_push(ScriptHost *host, const char *scene_name);
void ScriptHost_request_pop(ScriptHost *host);

void ScriptHost_update_fixed(ScriptHost *host, float dt);
void ScriptHost_update(ScriptHost *host, float dt);
void ScriptHost_draw(ScriptHost *host);

bool ScriptHost_reload_current(ScriptHost *host);
lua_State *ScriptHost_state(ScriptHost *host);

#endif
