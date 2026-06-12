#ifndef SCRIPT_HOST_H
#define SCRIPT_HOST_H

#include <stdbool.h>
#include "arena.h"
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

// Lua scene host. Scenes are Lua files at assets/scripts/scenes/<name>.lua
// that return a table of hooks: on_enter, on_update(dt), on_draw, on_exit.
// Lua orchestrates (which systems run, UI, transitions); heavy per-frame work
// stays in C systems registered through the ECS.

typedef struct lua_State lua_State;
typedef struct ScriptHost ScriptHost;

ScriptHost *ScriptHost_new(Arena_T arena, ECS *ecs, GlobalState *global_state);
void ScriptHost_dispose(ScriptHost *host);

// Make a C system addressable from Lua as gramarye.systems.add/enable(name).
void ScriptHost_register_system(ScriptHost *host, const char *name, SystemId id);

// Install an extra C function as gramarye.<module>.<name> (module NULL = root).
// Hook point for future libraries (e.g. gramarye-ui installing gramarye.ui.*).
void ScriptHost_register_function(ScriptHost *host, const char *module,
                                  const char *name, int (*fn)(lua_State *));

bool ScriptHost_load_scene(ScriptHost *host, const char *scene_name);
// Deferred transition (backs gramarye.scene.change); applied after on_update.
void ScriptHost_request_scene(ScriptHost *host, const char *scene_name);

void ScriptHost_update(ScriptHost *host, float dt);
void ScriptHost_draw(ScriptHost *host);

bool ScriptHost_reload_current(ScriptHost *host);
lua_State *ScriptHost_state(ScriptHost *host);

#endif
