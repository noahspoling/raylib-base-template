#include "services/stores/texture_store.h"
#include <stddef.h>
#include <stdint.h>
#include "array.h"
#include "atom.h"
#include "table.h"

#define TEXTURE_STORE_HINT 64

typedef struct {
    const char *path;
    Texture2D texture;
    int32_t refs;
} TextureSlot;

static Array_T g_slots;
static Table_T g_by_path;
static int32_t g_count;

static TextureSlot *slot_at(int32_t id) {
    if (!g_slots || id < 1 || id > g_count) return NULL;
    return (TextureSlot *)Array_get(g_slots, id - 1);
}

void TextureStore_init(void) {
    g_slots = Array_new(TEXTURE_STORE_HINT, sizeof(TextureSlot));
    g_by_path = Table_new(TEXTURE_STORE_HINT, NULL, NULL);
    g_count = 0;
}

void TextureStore_shutdown(void) {
    if (!g_slots) return;
    for (int32_t i = 1; i <= g_count; i++) {
        TextureSlot *slot = slot_at(i);
        if (slot->texture.id != 0) UnloadTexture(slot->texture);
    }
    Table_free(&g_by_path);
    Array_free(&g_slots);
    g_count = 0;
}

int32_t TextureStore_register(const char *path) {
    if (!g_slots || !path || !path[0]) return 0;
    const char *key = Atom_string(path);
    void *found = Table_get(g_by_path, key);
    if (found) return (int32_t)(intptr_t)found;

    if (g_count == Array_length(g_slots)) {
        Array_resize(g_slots, g_count * 2);
    }
    TextureSlot slot = { .path = key, .texture = { 0 }, .refs = 0 };
    Array_put(g_slots, g_count, &slot);
    g_count++;
    Table_put(g_by_path, key, (void *)(intptr_t)g_count);
    return g_count;
}

bool TextureStore_acquire(int32_t id) {
    TextureSlot *slot = slot_at(id);
    if (!slot) return false;
    if (slot->refs == 0) {
        slot->texture = LoadTexture(slot->path);
        if (slot->texture.id == 0) {
            TraceLog(LOG_WARNING, "TEXTURES: failed to load '%s'", slot->path);
            return false;
        }
    }
    slot->refs++;
    return true;
}

void TextureStore_release(int32_t id) {
    TextureSlot *slot = slot_at(id);
    if (!slot || slot->refs == 0) return;
    if (--slot->refs == 0) {
        UnloadTexture(slot->texture);
        slot->texture = (Texture2D){ 0 };
    }
}

int32_t TextureStore_load(const char *path) {
    int32_t id = TextureStore_register(path);
    return TextureStore_acquire(id) ? id : 0;
}

bool TextureStore_is_resident(int32_t id) {
    TextureSlot *slot = slot_at(id);
    return slot && slot->texture.id != 0;
}

const char *TextureStore_path(int32_t id) {
    TextureSlot *slot = slot_at(id);
    return slot ? slot->path : NULL;
}

Texture2D TextureStore_get(int32_t id) {
    TextureSlot *slot = slot_at(id);
    return slot ? slot->texture : (Texture2D){ 0 };
}

const Texture2D *TextureStore_get_ref(int32_t id) {
    TextureSlot *slot = slot_at(id);
    return slot && slot->texture.id != 0 ? &slot->texture : NULL;
}
