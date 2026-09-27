#ifndef SERVICES_WORLD_NOISE_H
#define SERVICES_WORLD_NOISE_H

#include <stdint.h>
#include "services/world/planet.h"
#include <math.h>

static inline uint32_t world_noise_hash3(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed + 0x9e3779b9u;
    h ^= (uint32_t)x * 0x85ebca6bu; h = (h << 13) | (h >> 19);
    h ^= (uint32_t)y * 0xc2b2ae35u; h = (h << 17) | (h >> 15);
    h ^= (uint32_t)z * 0x27d4eb2fu; h = (h << 11) | (h >> 21);
    h *= 0x2545f491u; h ^= h >> 16;
    return h;
}

static inline float world_noise_hashf(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return (float)(world_noise_hash3(x, y, z, seed) & 0xffffff) / (float)0xffffff;
}

static inline float world_noise_smooth(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float world_noise_lerp1(float a, float b, float t) { return a + (b - a) * t; }
static inline float world_noise_clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static inline float world_noise_value(float x, float y, float z, uint32_t seed) {
    int32_t xi = (int32_t)floorf(x), yi = (int32_t)floorf(y), zi = (int32_t)floorf(z);
    float xf = x - xi, yf = y - yi, zf = z - zi;
    float u = world_noise_smooth(xf), v = world_noise_smooth(yf), w = world_noise_smooth(zf);
    float c000 = world_noise_hashf(xi,   yi,   zi,   seed), c100 = world_noise_hashf(xi+1, yi,   zi,   seed);
    float c010 = world_noise_hashf(xi,   yi+1, zi,   seed), c110 = world_noise_hashf(xi+1, yi+1, zi,   seed);
    float c001 = world_noise_hashf(xi,   yi,   zi+1, seed), c101 = world_noise_hashf(xi+1, yi,   zi+1, seed);
    float c011 = world_noise_hashf(xi,   yi+1, zi+1, seed), c111 = world_noise_hashf(xi+1, yi+1, zi+1, seed);
    float x00 = world_noise_lerp1(c000, c100, u), x10 = world_noise_lerp1(c010, c110, u);
    float x01 = world_noise_lerp1(c001, c101, u), x11 = world_noise_lerp1(c011, c111, u);
    return world_noise_lerp1(world_noise_lerp1(x00, x10, v), world_noise_lerp1(x01, x11, v), w);
}

static inline float world_noise_fbm(PlanetV3 p, float freq, int32_t octaves, uint32_t seed) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int32_t o = 0; o < octaves; o++) {
        sum  += amp * world_noise_value(p.x * freq, p.y * freq, p.z * freq, seed + (uint32_t)o * 1013u);
        norm += amp;
        amp  *= 0.5f;
        freq *= 2.0f;
    }
    return (sum / norm) * 2.0f - 1.0f;
}

#endif
