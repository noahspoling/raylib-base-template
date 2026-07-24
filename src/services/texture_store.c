#include "services/texture_store.h"
#include <stdio.h>
#include <string.h>

#define TEXTURE_STORE_CAP 64
#define TEXTURE_PATH_MAX 192

typedef struct {
    char path[TEXTURE_PATH_MAX];
    Texture2D texture;
} TextureSlot;

static TextureSlot g_slots[TEXTURE_STORE_CAP];
static int g_count;

void TextureStore_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    g_count = 0;
}

int TextureStore_load(const char *path) {
    if (!path || !path[0]) return 0;
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_slots[i].path, path) == 0) return i + 1;
    }
    if (g_count >= TEXTURE_STORE_CAP) {
        TraceLog(LOG_WARNING, "TEXTURES: store full (%d), cannot load '%s'",
                 TEXTURE_STORE_CAP, path);
        return 0;
    }
    Texture2D tex = LoadTexture(path);
    if (tex.id == 0) {
        TraceLog(LOG_WARNING, "TEXTURES: failed to load '%s'", path);
        return 0;
    }
    TextureSlot *slot = &g_slots[g_count++];
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->texture = tex;
    return g_count;
}

Texture2D TextureStore_get(int id) {
    if (id < 1 || id > g_count) return (Texture2D){ 0 };
    return g_slots[id - 1].texture;
}

void TextureStore_shutdown(void) {
    for (int i = 0; i < g_count; i++) {
        if (g_slots[i].texture.id != 0) UnloadTexture(g_slots[i].texture);
    }
    g_count = 0;
}
