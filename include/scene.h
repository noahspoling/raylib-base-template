#ifndef SCENE_H
#define SCENE_H

#include <stddef.h>
#include <stdbool.h>
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"

#define SCENE_MAX_SYSTEMS 32

typedef struct Scene {
    const char *name;
    SystemId system_ids[SCENE_MAX_SYSTEMS];
    size_t count;
    System *sorted[SCENE_MAX_SYSTEMS];
    size_t sorted_count;
    bool dirty;
} Scene;

void Scene_init(Scene *scene, const char *name);
void Scene_add_system(Scene *scene, SystemId id);
void Scene_run(Scene *scene, ECS *ecs, float deltaTime);

#endif
