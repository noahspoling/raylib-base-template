#ifndef WORLD_PLANET_RENDER_H
#define WORLD_PLANET_RENDER_H

#include "raylib.h"
#include "services/world/planet.h"

Model planet_model_build(const Planet *p, const unsigned char *terrain);

void planet_model_update_colors(Model *model, const Planet *p, const unsigned char *terrain);

void planet_model_apply_colors(Model *model, const Planet *p, const Color *cell_colors);

void planet_model_unload(Model *model);

#endif
