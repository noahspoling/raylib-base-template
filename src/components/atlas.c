#include "components/atlas.h"
#include <stdint.h>
#include "services/stores/texture_store.h"
#include <stddef.h>
#include "mem.h"

#define ATLAS_FRAME_HINT 8

Atlas *Atlas_new(int32_t texture_id) {
    if (!TextureStore_acquire(texture_id)) return NULL;
    Atlas *atlas;
    NEW(atlas);
    atlas->texture = texture_id;
    atlas->frames = Array_new(ATLAS_FRAME_HINT, sizeof(Rectangle));
    atlas->count = 0;
    return atlas;
}

void Atlas_free(Atlas **atlas) {
    if (!atlas || !*atlas) return;
    TextureStore_release((*atlas)->texture);
    Array_free(&(*atlas)->frames);
    FREE(*atlas);
}

int32_t Atlas_add_frame(Atlas *atlas, Rectangle src) {
    if (atlas->count == Array_length(atlas->frames)) {
        Array_resize(atlas->frames, atlas->count * 2);
    }
    Array_put(atlas->frames, atlas->count, &src);
    return atlas->count++;
}

int32_t Atlas_add_grid(Atlas *atlas, int32_t cell_w, int32_t cell_h, int32_t cols, int32_t rows) {
    int32_t first = atlas->count;
    for (int32_t y = 0; y < rows; y++) {
        for (int32_t x = 0; x < cols; x++) {
            Rectangle src = { (float)(x * cell_w), (float)(y * cell_h),
                              (float)cell_w, (float)cell_h };
            Atlas_add_frame(atlas, src);
        }
    }
    return first;
}

Rectangle Atlas_frame(const Atlas *atlas, int32_t frame) {
    if (!atlas || frame < 0 || frame >= atlas->count) return (Rectangle){ 0 };
    return *(Rectangle *)Array_get(atlas->frames, frame);
}

Texture2D Atlas_texture(const Atlas *atlas) {
    return atlas ? TextureStore_get(atlas->texture) : (Texture2D){ 0 };
}

void Atlas_draw(const Atlas *atlas, int32_t frame, Rectangle dest, Vector2 origin,
                float rotation, Color tint) {
    Texture2D tex = Atlas_texture(atlas);
    if (tex.id == 0 || frame < 0 || frame >= atlas->count) return;
    DrawTexturePro(tex, Atlas_frame(atlas, frame), dest, origin, rotation, tint);
}
