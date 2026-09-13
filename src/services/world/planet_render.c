#include "services/world/planet_render.h"
#include "components/tile.h"

#include <stdlib.h>

#define PLANET_CHUNK_MAX_VERTS 65520

static int chunk_end(const Planet *p, int start, int *out_verts, int *out_tris) {
    int verts = 0, tris = 0, c = start;
    while (c < p->cell_count) {
        int cell_verts = p->degree[c] + 1;
        if (verts && verts + cell_verts > PLANET_CHUNK_MAX_VERTS) break;
        verts += cell_verts;
        tris  += p->degree[c];
        c++;
    }
    *out_verts = verts;
    *out_tris  = tris;
    return c;
}

static int chunk_count(const Planet *p) {
    int n = 0, c = 0;
    while (c < p->cell_count) {
        int v, t;
        c = chunk_end(p, c, &v, &t);
        n++;
    }
    return n;
}

static Mesh build_chunk_mesh(const Planet *p, const unsigned char *terrain,
                             int start, int end, int verts, int tris) {
    Mesh mesh = { 0 };
    mesh.triangleCount = tris;
    mesh.vertexCount   = verts;
    mesh.vertices = (float *)MemAlloc((unsigned)verts * 3 * sizeof(float));
    mesh.normals  = (float *)MemAlloc((unsigned)verts * 3 * sizeof(float));
    mesh.colors   = (unsigned char *)MemAlloc((unsigned)verts * 4 * sizeof(unsigned char));
    mesh.indices  = (unsigned short *)MemAlloc((unsigned)tris * 3 * sizeof(unsigned short));

    int vi = 0;
    int ii = 0;
    for (int c = start; c < end; c++) {
        Color col = TILE_TERRAIN_COLORS[terrain[c] % TILE_TERRAIN_COUNT];
        PlanetV3 center = p->pos[c];
        int deg  = p->degree[c];
        int base = vi;

        PlanetV3 verts_src[1 + PLANET_MAX_DEGREE];
        verts_src[0] = center;
        for (int k = 0; k < deg; k++) verts_src[1 + k] = p->corner_pos[p->cell_corners[c][k]];
        for (int v = 0; v < deg + 1; v++) {
            mesh.vertices[vi*3+0] = verts_src[v].x;
            mesh.vertices[vi*3+1] = verts_src[v].y;
            mesh.vertices[vi*3+2] = verts_src[v].z;
            mesh.normals[vi*3+0]  = center.x;
            mesh.normals[vi*3+1]  = center.y;
            mesh.normals[vi*3+2]  = center.z;
            mesh.colors[vi*4+0] = col.r;
            mesh.colors[vi*4+1] = col.g;
            mesh.colors[vi*4+2] = col.b;
            mesh.colors[vi*4+3] = col.a;
            vi++;
        }

        for (int k = 0; k < deg; k++) {
            mesh.indices[ii++] = (unsigned short)base;
            mesh.indices[ii++] = (unsigned short)(base + 1 + k);
            mesh.indices[ii++] = (unsigned short)(base + 1 + (k + 1) % deg);
        }
    }

    UploadMesh(&mesh, false);
    return mesh;
}

Model planet_model_build(const Planet *p, const unsigned char *terrain) {
    int nchunks = chunk_count(p);

    Model model = { 0 };
    model.transform     = (Matrix){ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    model.meshCount     = nchunks;
    model.meshes        = (Mesh *)MemAlloc((unsigned)nchunks * sizeof(Mesh));
    model.meshMaterial  = (int *)MemAlloc((unsigned)nchunks * sizeof(int));
    model.materialCount = 1;
    model.materials     = (Material *)MemAlloc(sizeof(Material));
    model.materials[0]  = LoadMaterialDefault();

    int c = 0, m = 0;
    while (c < p->cell_count) {
        int verts, tris;
        int end = chunk_end(p, c, &verts, &tris);
        model.meshes[m]       = build_chunk_mesh(p, terrain, c, end, verts, tris);
        model.meshMaterial[m] = 0;
        c = end; m++;
    }
    return model;
}

void planet_model_update_colors(Model *model, const Planet *p, const unsigned char *terrain) {
    if (model->meshCount < 1) return;

    int c = 0, m = 0;
    while (c < p->cell_count && m < model->meshCount) {
        int verts, tris;
        int end = chunk_end(p, c, &verts, &tris);
        Mesh *mesh = &model->meshes[m];
        if (mesh->vertexCount != verts || !mesh->colors) return;

        int vi = 0;
        for (int cc = c; cc < end; cc++) {
            Color col = TILE_TERRAIN_COLORS[terrain[cc] % TILE_TERRAIN_COUNT];
            int cell_verts = p->degree[cc] + 1;
            for (int k = 0; k < cell_verts; k++) {
                mesh->colors[vi*4+0] = col.r;
                mesh->colors[vi*4+1] = col.g;
                mesh->colors[vi*4+2] = col.b;
                mesh->colors[vi*4+3] = col.a;
                vi++;
            }
        }
        UpdateMeshBuffer(*mesh, 3, mesh->colors, mesh->vertexCount * 4, 0);
        c = end; m++;
    }
}

void planet_model_apply_colors(Model *model, const Planet *p, const Color *cell_colors) {
    if (model->meshCount < 1) return;

    int c = 0, m = 0;
    while (c < p->cell_count && m < model->meshCount) {
        int verts, tris;
        int end = chunk_end(p, c, &verts, &tris);
        Mesh *mesh = &model->meshes[m];
        if (mesh->vertexCount != verts || !mesh->colors) return;

        int vi = 0;
        for (int cc = c; cc < end; cc++) {
            Color col = cell_colors[cc];
            int cell_verts = p->degree[cc] + 1;
            for (int k = 0; k < cell_verts; k++) {
                mesh->colors[vi*4+0] = col.r;
                mesh->colors[vi*4+1] = col.g;
                mesh->colors[vi*4+2] = col.b;
                mesh->colors[vi*4+3] = col.a;
                vi++;
            }
        }
        UpdateMeshBuffer(*mesh, 3, mesh->colors, mesh->vertexCount * 4, 0);
        c = end; m++;
    }
}

void planet_model_unload(Model *model) {
    if (model->meshCount > 0) UnloadModel(*model);
    model->meshCount = 0;
    model->meshes = NULL;
}
