#ifndef COMPONENTS_SPRITE_H
#define COMPONENTS_SPRITE_H

#include <stdint.h>
#include "raylib.h"

typedef struct Transform2D {
    float x, y;
    float rot;
    float scale;
} Transform2D;

typedef struct SpriteComp {
    int32_t texture;
    Rectangle src;
    float w, h;
    Color tint;
} SpriteComp;

#endif
