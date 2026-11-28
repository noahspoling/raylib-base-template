#ifndef SERVICES_STORES_TEXTURE_STORE_H
#define SERVICES_STORES_TEXTURE_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "raylib.h"
#include "services/stores/store.h"

#define TEXTURE_STORE_HINT 64

typedef struct {
    StoreSlot base;     // must be first
    const char *path;
    Texture2D texture;
} TextureSlot;

void TextureStore_init(void);
void TextureStore_shutdown(void);

int32_t TextureStore_register(const char *name, const char *path);
int32_t TextureStore_find(const char *name);
bool TextureStore_acquire(int32_t id);
void TextureStore_release(int32_t id);
int32_t TextureStore_load(const char *name, const char *path);

bool TextureStore_is_resident(int32_t id);
const char *TextureStore_name(int32_t id);
const char *TextureStore_path(int32_t id);
Texture2D TextureStore_get(int32_t id);
const Texture2D *TextureStore_get_ref(int32_t id);

#endif
