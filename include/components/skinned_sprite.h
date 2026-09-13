#ifndef COMPONENTS_SKINNED_SPRITE_H
#define COMPONENTS_SKINNED_SPRITE_H

#include "raylib.h"

typedef struct SkinnedSpriteComp {
    int anim_texture;
    int skin_texture;
    Rectangle src;
    float w, h;
    Color tint;
} SkinnedSpriteComp;

#endif
