#ifndef WORLD_PLANET_H
#define WORLD_PLANET_H

#include <stddef.h>

#ifndef PLANET_MAX_LEVEL
#define PLANET_MAX_LEVEL 9
#endif

#define PLANET_MAX_DEGREE 6

typedef struct { float x, y, z; } PlanetV3;

typedef struct Planet {
    int level;
    int frequency;
    int cell_count;

    PlanetV3 *pos;
    int       (*neighbors)[PLANET_MAX_DEGREE];
    unsigned char *degree;

    PlanetV3 *corner_pos;
    int       corner_count;
    int       (*cell_corners)[PLANET_MAX_DEGREE];
} Planet;

Planet *planet_create(int level);

Planet *planet_rebuild(Planet *p, int level);

void planet_destroy(Planet *p);

int planet_cell_count_for_level(int level);

static inline float planet_cell_latitude(const Planet *p, int cell) {
    return p->pos[cell].y;
}

#endif
