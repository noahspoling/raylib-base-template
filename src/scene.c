#include "scene.h"
#include "arena.h"
#include <stdlib.h>

#define SCENE_SYSTEMS_CAP 32

struct Scene {
    const char *name;
    SystemId *system_ids;
    size_t count;
    size_t capacity;
};

static int system_priority_cmp(const void *a, const void *b) {
    const System *sa = *(const System *const *)a;
    const System *sb = *(const System *const *)b;
    if (sa->priority < sb->priority) return -1;
    if (sa->priority > sb->priority) return 1;
    return 0;
}

Scene *Scene_new(Arena_T arena, const char *name) {
    if (!arena || !name) return NULL;
    Scene *scene = (Scene *)Arena_alloc(arena, sizeof(Scene), __FILE__, __LINE__);
    if (!scene) return NULL;
    scene->name = name;
    scene->system_ids = (SystemId *)Arena_alloc(arena, sizeof(SystemId) * SCENE_SYSTEMS_CAP, __FILE__, __LINE__);
    if (!scene->system_ids) return NULL;
    scene->count = 0;
    scene->capacity = SCENE_SYSTEMS_CAP;
    return scene;
}

void Scene_add_system(Scene *scene, SystemId id) {
    if (!scene || id == SYSTEM_INVALID) return;
    if (scene->count >= scene->capacity) return;
    scene->system_ids[scene->count++] = id;
}

void Scene_run(Scene *scene, ECS *ecs, float deltaTime) {
    if (!scene || !ecs || scene->count == 0) return;

    System *systems[32];
    size_t n = 0;
    for (size_t i = 0; i < scene->count && n < 32; i++) {
        System *s = ECS_get_system(ecs, scene->system_ids[i]);
        if (s && s->enabled && s->updateFunc) {
            systems[n++] = s;
        }
    }
    if (n == 0) return;

    qsort(systems, n, sizeof(System *), system_priority_cmp);

    for (size_t i = 0; i < n; i++) {
        ECS_update_system(ecs, systems[i]->id, deltaTime);
    }
}
