#include "services/world/world_pipeline.h"

#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

static PlanetV3 v3(float x, float y, float z) { PlanetV3 v = { x, y, z }; return v; }
static PlanetV3 sub(PlanetV3 a, PlanetV3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static PlanetV3 scl(PlanetV3 a, float s)    { return v3(a.x * s, a.y * s, a.z * s); }
static float    dot(PlanetV3 a, PlanetV3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float    vlen(PlanetV3 a) { return sqrtf(dot(a, a)); }
static PlanetV3 nrm(PlanetV3 a)  { float l = vlen(a); return l > 1e-6f ? scl(a, 1.0f / l) : a; }
static float    clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float hashf(int i, unsigned int seed) {
    unsigned int h = (unsigned int)i * 2654435761u ^ (seed * 40503u + 0x9e3779b9u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h & 0xffffffu) / (float)0xffffffu;
}

void world_climate_run(WorldFields *f, const WindField *wind) {
    const Planet *p          = f->planet;
    const WorldGenParams *pm = f->params;
    int   n   = f->count;
    float sea = pm->sea_level;

    for (int i = 0; i < n; i++) {
        if (f->elevation[i] < sea) {
            f->moisture[i] = 1.0f;
        } else {
            float lat = fabsf(p->pos[i].y);
            f->moisture[i] = 0.12f + 0.10f * (1.0f - lat);
        }
    }

    float *weight  = (float *)calloc((size_t)n * PLANET_MAX_DEGREE, sizeof(float));
    int   *primary = (int   *)malloc((size_t)n * sizeof(int));
    if (!weight || !primary) { free(weight); free(primary); return; }
    for (int i = 0; i < n; i++) {
        primary[i] = -1;
        if (f->elevation[i] < sea) continue;
        PlanetV3 negw = scl(world_wind_sample(wind, p->pos[i]), -1.0f);
        int deg = p->degree[i];
        float local[PLANET_MAX_DEGREE] = { 0 };
        float sum = 0.0f, best = 0.0f;
        int besti = -1;
        for (int k = 0; k < deg; k++) {
            int nb = p->neighbors[i][k];
            if (nb < 0) continue;
            float align = dot(nrm(sub(p->pos[nb], p->pos[i])), negw);
            if (align > 0.0f) {
                local[k] = align * align;
                sum += local[k];
                if (local[k] > best) { best = local[k]; besti = k; }
            }
        }
        if (sum > 1e-6f) {
            for (int k = 0; k < deg; k++)
                weight[i * PLANET_MAX_DEGREE + k] = local[k] / sum;
            primary[i] = besti;
        }
    }

    float reach  = pm->moisture_reach > 0.0f ? pm->moisture_reach : 1.0f;
    int   sweeps = (int)(6.0f + reach * 14.0f);
    if (sweeps < 6)  sweeps = 6;
    if (sweeps > 48) sweeps = 48;
    const float keep         = 0.86f;
    const float recycle_gain = 0.16f;
    for (int s = 0; s < sweeps; s++) {
        for (int i = 0; i < n; i++) {
            if (f->elevation[i] < sea) continue;
            int deg = p->degree[i];
            float pulled = 0.0f;
            for (int k = 0; k < deg; k++) {
                float wgt = weight[i * PLANET_MAX_DEGREE + k];
                if (wgt <= 0.0f) continue;
                int nb = p->neighbors[i][k];
                if (nb < 0) continue;
                pulled += wgt * f->moisture[nb];
            }
            float warmth = clamp01((f->temperature[i] - 0.5f) * 2.0f);
            float cand = pulled * keep + f->moisture[i] * recycle_gain * warmth;
            if (cand > f->moisture[i]) f->moisture[i] = clamp01(cand);
        }
    }

    float rs = pm->rain_shadow;
    for (int i = 0; i < n; i++) {
        if (f->elevation[i] < sea || primary[i] < 0) continue;
        int up = p->neighbors[i][primary[i]];
        float d = f->elevation[i] - f->elevation[up];
        if (d > 0.0f) f->moisture[i] *= 1.0f + rs * clamp01(d * 2.0f) * 0.5f;
        else          f->moisture[i] *= 1.0f - rs * clamp01(-d * 2.0f);
        f->moisture[i] = clamp01(f->moisture[i]);
    }
    free(weight);
    free(primary);

    for (int i = 0; i < n; i++) {
        if (f->elevation[i] < sea) { f->rainfall[i] = 1.0f; continue; }
        float tcap   = 0.70f + 0.30f * f->temperature[i];
        float jitter = (hashf(i, pm->seed) - 0.5f) * 0.04f;
        f->rainfall[i] = clamp01(f->moisture[i] * tcap + jitter);
    }

    float *tmp = (float *)malloc((size_t)n * sizeof(float));
    if (tmp) {
        for (int pass = 0; pass < 3; pass++) {
            for (int i = 0; i < n; i++) {
                if (f->elevation[i] < sea) { tmp[i] = f->rainfall[i]; continue; }
                float sum = f->rainfall[i];
                int   cnt = 1, deg = p->degree[i];
                for (int k = 0; k < deg; k++) {
                    int nb = p->neighbors[i][k];
                    if (nb < 0) continue;
                    sum += f->rainfall[nb];
                    cnt++;
                }
                tmp[i] = sum / (float)cnt;
            }
            for (int i = 0; i < n; i++) f->rainfall[i] = tmp[i];
        }
        free(tmp);
    }
    for (int i = 0; i < n; i++)
        f->humidity[i] = (f->elevation[i] < sea) ? 1.0f : f->rainfall[i];
}
