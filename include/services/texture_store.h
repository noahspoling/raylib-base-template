#ifndef SERVICES_TEXTURE_STORE_H
#define SERVICES_TEXTURE_STORE_H

#include "raylib.h"

void TextureStore_init(void);
int TextureStore_load(const char *path);
Texture2D TextureStore_get(int id);
const Texture2D *TextureStore_get_ref(int id);
void TextureStore_shutdown(void);

#endif
