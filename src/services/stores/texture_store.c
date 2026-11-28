#include "services/stores/texture_store.h"
#include <stddef.h>
#include "atom.h"

static Store g_store;

static TextureSlot *slot_at(int32_t id) {
    return (TextureSlot *)Store_at(&g_store, id);
}

static bool texture_load(StoreSlot *base) {
    TextureSlot *slot = (TextureSlot *)base;
    slot->texture = LoadTexture(slot->path);
    return slot->texture.id != 0;
}

static void texture_unload(StoreSlot *base) {
    TextureSlot *slot = (TextureSlot *)base;
    UnloadTexture(slot->texture);
    slot->texture = (Texture2D){ 0 };
}

static bool texture_is_resident(const StoreSlot *base) {
    return ((const TextureSlot *)base)->texture.id != 0;
}

void TextureStore_init(void) {
    Store_init(&g_store, TEXTURE_STORE_HINT, sizeof(TextureSlot), "TEXTURES",
               (StoreOps){ texture_load, texture_unload, texture_is_resident });
}

void TextureStore_shutdown(void) {
    Store_shutdown(&g_store);
}

int32_t TextureStore_register(const char *name, const char *path) {
    if (!g_store.slots || !name || !name[0] || !path || !path[0]) return 0;
    const char *key = Atom_string(name);
    const char *file = Atom_string(path);

    int32_t id = Store_find(&g_store, key);
    if (id) {
        TextureSlot *slot = slot_at(id);
        if (slot->path != file) {
            TraceLog(LOG_WARNING, "TEXTURES: '%s' already registered to '%s'; ignoring '%s'",
                     key, slot->path, file);
        }
        return id;
    }

    for (int32_t i = 1; i <= g_store.count; i++) {
        TextureSlot *slot = slot_at(i);
        if (slot->path == file) {
            TraceLog(LOG_WARNING, "TEXTURES: '%s' is already registered as '%s'; aliasing '%s' to it",
                     file, slot->base.name, key);
            Store_alias(&g_store, key, i);
            return i;
        }
    }

    TextureSlot slot = { .base = { .name = key, .refs = 0 }, .path = file, .texture = { 0 } };
    return Store_add(&g_store, &slot);
}

int32_t TextureStore_find(const char *name) {
    if (!name || !name[0]) return 0;
    return Store_find(&g_store, Atom_string(name));
}

bool TextureStore_acquire(int32_t id) { return Store_acquire(&g_store, id); }
void TextureStore_release(int32_t id) { Store_release(&g_store, id); }

int32_t TextureStore_load(const char *name, const char *path) {
    int32_t id = TextureStore_register(name, path);
    return TextureStore_acquire(id) ? id : 0;
}

bool TextureStore_is_resident(int32_t id) { return Store_is_resident(&g_store, id); }
const char *TextureStore_name(int32_t id) { return Store_name(&g_store, id); }

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
