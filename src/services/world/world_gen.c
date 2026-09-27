#include "services/world/world_gen.h"
#include <stdint.h>
#include "services/world_noise.h"

#include <math.h>

static float clamp01(float v) { return world_noise_clamp01(v); }
static float fbm(PlanetV3 p, float freq, int32_t octaves, uint32_t seed) {
    return world_noise_fbm(p, freq, octaves, seed);
}

static float ridged(PlanetV3 p, float freq, int32_t octaves, uint32_t seed) {
    float n = fbm(p, freq, octaves, seed);
    float r = 1.0f - fabsf(n);
    return r * r;
}

static PlanetV3 warp(PlanetV3 p, float freq, float amt, uint32_t seed) {
    float wx = fbm(p, freq, 3, seed + 11u);
    float wy = fbm(p, freq, 3, seed + 22u);
    float wz = fbm(p, freq, 3, seed + 33u);
    PlanetV3 q = { p.x + amt * wx, p.y + amt * wy, p.z + amt * wz };
    return q;
}

static float warped_latitude(PlanetV3 pos, uint32_t seed) {
    float lat = fabsf(pos.y);
    lat += 0.05f * fbm(pos, 2.2f, 3, seed + 555u);
    return clamp01(lat);
}

static float climate_temperature(const WorldGenParams *pm, float lat, float elevation) {
    float t = 1.0f - lat;
    t -= 0.35f * fmaxf(0.0f, elevation);
    t += pm->warmth;
    return clamp01(t);
}

static float climate_humidity(float lat) {
    float eq  = expf(-(lat * lat) / (2.0f * 0.11f * 0.11f));
    float mid = expf(-((lat - 0.6f) * (lat - 0.6f)) / (2.0f * 0.13f * 0.13f));
    return clamp01(0.18f + 0.78f * eq + 0.45f * mid);
}

WorldGenParams world_gen_default_params(uint32_t seed) {
    WorldGenParams p = {
        .seed             = seed,
        .sea_level        = 0.0f,
        .mountain_level   = 0.5f,
        .noise_scale      = 1.7f,
        .warmth           = 0.0f,
        .rain_shadow      = 0.6f,
        .moisture_reach   = 1.0f,
        .enable_hydrology = false,
        .river_density    = 0.5f,
        .plate_count      = 10,
    };
    return p;
}

static WorldGenSample gen_continents(const WorldGenerator *self,
                                     const WorldGenParams *pm, PlanetV3 pos, int32_t cell) {
    (void)self; (void)cell;
    PlanetV3 wp = warp(pos, pm->noise_scale * 0.6f, 0.35f, pm->seed);
    float e = fbm(wp, pm->noise_scale, 6, pm->seed);
    e += 0.12f * fbm(pos, pm->noise_scale * 4.0f, 3, pm->seed + 77u);
    e *= 1.15f;

    float lat  = warped_latitude(pos, pm->seed);
    float temp = climate_temperature(pm, lat, e);
    float hum  = 0.55f * climate_humidity(lat)
               + 0.45f * (0.5f + 0.5f * fbm(pos, pm->noise_scale * 1.4f, 4, pm->seed + 404u));
    hum -= 0.25f * fmaxf(0.0f, e - 0.2f);
    WorldGenSample s = { e, temp, clamp01(hum) };
    return s;
}

static WorldGenSample gen_islands(const WorldGenerator *self,
                                  const WorldGenParams *pm, PlanetV3 pos, int32_t cell) {
    (void)self; (void)cell;
    float cluster = fbm(pos, pm->noise_scale * 0.55f, 3, pm->seed + 123u);
    float uplift  = clamp01((cluster - 0.05f) * 1.9f);

    PlanetV3 wp   = warp(pos, pm->noise_scale * 1.2f, 0.25f, pm->seed + 7u);
    float   detail = fbm(wp, pm->noise_scale * 1.9f, 5, pm->seed);

    float e = uplift * 0.85f + detail * 0.5f - 0.5f;

    float lat  = warped_latitude(pos, pm->seed);
    float temp = climate_temperature(pm, lat, e) + 0.1f;
    float hum  = clamp01(0.6f + 0.4f * fbm(pos, pm->noise_scale * 1.1f, 3, pm->seed + 909u));
    WorldGenSample s = { e, clamp01(temp), hum };
    return s;
}

