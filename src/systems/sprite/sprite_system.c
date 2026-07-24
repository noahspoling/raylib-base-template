#include "systems/sprite/sprite_system.h"
#include "components/sprite.h"
#include "services/texture_store.h"
#include "raylib.h"

static void draw_sprite_cb(ECS *ecs, EntityId entity, void *component, void *userData) {
    GlobalState *state = (GlobalState *)userData;
    SpriteComp *sprite = (SpriteComp *)component;
    Transform2D *tf = (Transform2D *)ECS_get_component(ecs, entity, state->transform_type);
    if (!tf) return;

    float w = sprite->w * tf->scale;
    float h = sprite->h * tf->scale;
    Rectangle dest = { tf->x, tf->y, w, h };
    Vector2 origin = { w / 2.0f, h / 2.0f };  // rotate/scale around the center

    if (sprite->texture != 0) {
        Texture2D tex = TextureStore_get(sprite->texture);
        if (tex.id != 0) {
            DrawTexturePro(tex, sprite->src, dest, origin, tf->rot, sprite->tint);
            return;
        }
    }
    DrawRectanglePro(dest, origin, tf->rot, sprite->tint);
}

static void sprite_system_update(ECS *ecs, float deltaTime, void *userData) {
    (void)deltaTime;
    GlobalState *state = (GlobalState *)userData;
    if (!state) return;
    BeginMode2D(state->camera);
    ECS_storage_iterate(ecs, state->sprite_type, draw_sprite_cb, state);
    EndMode2D();
}

SystemId sprite_system_register(ECS *ecs, GlobalState *state) {
    return ECS_register_system_with_priority(ecs, "sprite_render",
                                             NULL, 0,
                                             sprite_system_update, state,
                                             100);
}
