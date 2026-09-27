#include "services/world/world_pipeline.h"
#include <stdint.h>

#include <math.h>
#include <stdlib.h>

static PlanetV3 v3(float x, float y, float z) { PlanetV3 v = { x, y, z }; return v; }
static PlanetV3 sub(PlanetV3 a, PlanetV3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static PlanetV3 add(PlanetV3 a, PlanetV3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static PlanetV3 scl(PlanetV3 a, float s)    { return v3(a.x * s, a.y * s, a.z * s); }
static float    dot(PlanetV3 a, PlanetV3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static PlanetV3 crs(PlanetV3 a, PlanetV3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static float    vlen(PlanetV3 a) { return sqrtf(dot(a, a)); }
static PlanetV3 nrm(PlanetV3 a)  { float l = vlen(a); return l > 1e-6f ? scl(a, 1.0f / l) : a; }
static float    clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static float hashf(uint32_t seed, int32_t i, uint32_t salt) {
    uint32_t h = (uint32_t)i * 2654435761u ^ (seed * 40503u + salt * 0x9e3779bbu);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h & 0xffffffu) / (float)0xffffffu;
}

static void tangent_basis(PlanetV3 p, PlanetV3 *t1, PlanetV3 *t2) {
    PlanetV3 a = crs(v3(0, 1, 0), p);
    if (vlen(a) < 1e-4f) a = v3(1, 0, 0);
    *t1 = nrm(a);
    *t2 = nrm(crs(p, *t1));
}

PlateField world_plate_field_build(int32_t count, uint32_t seed) {
    PlateField pf = { 0, NULL, NULL, NULL, NULL, NULL };
    if (count < 2)  count = 2;
    if (count > 64) count = 64;
    pf.count   = count;
    pf.seed    = (PlanetV3 *)malloc((size_t)count * sizeof(PlanetV3));
    pf.axis    = (PlanetV3 *)malloc((size_t)count * sizeof(PlanetV3));
    pf.speed   = (float *)malloc((size_t)count * sizeof(float));
    pf.oceanic = (uint8_t *)malloc((size_t)count);
    if (!pf.seed || !pf.axis || !pf.speed || !pf.oceanic) {
        free(pf.seed); free(pf.axis); free(pf.speed); free(pf.oceanic);
        PlateField empty = { 0, NULL, NULL, NULL, NULL, NULL };
        return empty;
    }

    const float golden_angle = 2.39996323f;
    float avg_spacing   = sqrtf(4.0f * 3.14159265f / (float)count);
    float jitter_radius = 0.55f * avg_spacing;

    for (int32_t i = 0; i < count; i++) {
        float yv = 1.0f - (2.0f * (float)i + 1.0f) / (float)count;
        float r2 = 1.0f - yv * yv;
        float r  = r2 > 0.0f ? sqrtf(r2) : 0.0f;
        float theta = golden_angle * (float)i;
        PlanetV3 base = v3(cosf(theta) * r, yv, sinf(theta) * r);

        PlanetV3 t1, t2;
        tangent_basis(base, &t1, &t2);
        float jang = hashf(seed, i, 101) * 6.2831853f;
        float jmag = hashf(seed, i, 102) * jitter_radius;
        PlanetV3 jitter = scl(add(scl(t1, cosf(jang)), scl(t2, sinf(jang))), jmag);
        pf.seed[i] = nrm(add(base, jitter));

        float au = hashf(seed, i, 103), av = hashf(seed, i, 104);
        float az  = 1.0f - 2.0f * au;
        float ar2 = 1.0f - az * az;
        float ar  = ar2 > 0.0f ? sqrtf(ar2) : 0.0f;
        float aphi = av * 6.2831853f;
        pf.axis[i] = v3(ar * cosf(aphi), az, ar * sinf(aphi));

        pf.speed[i] = hashf(seed, i, 105) * 2.0f - 1.0f;

        pf.oceanic[i] = hashf(seed, i, 106) < 0.65f ? 1 : 0;
    }

    pf.index = KDTree_new(count, 3, (const float *)pf.seed);
    return pf;
}

void world_plate_field_free(PlateField *pf) {
    if (!pf) return;
    free(pf->seed);    pf->seed    = NULL;
    free(pf->axis);    pf->axis    = NULL;
    free(pf->speed);   pf->speed   = NULL;
    free(pf->oceanic); pf->oceanic = NULL;
    if (pf->index) KDTree_free(&pf->index);
    pf->count = 0;
}

static PlanetV3 plate_velocity(const PlateField *pf, int32_t plate, PlanetV3 pos) {
    return scl(crs(pf->axis[plate], pos), pf->speed[plate]);
}

#define TECT_BASELINE_K 6

void world_tectonics_run(WorldFields *f, const PlateField *pf) {
    const Planet *p = f->planet;
    int32_t n = f->count;

    if (!pf || pf->count == 0 || !pf->index) {
        for (int32_t i = 0; i < n; i++) { f->plate[i] = -1; f->fault[i] = WORLD_FAULT_NONE; f->stress[i] = 0.0f; }
        return;
    }

    float avg_spacing = sqrtf(4.0f * 3.14159265f / (float)pf->count);
    float sigma = 0.05f * avg_spacing;
    if (sigma < 1e-4f) sigma = 1e-4f;

    const float uplift     = 0.42f;
    const float rift       = 0.22f;
    const float ocean_bias = -0.10f;
    const float land_bias  =  0.05f;

    for (int32_t i = 0; i < n; i++) {
        PlanetV3 pos = p->pos[i];
        int32_t idx[TECT_BASELINE_K]; float d2[TECT_BASELINE_K];
        int32_t k = KDTree_nearest(pf->index, (const float *)&pos, TECT_BASELINE_K, idx, d2);
        int32_t plate_a = k > 0 ? idx[0] : -1;

        f->plate[i]  = (int8_t)plate_a;
        f->fault[i]  = WORLD_FAULT_NONE;
        f->stress[i] = 0.0f;
        if (plate_a < 0) continue;

        float bsum = 0.0f, wsum = 0.0f;
        for (int32_t j = 0; j < k; j++) {
            float wgt = 1.0f / (d2[j] + 1e-4f);
            float b = pf->oceanic[idx[j]] ? ocean_bias : land_bias;
            bsum += wgt * b;
            wsum += wgt;
        }
        f->elevation[i] += wsum > 0.0f ? bsum / wsum : (pf->oceanic[plate_a] ? ocean_bias : land_bias);

        if (k >= 2) {
            int32_t plate_b = idx[1];
            float dist_a = sqrtf(d2[0]), dist_b = sqrtf(d2[1]);
            float gap = dist_b - dist_a;
            float proximity = expf(-(gap * gap) / (2.0f * sigma * sigma));

            if (proximity >= 0.02f) {
                PlanetV3 va = plate_velocity(pf, plate_a, pos);
                PlanetV3 vb = plate_velocity(pf, plate_b, pos);
                PlanetV3 rel = sub(va, vb);

                PlanetV3 toward_b = sub(pf->seed[plate_b], pf->seed[plate_a]);
                PlanetV3 normal = nrm(sub(toward_b, scl(pos, dot(toward_b, pos))));

                float normal_comp = dot(rel, normal);
                PlanetV3 tangential = sub(rel, scl(normal, normal_comp));
                float tangent_mag = vlen(tangential);

                f->stress[i] = vlen(rel) * proximity;

                if (fabsf(normal_comp) > tangent_mag) {
                    if (normal_comp > 0.0f) {
                        f->fault[i] = WORLD_FAULT_CONVERGENT;
                        f->elevation[i] += uplift * proximity * clamp01(normal_comp);
                    } else {
                        f->fault[i] = WORLD_FAULT_DIVERGENT;
                        f->elevation[i] -= rift * proximity * clamp01(-normal_comp);
                    }
                } else {
                    f->fault[i] = WORLD_FAULT_TRANSFORM;
                }
            }
        }

        if (f->elevation[i] < -1.0f)      f->elevation[i] = -1.0f;
        else if (f->elevation[i] > 1.0f)  f->elevation[i] =  1.0f;
    }
}
