#ifndef SCRIPT_HOST_H
#define SCRIPT_HOST_H

#include <stdbool.h>
#include "arena.h"
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"

// Lua scene host. Scenes are Lua files at assets/scripts/scenes/<name>.lua
// that return a table of hooks: on_enter, on_update(dt), on_draw, on_exit,
// and (for the scene stack) on_pause / on_resume. Lua orchestrates (which
// systems run, UI, transitions); heavy per-frame work stays in C systems.
//
// Timing model: C systems of the active scene run at a fixed timestep via
// ScriptHost_update_fixed (deterministic simulation); on_update/on_draw run
// once per render frame via ScriptHost_update/ScriptHost_draw, so raylib's
// per-frame input edges (key_pressed) behave naturally in Lua.
//
// Scenes form a stack: change() replaces the whole stack, push() suspends the
// top scene (on_pause) and enters a new one, pop() exits the top (on_exit)
// and resumes the one below (on_resume). Only the top scene updates/draws.

typedef struct lua_State lua_State;
typedef struct ScriptHost ScriptHost;

ScriptHost *ScriptHost_new(Arena_T arena, ECS *ecs, GlobalState *global_state);
void ScriptHost_dispose(ScriptHost *host);

// Arena rewound on every full scene change (not on push/pop). Scene-lifetime
// C allocations go here.
Arena_T ScriptHost_scene_arena(ScriptHost *host);

// Make a C system addressable from Lua as gramarye.systems.add/enable(name).
void ScriptHost_register_system(ScriptHost *host, const char *name, SystemId id);

// Install an extra C function as gramarye.<module>.<name> (module NULL = root).
void ScriptHost_register_function(ScriptHost *host, const char *module,
                                  const char *name, int (*fn)(lua_State *));

bool ScriptHost_load_scene(ScriptHost *host, const char *scene_name);
// Deferred transitions (back gramarye.scene.change/push/pop); applied after
// on_update, never mid-hook.
void ScriptHost_request_scene(ScriptHost *host, const char *scene_name);
void ScriptHost_request_push(ScriptHost *host, const char *scene_name);
void ScriptHost_request_pop(ScriptHost *host);

// Fixed-step simulation: runs the active scene's C systems. Call 0..N times
// per render frame with a constant dt.
void ScriptHost_update_fixed(ScriptHost *host, float dt);
// Per-render-frame: Lua on_update(dt), then applies pending scene transitions.
void ScriptHost_update(ScriptHost *host, float dt);
void ScriptHost_draw(ScriptHost *host);

bool ScriptHost_reload_current(ScriptHost *host);
lua_State *ScriptHost_state(ScriptHost *host);

#endif
