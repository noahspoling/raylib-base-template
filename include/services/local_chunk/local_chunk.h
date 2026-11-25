#ifndef SERVICES_LOCAL_CHUNK_H
#define SERVICES_LOCAL_CHUNK_H

#include <stdint.h>
#include <stdbool.h>
#include "services/world/planet.h"
#include "services/world/world_pipeline.h"
#include "raylib.h"

#define LOCAL_CHUNK_MAX_NEIGHBORS PLANET_MAX_DEGREE
#define LOCAL_CHUNK_METERS_PER_PIXEL 500.0f
#define LOCAL_CHUNK_OWNER_SELF    0
#define LOCAL_CHUNK_OWNER_UNKNOWN 255

typedef struct LocalChunkNeighbor {
    int32_t cell;
    float   elevation;
    float   temperature;
    float   humidity;
    uint8_t terrain;
    bool    flows_into_me;
    bool    i_flow_into_it;
} LocalChunkNeighbor;

typedef struct LocalChunkFields {
    int32_t  cell;
    int32_t  res;
    uint32_t world_seed;
    uint32_t seed;
    uint64_t key;

    uint8_t            degree;
    LocalChunkNeighbor neighbors[LOCAL_CHUNK_MAX_NEIGHBORS];

    float         self_elevation;
    float         self_temperature;
    float         self_humidity;
    uint8_t       self_terrain;

    float         *elevation;
    float         *temperature;
    float         *humidity;
    float         *lake;
    uint8_t       *terrain;
    uint8_t       *river_mask;
    uint8_t       *owner;
    float          extent;
    PlanetV3       east, north;

    const Planet  *planet;
    WorldFields    world;
    int32_t        generator_index;
} LocalChunkFields;

typedef struct LocalChunkStage {
    const char *name;
    void (*run)(LocalChunkFields *f);
} LocalChunkStage;

int32_t                local_chunk_stage_count(void);
const LocalChunkStage *local_chunk_stage_get(int32_t index);

LocalChunkFields *local_chunk_create(const Planet *planet,
                                      const WorldFields *fields,
                                      uint32_t world_seed,
                                      int32_t generator_index,
                                      int32_t cell, int32_t res);
void local_chunk_run(LocalChunkFields *f);
void local_chunk_destroy(LocalChunkFields *f);

void local_chunk_draw_in_rect(const LocalChunkFields *f, float x, float y, float fw, float fh);

typedef enum LocalChunkView {
    LOCAL_CHUNK_VIEW_TERRAIN,
    LOCAL_CHUNK_VIEW_TOPOGRAPHY,
    LOCAL_CHUNK_VIEW_COUNT
} LocalChunkView;

const char *local_chunk_view_name(int32_t view);

typedef struct LocalChunkGroup {
    LocalChunkFields *tiles[1 + LOCAL_CHUNK_MAX_NEIGHBORS];
    int32_t count;

    int32_t   view_res;
    float     view_span;
    uint8_t  *view_tile;
    int32_t  *view_index;
    int32_t   owner_mismatches;
    Texture2D view_connected[LOCAL_CHUNK_VIEW_COUNT];
    Texture2D view_tinted[LOCAL_CHUNK_VIEW_COUNT];
} LocalChunkGroup;

int32_t local_chunk_res_for_cell(const Planet *planet, int32_t cell);

LocalChunkGroup local_chunk_create_group(const Planet *planet,
                                          const WorldFields *fields,
                                          uint32_t world_seed,
                                          int32_t generator_index,
                                          int32_t cell);
void local_chunk_destroy_group(LocalChunkGroup *g);

void local_chunk_draw_group_in_rect(const LocalChunkGroup *g, int32_t view, float x, float y, float fw, float fh);

void local_chunk_draw_connected_in_rect(const LocalChunkGroup *g, int32_t view, float x, float y, float fw, float fh);

#define LOCAL_CHUNK_KEY_CELL_BITS  27
#define LOCAL_CHUNK_KEY_CELL_MASK  ((1 << LOCAL_CHUNK_KEY_CELL_BITS) - 1)
#define LOCAL_CHUNK_KEY_LEVEL_MASK 0x1f

typedef char local_chunk_key_level_fits[(PLANET_MAX_LEVEL <= LOCAL_CHUNK_KEY_LEVEL_MASK) ? 1 : -1];
typedef char local_chunk_key_cell_fits[((10LL << (2 * PLANET_MAX_LEVEL)) + 2 <= LOCAL_CHUNK_KEY_CELL_MASK) ? 1 : -1];

uint64_t local_chunk_cache_key(uint32_t world_seed, int32_t level, int32_t cell);

void local_chunk_north_frame(PlanetV3 center, PlanetV3 *east, PlanetV3 *north);

float local_chunk_extent(const Planet *planet, int32_t cell);

PlanetV3 local_chunk_border_warp(const Planet *planet, uint32_t world_seed, PlanetV3 p);

int32_t local_chunk_owner_at(const Planet *planet, uint32_t world_seed, int32_t hint_cell, PlanetV3 p);

typedef struct LocalChunkBlend {
    int32_t cell[3];
    float   weight[3];
} LocalChunkBlend;

LocalChunkBlend local_chunk_blend_at(const Planet *planet, uint32_t world_seed, int32_t hint_cell, PlanetV3 p);

#endif
