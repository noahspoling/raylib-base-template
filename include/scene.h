#ifndef SCENE_H
#define SCENE_H

#include <stddef.h>
#include <stdbool.h>
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"

// A Scene is a small value type: a name plus the C systems that run while it
// is active. It is embedded by value in ScriptHost's scene stack, so scene
// transitions allocate nothing. The resolved System* list is cached and kept
// sorted by priority; it is rebuilt only when the system set changes.

#define SCENE_MAX_SYSTEMS 32

typedef struct Scene {
    const char *name;                       // points at owner's storage
    SystemId system_ids[SCENE_MAX_SYSTEMS];
    size_t count;
    System *sorted[SCENE_MAX_SYSTEMS];      // cache: resolved + priority-sorted
    size_t sorted_count;
    bool dirty;                             // system set changed since last run
} Scene;

void Scene_init(Scene *scene, const char *name);
void Scene_add_system(Scene *scene, SystemId id);
void Scene_run(Scene *scene, ECS *ecs, float deltaTime);

#endif
