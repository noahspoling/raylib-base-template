#ifndef COMPONENTS_SKINNED_SPRITE_H
#define COMPONENTS_SKINNED_SPRITE_H

#include <stdint.h>
#include "raylib.h"

typedef struct SkinnedSpriteComp {
    int32_t anim_texture;
    int32_t skin_texture;
    Rectangle src;
    float w, h;
    Color tint;
} SkinnedSpriteComp;

#endif
