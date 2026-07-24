#include "scene.h"
#include "raylib.h"
#include <string.h>

void Scene_init(Scene *scene, const char *name) {
    if (!scene) return;
    memset(scene, 0, sizeof(*scene));
    scene->name = name;
}

void Scene_add_system(Scene *scene, SystemId id) {
    if (!scene || id == SYSTEM_INVALID) return;
    if (scene->count >= SCENE_MAX_SYSTEMS) {
        TraceLog(LOG_WARNING, "SCENE: '%s' system list full (%d), dropping system %u",
                 scene->name ? scene->name : "?", SCENE_MAX_SYSTEMS, id);
        return;
    }
    scene->system_ids[scene->count++] = id;
    scene->dirty = true;
}

// Rebuild the resolved System* cache, insertion-sorted by priority (stable,
// n <= SCENE_MAX_SYSTEMS). Runs only when the system set changed.
static void scene_rebuild_cache(Scene *scene, ECS *ecs) {
    scene->sorted_count = 0;
    for (size_t i = 0; i < scene->count; i++) {
        System *s = ECS_get_system(ecs, scene->system_ids[i]);
        if (!s) {
            TraceLog(LOG_WARNING, "SCENE: '%s' references unknown system %u",
                     scene->name ? scene->name : "?", scene->system_ids[i]);
            continue;
        }
        size_t j = scene->sorted_count;
        while (j > 0 && scene->sorted[j - 1]->priority > s->priority) {
            scene->sorted[j] = scene->sorted[j - 1];
            j--;
        }
        scene->sorted[j] = s;
        scene->sorted_count++;
    }
    scene->dirty = false;
}

void Scene_run(Scene *scene, ECS *ecs, float deltaTime) {
    if (!scene || !ecs || scene->count == 0) return;
    if (scene->dirty) scene_rebuild_cache(scene, ecs);

    for (size_t i = 0; i < scene->sorted_count; i++) {
        System *s = scene->sorted[i];
        if (s->enabled && s->updateFunc) {
            ECS_update_system(ecs, s->id, deltaTime);
        }
    }
}
