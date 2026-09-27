#include "services/world/world_pipeline.h"
#include <stdint.h>

#include <math.h>
#include <stdlib.h>

static PlanetV3 v3(float x, float y, float z) { PlanetV3 v = { x, y, z }; return v; }
static PlanetV3 add(PlanetV3 a, PlanetV3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static PlanetV3 scl(PlanetV3 a, float s)    { return v3(a.x * s, a.y * s, a.z * s); }
static PlanetV3 crs(PlanetV3 a, PlanetV3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static float    vlen(PlanetV3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static PlanetV3 nrm(PlanetV3 a)  { float l = vlen(a); return l > 1e-6f ? scl(a, 1.0f / l) : a; }

PlanetV3 world_climate_wind_at(PlanetV3 pos) {
    PlanetV3 east = crs(v3(0, 1, 0), pos);
    if (vlen(east) < 1e-4f) east = v3(1, 0, 0);
    east = nrm(east);
    PlanetV3 north = nrm(crs(pos, east));

    float y  = pos.y;
    float ay = fabsf(y);
    float s  = (y >= 0.0f) ? 1.0f : -1.0f;
    float zonal, merid;
    if (ay < 0.5f) {
        zonal = -1.0f; merid = -0.3f * s;
    } else if (ay < 0.866f) {
        zonal =  1.0f; merid =  0.3f * s;
    } else {
        zonal = -1.0f; merid = -0.3f * s;
    }
    return nrm(add(scl(east, zonal), scl(north, merid)));
}

PlanetV3 world_climate_wind(const Planet *p, int32_t cell) {
    return world_climate_wind_at(p->pos[cell]);
}

#define WIND_SAMPLE_K 4

WindField world_wind_field_build(int32_t count) {
    WindField wf = { 0, NULL, NULL, NULL };
    if (count <= 0) return wf;
    wf.count = count;
    wf.pos  = (PlanetV3 *)malloc((size_t)count * sizeof(PlanetV3));
    wf.wind = (PlanetV3 *)malloc((size_t)count * sizeof(PlanetV3));
    if (!wf.pos || !wf.wind) {
        free(wf.pos); free(wf.wind);
        wf.pos = wf.wind = NULL; wf.count = 0;
        return wf;
    }

    const float golden_angle = 2.39996323f;
    for (int32_t i = 0; i < count; i++) {
        float yv = 1.0f - (2.0f * (float)i + 1.0f) / (float)count;
        float r2 = 1.0f - yv * yv;
        float r  = r2 > 0.0f ? sqrtf(r2) : 0.0f;
        float theta = golden_angle * (float)i;
        PlanetV3 p = v3(cosf(theta) * r, yv, sinf(theta) * r);
        wf.pos[i]  = p;
        wf.wind[i] = world_climate_wind_at(p);
    }

    wf.index = KDTree_new(count, 3, (const float *)wf.pos);
    return wf;
}

void world_wind_field_free(WindField *wf) {
    if (!wf) return;
    free(wf->pos);  wf->pos  = NULL;
    free(wf->wind); wf->wind = NULL;
    if (wf->index) KDTree_free(&wf->index);
    wf->count = 0;
}

PlanetV3 world_wind_sample(const WindField *wf, PlanetV3 pos) {
    if (!wf || wf->count == 0 || !wf->index) return world_climate_wind_at(pos);

    int32_t idx[WIND_SAMPLE_K];
    float   d2[WIND_SAMPLE_K];
    int32_t k = KDTree_nearest(wf->index, (const float *)&pos, WIND_SAMPLE_K, idx, d2);
    if      (k == 0) return world_climate_wind_at(pos);

    PlanetV3 sum = v3(0, 0, 0);
    float wsum = 0.0f;
    for (int32_t i = 0; i < k; i++) {
        float w = 1.0f / (d2[i] + 1e-4f);
        sum = add(sum, scl(wf->wind[idx[i]], w));
        wsum += w;
    }
    if (wsum <= 0.0f) return wf->wind[idx[0]];
    PlanetV3 avg = scl(sum, 1.0f / wsum);
    return vlen(avg) > 1e-5f ? nrm(avg) : wf->wind[idx[0]];
}
