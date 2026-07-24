#ifndef COMPONENTS_SPRITE_H
#define COMPONENTS_SPRITE_H

#include "raylib.h"

// POD components for the sprite render path. Registered in main.c; type ids
// live in GlobalState (transform_type / sprite_type).

typedef struct Transform2D {
    float x, y;
    float rot;    // degrees
    float scale;
} Transform2D;

typedef struct SpriteComp {
    int texture;    // TextureStore id; 0 = untextured (draws a w x h tinted quad)
    Rectangle src;  // source rect in the texture (ignored when texture == 0)
    float w, h;     // world size, centered on the transform
    Color tint;
} SpriteComp;

#endif
