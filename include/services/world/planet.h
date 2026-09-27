#ifndef WORLD_PLANET_H
#define WORLD_PLANET_H

#include <stdint.h>
#include <stddef.h>

#ifndef PLANET_MAX_LEVEL
#define PLANET_MAX_LEVEL 9
#endif

#define PLANET_MAX_DEGREE 6

typedef struct { float x, y, z; } PlanetV3;

typedef struct Planet {
    int32_t level;
    int32_t frequency;
    int32_t cell_count;

    PlanetV3 *pos;
    int32_t   (*neighbors)[PLANET_MAX_DEGREE];
    uint8_t  *degree;

    PlanetV3 *corner_pos;
    int32_t   corner_count;
    int32_t   (*cell_corners)[PLANET_MAX_DEGREE];
} Planet;

Planet *planet_create(int32_t level);

Planet *planet_rebuild(Planet *p, int32_t level);

void planet_destroy(Planet *p);

int32_t planet_cell_count_for_level(int32_t level);

static inline float planet_cell_latitude(const Planet *p, int32_t cell) {
    return p->pos[cell].y;
}

#endif
