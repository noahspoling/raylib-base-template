#ifndef COMPONENTS_ATLAS_H
#define COMPONENTS_ATLAS_H

#include <stdint.h>
#include "raylib.h"
#include "array.h"

typedef struct Atlas {
    int32_t texture;
    Array_T frames;
    int32_t count;
} Atlas;

Atlas *Atlas_new(int32_t texture_id);
void Atlas_free(Atlas **atlas);

int32_t Atlas_add_frame(Atlas *atlas, Rectangle src);
int32_t Atlas_add_grid(Atlas *atlas, int32_t cell_w, int32_t cell_h, int32_t cols, int32_t rows);
Rectangle Atlas_frame(const Atlas *atlas, int32_t frame);
Texture2D Atlas_texture(const Atlas *atlas);
void Atlas_draw(const Atlas *atlas, int32_t frame, Rectangle dest, Vector2 origin,
                float rotation, Color tint);

#endif
