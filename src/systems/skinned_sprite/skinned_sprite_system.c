#include "systems/skinned_sprite/skinned_sprite_system.h"
#include <stdint.h>
#include "components/skinned_sprite.h"
#include "components/sprite.h"
#include "services/stores/texture_store.h"
#include "raylib.h"

#if defined(__ANDROID__)
#define ASSET_PREFIX ""
#else
#define ASSET_PREFIX "assets/"
#endif

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define SKIN_GLSL_VERSION 100
#else
#define SKIN_GLSL_VERSION 330
#endif

static Shader g_skin_shader;
static int32_t g_texture1_loc = -1;
static int32_t g_texel_size_loc = -1;
static bool g_shader_loaded = false;

static void draw_skinned_sprite_cb(ECS *ecs, EntityId entity, void *component, void *userData) {
    GlobalState *state = (GlobalState *)userData;
    SkinnedSpriteComp *skin = (SkinnedSpriteComp *)component;
    Transform2D *tf = (Transform2D *)ECS_get_component(ecs, entity, state->transform_type);
    if (!tf) return;

    Texture2D anim = TextureStore_get(skin->anim_texture);
    Texture2D skinTex = TextureStore_get(skin->skin_texture);
    if (anim.id == 0 || skinTex.id == 0) return;

    Vector2 texelSize = { 1.0f / (float)skinTex.width, 1.0f / (float)skinTex.height };
    SetShaderValueTexture(g_skin_shader, g_texture1_loc, skinTex);
    SetShaderValue(g_skin_shader, g_texel_size_loc, &texelSize, SHADER_UNIFORM_VEC2);

    float w = skin->w * tf->scale;
    float h = skin->h * tf->scale;
    Rectangle dest = { tf->x, tf->y, w, h };
    Vector2 origin = { w / 2.0f, h / 2.0f };
    DrawTexturePro(anim, skin->src, dest, origin, tf->rot, skin->tint);
}

static void skinned_sprite_system_update(ECS *ecs, float deltaTime, void *userData) {
    (void)deltaTime;
    GlobalState *state = (GlobalState *)userData;
    if (!state || !g_shader_loaded) return;
    BeginMode2D(state->camera);
    BeginShaderMode(g_skin_shader);
    ECS_storage_iterate(ecs, state->skinned_sprite_type, draw_skinned_sprite_cb, state);
    EndShaderMode();
    EndMode2D();
}

SystemId skinned_sprite_system_register(ECS *ecs, GlobalState *state) {
    if (!g_shader_loaded) {
        const char *fs = TextFormat(ASSET_PREFIX "shaders/glsl%d/skin.fs", SKIN_GLSL_VERSION);
        g_skin_shader = LoadShader(0, fs);
        if (g_skin_shader.id == 0) {
            TraceLog(LOG_WARNING, "SKIN: failed to load skin shader from '%s'", fs);
        } else {
            g_texture1_loc = GetShaderLocation(g_skin_shader, "texture1");
            g_texel_size_loc = GetShaderLocation(g_skin_shader, "skinTexelSize");
            g_shader_loaded = true;
        }
    }
    return ECS_register_system_with_priority(ecs, "skinned_sprite_render",
                                             NULL, 0,
                                             skinned_sprite_system_update, state,
                                             100);
}

void skinned_sprite_system_shutdown(void) {
    if (g_shader_loaded) {
        UnloadShader(g_skin_shader);
        g_shader_loaded = false;
        g_texture1_loc = -1;
        g_texel_size_loc = -1;
    }
}
