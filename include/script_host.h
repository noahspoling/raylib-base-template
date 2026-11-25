#ifndef SCRIPT_HOST_H
#define SCRIPT_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include "arena.h"
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"
#include "states/global_state.h"
#include "services/mounts/mounts.h"

#define SCRIPT_HOST_MAX_TRACKED 128

typedef struct ScriptHostLoadedFile {
    char    rel[128];
    char    path[MOUNTS_PATH_MAX];
    int32_t mount;
    int32_t below;
    long    mtime;
} ScriptHostLoadedFile;

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

void ScriptHost_clear_modules(ScriptHost *host);
void ScriptHost_unload_all(ScriptHost *host);

int32_t ScriptHost_loaded_count(ScriptHost *host);
const ScriptHostLoadedFile *ScriptHost_loaded_get(ScriptHost *host, int32_t index);

#endif
