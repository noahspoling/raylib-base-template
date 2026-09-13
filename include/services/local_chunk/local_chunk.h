#ifndef SERVICES_LOCAL_CHUNK_H
#define SERVICES_LOCAL_CHUNK_H

#include <stdbool.h>
#include "services/world/planet.h"
#include "services/world/world_pipeline.h"

#define LOCAL_CHUNK_MAX_NEIGHBORS PLANET_MAX_DEGREE

typedef struct LocalChunkNeighbor {
    int   cell;
    float elevation;
    float temperature;
    float humidity;
    unsigned char terrain;
    bool  flows_into_me;
    bool  i_flow_into_it;
} LocalChunkNeighbor;

typedef struct LocalChunkFields {
    int          cell;
    int          res;
    unsigned int world_seed;
    unsigned int seed;

    int    degree;
    LocalChunkNeighbor neighbors[LOCAL_CHUNK_MAX_NEIGHBORS];

    float         self_elevation;
    float         self_temperature;
    float         self_humidity;
    unsigned char self_terrain;

    float         *elevation;
    unsigned char *river_mask;
    float         extent;

    const Planet *planet;
    WorldFields   world;
    int           generator_index;
} LocalChunkFields;

typedef struct LocalChunkStage {
    const char *name;
    void (*run)(LocalChunkFields *f);
} LocalChunkStage;

int                    local_chunk_stage_count(void);
const LocalChunkStage *local_chunk_stage_get(int index);

LocalChunkFields *local_chunk_create(const Planet *planet,
                                      const WorldFields *fields,
                                      unsigned int world_seed,
                                      int generator_index,
                                      int cell, int res);
void local_chunk_run(LocalChunkFields *f);
void local_chunk_destroy(LocalChunkFields *f);

void local_chunk_draw_in_rect(const LocalChunkFields *f, float x, float y, float fw, float fh);

typedef struct LocalChunkGroup {
    LocalChunkFields *tiles[1 + LOCAL_CHUNK_MAX_NEIGHBORS];
    int count;
} LocalChunkGroup;

LocalChunkGroup local_chunk_create_group(const Planet *planet,
                                          const WorldFields *fields,
                                          unsigned int world_seed,
                                          int generator_index,
                                          int cell, int res);
void local_chunk_destroy_group(LocalChunkGroup *g);

void local_chunk_draw_group_in_rect(const LocalChunkGroup *g, float x, float y, float fw, float fh);

unsigned int local_chunk_cache_key(unsigned int world_seed, int cell);

typedef enum {
    LOCAL_CHUNK_EDGE_CLEAN = 0,
    LOCAL_CHUNK_EDGE_NORTH,
    LOCAL_CHUNK_EDGE_SOUTH,
} LocalChunkEdgeRole;

void local_chunk_north_frame(PlanetV3 center, PlanetV3 *east, PlanetV3 *north);

LocalChunkEdgeRole local_chunk_edge_role(const Planet *planet, int cell, int neighborSlot);

typedef struct LocalChunkCrossing {
    int  cell;
    int  col, row;
    bool ok;
} LocalChunkCrossing;

LocalChunkCrossing local_chunk_cross_border(const Planet *planet, int cell, int res,
                                             int col, int row, int neighborSlot);

int local_chunk_debug_check_reciprocity(const Planet *planet, int *out_mismatches);

void local_chunk_draw_square_in_rect(const LocalChunkFields *f, float x, float y, float fw, float fh);

void local_chunk_draw_connected_in_rect(const LocalChunkGroup *g, float x, float y, float fw, float fh);

#endif
