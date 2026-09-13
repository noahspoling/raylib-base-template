#include "services/world/planet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static PlanetV3 v3(float x, float y, float z) { PlanetV3 v = { x, y, z }; return v; }
static PlanetV3 v3add(PlanetV3 a, PlanetV3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static PlanetV3 v3sub(PlanetV3 a, PlanetV3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static PlanetV3 v3scale(PlanetV3 a, float s)  { return v3(a.x * s, a.y * s, a.z * s); }
static float    v3dot(PlanetV3 a, PlanetV3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static PlanetV3 v3cross(PlanetV3 a, PlanetV3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static PlanetV3 v3norm(PlanetV3 a) {
    float len = sqrtf(v3dot(a, a));
    return len > 0.0f ? v3scale(a, 1.0f / len) : a;
}

typedef struct { PlanetV3 *data; int count, cap; } VertBuf;
typedef struct { int (*data)[3];  int count, cap; } FaceBuf;

static int vb_push(VertBuf *b, PlanetV3 v) {
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->data = (PlanetV3 *)realloc(b->data, (size_t)b->cap * sizeof(PlanetV3));
    }
    b->data[b->count] = v;
    return b->count++;
}
static void fb_push(FaceBuf *b, int a, int c, int d) {
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->data = (int (*)[3])realloc(b->data, (size_t)b->cap * sizeof(*b->data));
    }
    b->data[b->count][0] = a;
    b->data[b->count][1] = c;
    b->data[b->count][2] = d;
    b->count++;
}

typedef struct { unsigned long long key; int val; } MpEntry;
typedef struct { MpEntry *slots; int mask; } MpCache;

static void mp_init(MpCache *c, int expected) {
    int cap = 16;
    while (cap < expected * 2) cap <<= 1;
    c->slots = (MpEntry *)calloc((size_t)cap, sizeof(MpEntry));
    c->mask = cap - 1;
}
static void mp_free(MpCache *c) { free(c->slots); c->slots = NULL; }

static unsigned long long mp_mix(unsigned long long x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}
static int mp_get(MpCache *c, VertBuf *vb, int a, int b) {
    int lo = a < b ? a : b, hi = a < b ? b : a;
    unsigned long long key = ((unsigned long long)lo << 32) | (unsigned)hi;
    unsigned long long slot = mp_mix(key) & (unsigned)c->mask;
    unsigned long long stored = key + 1;
    for (;;) {
        MpEntry *e = &c->slots[slot];
        if (e->key == 0) {
            PlanetV3 mid = v3norm(v3scale(v3add(vb->data[a], vb->data[b]), 0.5f));
            int idx = vb_push(vb, mid);
            e->key = stored;
            e->val = idx;
            return idx;
        }
        if (e->key == stored) return e->val;
        slot = (slot + 1) & (unsigned)c->mask;
    }
}

static void build_icosahedron(VertBuf *vb, FaceBuf *fb) {
    const float t = (1.0f + sqrtf(5.0f)) * 0.5f;
    const PlanetV3 base[12] = {
        {-1,  t, 0}, { 1,  t, 0}, {-1, -t, 0}, { 1, -t, 0},
        { 0, -1, t}, { 0,  1, t}, { 0, -1,-t}, { 0,  1,-t},
        { t,  0,-1}, { t,  0, 1}, {-t,  0,-1}, {-t,  0, 1},
    };
    for (int i = 0; i < 12; i++) vb_push(vb, v3norm(base[i]));

    static const int faces[20][3] = {
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1},
    };
    for (int i = 0; i < 20; i++) fb_push(fb, faces[i][0], faces[i][1], faces[i][2]);
}

static void subdivide(VertBuf *vb, FaceBuf *fb) {
    MpCache cache;
    mp_init(&cache, fb->count * 3);
    FaceBuf out = {0};
    for (int i = 0; i < fb->count; i++) {
        int a = fb->data[i][0], b = fb->data[i][1], c = fb->data[i][2];
        int ab = mp_get(&cache, vb, a, b);
        int bc = mp_get(&cache, vb, b, c);
        int ca = mp_get(&cache, vb, c, a);
        fb_push(&out, a,  ab, ca);
        fb_push(&out, b,  bc, ab);
        fb_push(&out, c,  ca, bc);
        fb_push(&out, ab, bc, ca);
    }
    mp_free(&cache);
    free(fb->data);
    *fb = out;
}

static void order_ring(PlanetV3 center, PlanetV3 *items, int *ids, int n) {
    PlanetV3 up = (fabsf(center.y) < 0.9f) ? v3(0,1,0) : v3(1,0,0);
    PlanetV3 tan = v3norm(v3cross(up, center));
    PlanetV3 bit = v3cross(center, tan);
    float ang[PLANET_MAX_DEGREE];
    for (int i = 0; i < n; i++) {
        PlanetV3 d = v3sub(items[i], v3scale(center, v3dot(items[i], center)));
        ang[i] = atan2f(v3dot(d, bit), v3dot(d, tan));
    }
    for (int i = 1; i < n; i++) {
        float a = ang[i]; PlanetV3 it = items[i]; int id = ids[i];
        int j = i - 1;
        while (j >= 0 && ang[j] > a) { ang[j+1] = ang[j]; items[j+1] = items[j]; ids[j+1] = ids[j]; j--; }
        ang[j+1] = a; items[j+1] = it; ids[j+1] = id;
    }
}

static int build_topology(Planet *p, VertBuf *vb, FaceBuf *fb) {
    int nv = vb->count, nf = fb->count;

    p->corner_pos = (PlanetV3 *)malloc((size_t)nf * sizeof(PlanetV3));
    if (!p->corner_pos) return 0;
    p->corner_count = nf;
    for (int i = 0; i < nf; i++) {
        PlanetV3 a = vb->data[fb->data[i][0]];
        PlanetV3 b = vb->data[fb->data[i][1]];
        PlanetV3 c = vb->data[fb->data[i][2]];
        p->corner_pos[i] = v3norm(v3scale(v3add(v3add(a, b), c), 1.0f / 3.0f));
    }

    int (*nbr)[PLANET_MAX_DEGREE]  = malloc((size_t)nv * sizeof(*nbr));
    int (*face)[PLANET_MAX_DEGREE] = malloc((size_t)nv * sizeof(*face));
    unsigned char *deg  = malloc((size_t)nv);
    unsigned char *fdeg = malloc((size_t)nv);
    if (!nbr || !face || !deg || !fdeg) { free(nbr); free(face); free(deg); free(fdeg); return 0; }
    memset(deg, 0, (size_t)nv);
    memset(fdeg, 0, (size_t)nv);

    for (int i = 0; i < nf; i++) {
        for (int k = 0; k < 3; k++) {
            int v  = fb->data[i][k];
            int n1 = fb->data[i][(k + 1) % 3];
            int n2 = fb->data[i][(k + 2) % 3];
            if (fdeg[v] < PLANET_MAX_DEGREE) face[v][fdeg[v]++] = i;
            for (int which = 0; which < 2; which++) {
                int adj = which ? n2 : n1;
                int seen = 0;
                for (int q = 0; q < deg[v]; q++) if (nbr[v][q] == adj) { seen = 1; break; }
                if (!seen && deg[v] < PLANET_MAX_DEGREE) nbr[v][deg[v]++] = adj;
            }
        }
    }

    for (int v = 0; v < nv; v++) {
        p->pos[v]    = vb->data[v];
        p->degree[v] = deg[v];

        PlanetV3 items[PLANET_MAX_DEGREE]; int ids[PLANET_MAX_DEGREE];
        for (int q = 0; q < deg[v]; q++) { ids[q] = nbr[v][q]; items[q] = vb->data[nbr[v][q]]; }
        order_ring(vb->data[v], items, ids, deg[v]);
        for (int q = 0; q < PLANET_MAX_DEGREE; q++) p->neighbors[v][q] = q < deg[v] ? ids[q] : -1;

        for (int q = 0; q < fdeg[v]; q++) { ids[q] = face[v][q]; items[q] = p->corner_pos[face[v][q]]; }
        order_ring(vb->data[v], items, ids, fdeg[v]);
        for (int q = 0; q < PLANET_MAX_DEGREE; q++) p->cell_corners[v][q] = q < fdeg[v] ? ids[q] : -1;
    }

    free(nbr); free(face); free(deg); free(fdeg);
    return 1;
}

static int clamp_level(int level) {
    if (level < 0) return 0;
    if (level > PLANET_MAX_LEVEL) return PLANET_MAX_LEVEL;
    return level;
}

int planet_cell_count_for_level(int level) {
    int f = 1 << clamp_level(level);
    return 10 * f * f + 2;
}

static void free_buffers(Planet *p) {
    free(p->pos);          p->pos = NULL;
    free(p->neighbors);    p->neighbors = NULL;
    free(p->degree);       p->degree = NULL;
    free(p->corner_pos);   p->corner_pos = NULL;
    free(p->cell_corners); p->cell_corners = NULL;
}

static int planet_build(Planet *p, int level) {
    level = clamp_level(level);
    p->level     = level;
    p->frequency = 1 << level;

    VertBuf vb = {0};
    FaceBuf fb = {0};
    build_icosahedron(&vb, &fb);
    for (int i = 0; i < level; i++) subdivide(&vb, &fb);

    p->cell_count = vb.count;

    p->pos          = (PlanetV3 *)malloc((size_t)vb.count * sizeof(PlanetV3));
    p->neighbors    = malloc((size_t)vb.count * sizeof(*p->neighbors));
    p->degree       = (unsigned char *)malloc((size_t)vb.count);
    p->cell_corners = malloc((size_t)vb.count * sizeof(*p->cell_corners));
    p->corner_pos   = NULL; p->corner_count = 0;

    int ok = p->pos && p->neighbors && p->degree && p->cell_corners;
    if (ok) ok = build_topology(p, &vb, &fb);

    free(vb.data);
    free(fb.data);
    if (!ok) free_buffers(p);
    return ok;
}

Planet *planet_create(int level) {
    Planet *p = (Planet *)calloc(1, sizeof(Planet));
    if (!p) return NULL;
    if (!planet_build(p, level)) { free(p); return NULL; }
    return p;
}

Planet *planet_rebuild(Planet *p, int level) {
    if (!p) return planet_create(level);
    free_buffers(p);
    if (!planet_build(p, level)) { free(p); return NULL; }
    return p;
}

void planet_destroy(Planet *p) {
    if (!p) return;
    free_buffers(p);
    free(p);
}
