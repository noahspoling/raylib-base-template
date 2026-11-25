#ifndef DEV_SHADER_EDITOR_H
#define DEV_SHADER_EDITOR_H

#include <stdbool.h>
#include <stdint.h>

void        shader_editor_set_text(const char *text, bool keep_view);
const char *shader_editor_text(void);
uint32_t    shader_editor_revision(void);
double      shader_editor_last_edit(void);
bool        shader_editor_focused(void);
void        shader_editor_set_focus(bool focused);
void        shader_editor_set_info(const char *info);
void        shader_editor_set_errors(const char *log);
void        shader_editor_draw_in_rect(float x, float y, float w, float h);
void        shader_editor_shutdown(void);

#endif
