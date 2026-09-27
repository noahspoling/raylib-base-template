#ifndef WORLD_WORLD_H
#define WORLD_WORLD_H

#include <stdint.h>
#include "raylib.h"
#include "gramarye_ecs/ecs.h"
#include "services/world/planet.h"
#include "services/world/world_gen.h"
#include "services/world/world_pipeline.h"

typedef struct World World;

#define WORLD_GLOBE_KIND    1
#define WORLD_CONTROLS_KIND 2

World *world_create(ECS *ecs, ComponentTypeId tile_type, int32_t level);
void   world_destroy(World *w);

void    world_set_level(World *w, int32_t level);
int32_t world_level(const World *w);
int32_t world_cell_count(const World *w);

void    world_set_pending_level(World *w, int32_t level);
int32_t world_pending_level(const World *w);

void         world_set_generator(World *w, int32_t index);
int32_t      world_generator_index(const World *w);
const char  *world_generator_name(const World *w);
uint32_t     world_seed(const World *w);

void world_regenerate(World *w);

void world_apply(World *w);

void world_step_generation(World *w);

bool         world_generating(const World *w);
const char  *world_gen_stage_name(const World *w);
int32_t      world_gen_stage(const World *w);
int32_t      world_gen_stage_count(void);

void world_reroll(World *w);

void world_set_seed(World *w, uint32_t seed);

bool world_dirty(const World *w);

WorldGenParams *world_params(World *w);

typedef struct WorldTileInfo {
    int32_t cell;
    float   elevation, temperature, humidity;
    float   rainfall;
    float   flow;
    int32_t region;
    float   stress;
    float   lat, lon;
    int8_t  plate;
    uint8_t river;
    uint8_t fault;
    uint8_t terrain;
    uint8_t neighbors;
} WorldTileInfo;

int32_t world_selected(const World *w);
bool    world_selected_info(const World *w, WorldTileInfo *out);
void    world_clear_selection(World *w);

int32_t world_neighbor_count(const World *w);
bool    world_neighbor_info(const World *w, int32_t slot, WorldTileInfo *out);

bool world_selected_screen_pos(const World *w, float *out_x, float *out_y);

void world_set_input_suppressed(World *w, bool suppressed);

const Planet *world_planet(const World *w);
WorldFields   world_fields(const World *w);

void world_set_sea_level(World *w, float v);
void world_set_warmth(World *w, float v);
void world_set_mountain_level(World *w, float v);
void world_set_noise_scale(World *w, float v);

void world_set_rain_shadow(World *w, float v);
void world_set_moisture_reach(World *w, float v);
void world_set_hydrology(World *w, bool on);
bool world_hydrology_enabled(const World *w);
void world_set_river_density(World *w, float v);

void world_set_plate_count(World *w, int32_t v);

enum {
    WORLD_VIEW_TERRAIN = 0,
    WORLD_VIEW_TEMPERATURE,
    WORLD_VIEW_RAINFALL,
    WORLD_VIEW_FLOW,
    WORLD_VIEW_REGION,
    WORLD_VIEW_WIND,
    WORLD_VIEW_ELEVATION,
    WORLD_VIEW_MOISTURE,
    WORLD_VIEW_PLATES,
    WORLD_VIEW_COUNT
};
void    world_set_view(World *w, int32_t mode);
int32_t world_view(const World *w);

void world_draw_in_rect(World *w, float x, float y, float w_px, float h_px);

void world_draw_controls_in_rect(World *w, float x, float y, float w_px, float h_px);

#endif
