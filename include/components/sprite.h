#ifndef COMPONENTS_SPRITE_H
#define COMPONENTS_SPRITE_H

#include "raylib.h"

typedef struct Transform2D {
    float x, y;
    float rot;
    float scale;
} Transform2D;

typedef struct SpriteComp {
    int texture;
    Rectangle src;
    float w, h;
    Color tint;
} SpriteComp;

#endif
