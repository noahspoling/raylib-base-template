#ifndef SCENE_H
#define SCENE_H

#include <stddef.h>
#include "gramarye_ecs/ecs.h"
#include "gramarye_ecs/system.h"

typedef struct Scene Scene;

Scene *Scene_new(Arena_T arena, const char *name);
void Scene_add_system(Scene *scene, SystemId id);
void Scene_run(Scene *scene, ECS *ecs, float deltaTime);

#endif
