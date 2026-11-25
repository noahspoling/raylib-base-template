#ifndef SERVICES_TILEMAP_LUA_H
#define SERVICES_TILEMAP_LUA_H

#include <stdbool.h>
#include <stdint.h>
#include "script_host.h"

#define TILEMAP_VIEW_KIND    9
#define TILEMAP_PALETTE_KIND 10

void tilemap_lua_register(ScriptHost *host);

bool tilemap_lua_draw(int32_t kind, float x, float y, float w, float h);

void tilemap_lua_shutdown(void);

void tilemap_lua_reload_shaders(void);
void tilemap_lua_watch_shaders(float dt);

#endif
