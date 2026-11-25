#ifndef DEV_SHADER_PLAYGROUND_H
#define DEV_SHADER_PLAYGROUND_H

#include "script_host.h"

#define SHADER_PLAYGROUND_KIND 7
#define SHADER_EDITOR_KIND     8

void shader_playground_lua_register(ScriptHost *host);
void shader_playground_draw_in_rect(float x, float y, float w, float h);
void shader_playground_draw_editor_in_rect(float x, float y, float w, float h);
void shader_playground_unload(void);

#endif