static WorldGenSample gen_bands(const WorldGenerator *self,
                                const WorldGenParams *pm, PlanetV3 pos, int32_t cell) {
    (void)self; (void)cell;
    float e = 0.45f * fbm(pos, pm->noise_scale * 1.8f, 4, pm->seed);
    float lat  = warped_latitude(pos, pm->seed);
    float temp = clamp01(1.0f - lat + pm->warmth);
    float hum  = climate_humidity(lat);
    WorldGenSample s = { e, temp, hum };
    return s;
}

static const WorldGenerator GENERATORS[] = {
    { "Continents",     gen_continents, NULL },
    { "Islands",        gen_islands,    NULL },
    { "Latitude Bands", gen_bands,      NULL },
};
static const int32_t GEN_COUNT = (int32_t)(sizeof(GENERATORS) / sizeof(GENERATORS[0]));

int32_t world_gen_count(void) { return GEN_COUNT; }

const WorldGenerator *world_gen_get(int32_t index) {
    if (index < 0) index = 0;
    if (index >= GEN_COUNT) index = GEN_COUNT - 1;
    return &GENERATORS[index];
}
const char *world_gen_name(int32_t index) { return world_gen_get(index)->name; }

typedef struct {
    uint8_t terrain;
    float it, tt;
    float ih, th;
    float ie, te;
} BiomeProfile;

static const BiomeProfile BIOMES[] = {
    { TILE_TERRAIN_SNOW,       0.05f, 0.12f, 0.50f, 0.60f, 0.40f, 0.80f },
    { TILE_TERRAIN_TUNDRA,     0.26f, 0.10f, 0.45f, 0.35f, 0.40f, 0.80f },
    { TILE_TERRAIN_TAIGA,      0.40f, 0.11f, 0.62f, 0.28f, 0.45f, 0.70f },
    { TILE_TERRAIN_GRASS,      0.56f, 0.16f, 0.33f, 0.16f, 0.45f, 0.70f },
    { TILE_TERRAIN_FOREST,     0.56f, 0.16f, 0.66f, 0.20f, 0.45f, 0.70f },
    { TILE_TERRAIN_DESERT,     0.78f, 0.18f, 0.10f, 0.13f, 0.45f, 0.80f },
    { TILE_TERRAIN_SAVANNA,    0.80f, 0.15f, 0.44f, 0.18f, 0.40f, 0.70f },
    { TILE_TERRAIN_RAINFOREST, 0.80f, 0.20f, 0.74f, 0.24f, 0.35f, 0.70f },
    { TILE_TERRAIN_SWAMP,      0.62f, 0.18f, 0.92f, 0.13f, 0.10f, 0.22f },
};
static const int32_t BIOME_COUNT = (int32_t)(sizeof(BIOMES) / sizeof(BIOMES[0]));

static float gauss(float x, float ideal, float tol) {
    float d = (x - ideal) / tol;
    return expf(-d * d);
}

TileTerrain tile_classify(const WorldGenParams *pm,
                          float elevation, float temperature, float humidity) {
    float sea = pm->sea_level;

    if (elevation < sea - 0.16f)  return TILE_TERRAIN_OCEAN;
    if (elevation < sea)          return TILE_TERRAIN_SHALLOW;

    if (elevation > pm->mountain_level)
        return (temperature < 0.35f) ? TILE_TERRAIN_SNOW : TILE_TERRAIN_ROCK;

    float span = pm->mountain_level - sea;
    if (span < 1e-3f) span = 1e-3f;
    float en = clamp01((elevation - sea) / span);

    int32_t best = 0;
    float best_score = -1.0f;
    for (int32_t b = 0; b < BIOME_COUNT; b++) {
        const BiomeProfile *bp = &BIOMES[b];
        float gt = gauss(temperature, bp->it, bp->tt);
        float gh = gauss(humidity,    bp->ih, bp->th);
        float ge = gauss(en,          bp->ie, bp->te);
        float s  = gt * gh * (0.5f + 0.5f * ge);
        if (s > best_score) { best_score = s; best = b; }
    }
    return (TileTerrain)BIOMES[best].terrain;
}
