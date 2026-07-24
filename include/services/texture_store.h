#ifndef SERVICES_TEXTURE_STORE_H
#define SERVICES_TEXTURE_STORE_H

#include "raylib.h"

// Small path-deduplicating texture registry so components can hold an int id
// instead of a raylib handle. Ids are >= 1; 0 means "no texture".

void TextureStore_init(void);
int TextureStore_load(const char *path);   // cached by path; 0 on failure
Texture2D TextureStore_get(int id);        // id 0 / unknown -> zeroed texture
void TextureStore_shutdown(void);

#endif
