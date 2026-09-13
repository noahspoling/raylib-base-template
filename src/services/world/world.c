#include "services/world/world.h"
#include "services/world/planet_render.h"
#include "services/world/world_pipeline.h"
#include "components/tile.h"

#include "rlgl.h"
#include "raymath.h"

#include <math.h>
#include <stdlib.h>

#define WORLD_WIND_FIELD_COUNT 384

struct World {
    ECS *ecs;
    ComponentTypeId tile_type;

    Planet *planet;

    float         *elevation;
    float         *temperature;
    float         *humidity;
    unsigned char *terrain;
    float         *rainfall;
    float         *moisture;
    int           *downhill;
    float         *flow;
    unsigned char *river;
    float         *water_level;
    int           *region;
    int           *plate;
    unsigned char *fault;
    float         *stress;
    EntityId      *cells;
    int            cell_cap;
    WindField      wind;
    PlateField     plate_field;

    int            gen_index;
    WorldGenParams params;

    int   pending_level;
    bool  dirty;

    Model model;
    bool  model_valid;
    Model river_model;
    bool  river_valid;
    int   view_mode;

    int   gen_stage;
    int   gen_target_level;
    bool  gen_box_shown;

    Camera3D cam;
    float yaw, pitch, dist;
    bool  auto_rotate;

    int   ui_drag;
    float ui_pending;

    int     selected;
    bool    globe_pressed;
    Vector2 press_pos;

    float globe_x, globe_y, globe_fw, globe_fh;
    bool  globe_rect_valid;

    bool  input_suppressed;
};

static void despawn_tiles(World *w) {
    if (!w->cells) return;
    EntityRegistry *reg = ECS_get_entity_registry(w->ecs);
    for (int i = 0; i < w->cell_cap; i++) {
        if (ECS_has_component(w->ecs, w->cells[i], w->tile_type))
            ECS_remove_component(w->ecs, w->cells[i], w->tile_type);
        Entity_destroy(reg, w->cells[i]);
    }
}

static bool alloc_and_spawn(World *w) {
    int n = w->planet->cell_count;
    int old_n = w->cell_cap;
    w->elevation   = (float *)realloc(w->elevation,   (size_t)n * sizeof(float));
    w->temperature = (float *)realloc(w->temperature, (size_t)n * sizeof(float));
    w->humidity    = (float *)realloc(w->humidity,    (size_t)n * sizeof(float));
    w->terrain     = (unsigned char *)realloc(w->terrain, (size_t)n);
    w->rainfall    = (float *)realloc(w->rainfall,    (size_t)n * sizeof(float));
    w->moisture    = (float *)realloc(w->moisture,    (size_t)n * sizeof(float));
    w->downhill    = (int *)realloc(w->downhill,      (size_t)n * sizeof(int));
    w->flow        = (float *)realloc(w->flow,        (size_t)n * sizeof(float));
    w->river       = (unsigned char *)realloc(w->river, (size_t)n);
    w->water_level = (float *)realloc(w->water_level, (size_t)n * sizeof(float));
    w->region      = (int *)realloc(w->region,        (size_t)n * sizeof(int));
    w->plate       = (int *)realloc(w->plate,          (size_t)n * sizeof(int));
    w->fault       = (unsigned char *)realloc(w->fault, (size_t)n);
    w->stress      = (float *)realloc(w->stress,       (size_t)n * sizeof(float));
    w->cells       = (EntityId *)realloc(w->cells, (size_t)n * sizeof(EntityId));
    if (!w->elevation || !w->temperature || !w->humidity || !w->terrain ||
        !w->rainfall || !w->moisture ||
        !w->downhill || !w->flow || !w->river || !w->water_level || !w->region ||
        !w->plate || !w->fault || !w->stress || !w->cells)
        return false;

    EntityRegistry *reg = ECS_get_entity_registry(w->ecs);

    for (int i = n; i < old_n; i++) {
        if (ECS_has_component(w->ecs, w->cells[i], w->tile_type))
            ECS_remove_component(w->ecs, w->cells[i], w->tile_type);
        Entity_destroy(reg, w->cells[i]);
    }
    for (int i = old_n; i < n; i++) {
        w->cells[i] = Entity_create(reg);
        TileComp tc = { .cell = i, .elevation = 0, .temperature = 0, .humidity = 0,
                        .terrain = TILE_TERRAIN_OCEAN };
        ECS_add_component(w->ecs, w->cells[i], w->tile_type, &tc);
    }
    w->cell_cap = n;
    return true;
}

WorldFields world_fields(const World *w) {
    WorldFields f = {
        .planet = w->planet, .params = &w->params, .count = w->planet->cell_count,
        .elevation = w->elevation, .temperature = w->temperature, .humidity = w->humidity,
        .rainfall = w->rainfall, .moisture = w->moisture,
        .downhill = w->downhill, .flow = w->flow, .river = w->river,
        .water_level = w->water_level, .region = w->region,
        .terrain = w->terrain,
        .plate = w->plate, .fault = w->fault, .stress = w->stress,
    };
    return f;
}

static Color lerp_col(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    Color c = { (unsigned char)(a.r + (b.r - a.r) * t),
                (unsigned char)(a.g + (b.g - a.g) * t),
                (unsigned char)(a.b + (b.b - a.b) * t), 255 };
    return c;
}

static Color view_color(const World *w, int c) {
    bool ocean = w->elevation[c] < w->params.sea_level;
    switch (w->view_mode) {
    case WORLD_VIEW_TEMPERATURE:
        if (ocean) return (Color){ 30, 40, 70, 255 };
        return lerp_col((Color){ 40, 90, 200, 255 }, (Color){ 214, 70, 40, 255 }, w->temperature[c]);
    case WORLD_VIEW_RAINFALL:
        if (ocean) return (Color){ 24, 54, 110, 255 };
        return lerp_col((Color){ 206, 184, 120, 255 }, (Color){ 40, 120, 180, 255 }, w->rainfall[c]);
    case WORLD_VIEW_FLOW: {
        if (ocean) return (Color){ 24, 40, 70, 255 };
        float t = w->flow[c] > 0.0f ? logf(1.0f + w->flow[c]) / logf(201.0f) : 0.0f;
        return lerp_col((Color){ 60, 66, 58, 255 }, (Color){ 70, 160, 220, 255 }, t);
    }
    case WORLD_VIEW_REGION: {
        if (w->region[c] < 0) return (Color){ 40, 48, 60, 255 };
        unsigned int h = (unsigned int)(w->region[c] + 1) * 2654435761u;
        return (Color){ (unsigned char)(60 + h % 180),
                        (unsigned char)(60 + (h >> 8) % 180),
                        (unsigned char)(60 + (h >> 16) % 180), 255 };
    }
    case WORLD_VIEW_WIND: {
        PlanetV3 dir = world_wind_sample(&w->wind, w->planet->pos[c]);
        float hue = (atan2f(dir.z, dir.x) + PI) / (2.0f * PI) * 360.0f;
        return ColorFromHSV(hue, 0.85f, ocean ? 0.45f : 0.95f);
    }
    case WORLD_VIEW_ELEVATION: {
        float sea = w->params.sea_level;
        if (ocean) {
            float t = Clamp((w->elevation[c] + 1.0f) / (sea + 1.0f), 0.0f, 1.0f);
            return lerp_col((Color){ 8, 16, 40, 255 }, (Color){ 60, 110, 170, 255 }, t);
        }
        float span = 1.0f - sea; if (span < 1e-3f) span = 1e-3f;
        float t = Clamp((w->elevation[c] - sea) / span, 0.0f, 1.0f);
        return lerp_col((Color){ 70, 110, 60, 255 }, (Color){ 250, 250, 252, 255 }, t);
    }
    case WORLD_VIEW_MOISTURE:
        if (ocean) return (Color){ 20, 30, 46, 255 };
        return lerp_col((Color){ 96, 74, 48, 255 }, (Color){ 120, 60, 190, 255 }, w->moisture[c]);
    case WORLD_VIEW_PLATES: {
        if (w->fault[c] != WORLD_FAULT_NONE) {
            float t = Clamp(w->stress[c] * 1.5f, 0.15f, 1.0f);
            Color fc = w->fault[c] == WORLD_FAULT_CONVERGENT ? (Color){ 230, 60, 50, 255 }
                     : w->fault[c] == WORLD_FAULT_DIVERGENT  ? (Color){ 60, 210, 190, 255 }
                                                              : (Color){ 235, 205, 60, 255 };
            return lerp_col((Color){ 40, 40, 46, 255 }, fc, t);
        }
        if (w->plate[c] < 0) return (Color){ 40, 40, 46, 255 };
        unsigned int h = (unsigned int)(w->plate[c] + 1) * 2654435761u;
        Color base = { (unsigned char)(50 + h % 160),
                       (unsigned char)(50 + (h >> 8) % 160),
                       (unsigned char)(50 + (h >> 16) % 160), 255 };
        return ocean ? lerp_col((Color){ 10, 12, 20, 255 }, base, 0.5f) : base;
    }
    default:
        return TILE_TERRAIN_COLORS[w->terrain[c] % TILE_TERRAIN_COUNT];
    }
}

static void world_recolor(World *w) {
    if (!w->model_valid) return;
    if (w->view_mode == WORLD_VIEW_TERRAIN) {
        planet_model_update_colors(&w->model, w->planet, w->terrain);
        return;
    }
    int n = w->planet->cell_count;
    Color *cols = (Color *)malloc((size_t)n * sizeof(Color));
    if (!cols) return;
    for (int c = 0; c < n; c++) cols[c] = view_color(w, c);
    planet_model_apply_colors(&w->model, w->planet, cols);
    free(cols);
}

static void stage_sample(World *w) {
    const WorldGenerator *g = world_gen_get(w->gen_index);
    for (int i = 0; i < w->planet->cell_count; i++) {
        WorldGenSample s = g->sample(g, &w->params, w->planet->pos[i], i);
        w->elevation[i]   = s.elevation;
        w->temperature[i] = s.temperature;
        w->humidity[i]    = s.humidity;
        w->flow[i]        = 0.0f;
        w->river[i]       = 0;
        w->water_level[i] = WORLD_NO_WATER;
        w->downhill[i]    = -1;
        w->region[i]      = -1;
    }
}

static void stage_tectonics(World *w) {
    world_plate_field_free(&w->plate_field);
    w->plate_field = world_plate_field_build(w->params.plate_count, w->params.seed);
    WorldFields f = world_fields(w);
    world_tectonics_run(&f, &w->plate_field);
}

static void stage_biomes(World *w) {
    WorldFields f = world_fields(w);
    world_biomes_classify(&f);
}

static void build_river_mesh(World *w);

static void stage_finalize(World *w) {
    for (int i = 0; i < w->planet->cell_count; i++) {
        TileComp *tc = (TileComp *)ECS_get_component(w->ecs, w->cells[i], w->tile_type);
        if (tc) {
            tc->cell        = i;
            tc->elevation   = w->elevation[i];
            tc->temperature = w->temperature[i];
            tc->humidity    = w->humidity[i];
            tc->rainfall    = w->rainfall[i];
            tc->flow        = w->flow[i];
            tc->river       = w->river[i];
            tc->water_level = w->water_level[i];
            tc->region      = w->region[i];
            tc->plate       = w->plate[i];
            tc->fault       = w->fault[i];
            tc->stress      = w->stress[i];
            tc->terrain     = w->terrain[i];
        }
    }
    if (!w->model_valid) { w->model = planet_model_build(w->planet, w->terrain); w->model_valid = true; }
    world_recolor(w);
    build_river_mesh(w);
}

void world_regenerate(World *w) {
    stage_sample(w);
    stage_tectonics(w);
    WorldFields f = world_fields(w);
    world_climate_run(&f, &w->wind);
    stage_biomes(w);
    world_hydrology_run(&f);
    world_regions_run(&f);
    stage_finalize(w);
}

enum { GEN_IDLE = 0, GEN_GEOMETRY, GEN_SAMPLE, GEN_TECTONICS, GEN_CLIMATE, GEN_BIOMES,
       GEN_HYDROLOGY, GEN_REGIONS, GEN_FINALIZE, GEN_STAGE_COUNT };
static const char *GEN_STAGE_NAMES[GEN_STAGE_COUNT] = {
    "", "Geometry", "Sampling terrain", "Tectonics", "Climate", "Biomes",
    "Rivers & lakes", "Regions", "Finishing"
};

static void stage_geometry(World *w) {
    if (w->gen_target_level == w->planet->level) return;
    w->planet = planet_rebuild(w->planet, w->gen_target_level);
    alloc_and_spawn(w);
    if (w->model_valid) { planet_model_unload(&w->model); w->model_valid = false; }
    if (w->river_valid) { planet_model_unload(&w->river_model); w->river_valid = false; }
    w->selected = -1;
}

void world_step_generation(World *w) {
    if (w->gen_stage == GEN_IDLE) return;
    WorldFields f = world_fields(w);
    switch (w->gen_stage) {
        case GEN_GEOMETRY:  stage_geometry(w);        break;
        case GEN_SAMPLE:    stage_sample(w);          break;
        case GEN_TECTONICS: stage_tectonics(w);       break;
        case GEN_CLIMATE:   world_climate_run(&f, &w->wind); break;
        case GEN_BIOMES:    stage_biomes(w);          break;
        case GEN_HYDROLOGY: world_hydrology_run(&f);  break;
        case GEN_REGIONS:   world_regions_run(&f);    break;
        case GEN_FINALIZE:
            stage_finalize(w);
            w->pending_level = w->planet->level;
            w->dirty = false;
            break;
    }
    w->gen_stage++;
    if (w->gen_stage >= GEN_STAGE_COUNT) w->gen_stage = GEN_IDLE;
}

bool world_generating(const World *w) { return w->gen_stage != GEN_IDLE; }
int  world_gen_stage(const World *w)  { return w->gen_stage; }
int  world_gen_stage_count(void)      { return GEN_STAGE_COUNT; }
const char *world_gen_stage_name(const World *w) {
    int s = w->gen_stage;
    return (s > 0 && s < GEN_STAGE_COUNT) ? GEN_STAGE_NAMES[s] : "";
}

void world_reroll(World *w) {
    w->params.seed = (unsigned int)GetRandomValue(1, 1 << 30);
    w->dirty = true;
}

void world_set_seed(World *w, unsigned int seed) {
    if (w->params.seed == seed) return;
    w->params.seed = seed;
    w->dirty = true;
}

World *world_create(ECS *ecs, ComponentTypeId tile_type, int level) {
    World *w = (World *)calloc(1, sizeof(World));
    if (!w) return NULL;
    w->ecs = ecs;
    w->tile_type = tile_type;
    w->gen_index = 0;
    w->params = world_gen_default_params(1337u);

    w->planet = planet_create(level);
    if (!w->planet) { free(w); return NULL; }
    if (!alloc_and_spawn(w)) { planet_destroy(w->planet); free(w); return NULL; }
    w->wind = world_wind_field_build(WORLD_WIND_FIELD_COUNT);

    w->cam = (Camera3D){ .position = { 0, 1.2f, 3.2f }, .target = { 0, 0, 0 },
                         .up = { 0, 1, 0 }, .fovy = 45.0f, .projection = CAMERA_PERSPECTIVE };
    w->yaw = 0.0f; w->pitch = 0.35f; w->dist = 3.2f; w->auto_rotate = true;
    w->ui_drag = -1;
    w->selected = -1;
    w->pending_level = w->planet->level;
    w->dirty = false;

    world_regenerate(w);
    return w;
}

void world_destroy(World *w) {
    if (!w) return;
    if (w->model_valid) planet_model_unload(&w->model);
    if (w->river_valid) planet_model_unload(&w->river_model);
    despawn_tiles(w);
    world_wind_field_free(&w->wind);
    world_plate_field_free(&w->plate_field);
    free(w->elevation); free(w->temperature); free(w->humidity);
    free(w->terrain);
    free(w->rainfall); free(w->moisture);
    free(w->downhill); free(w->flow); free(w->river); free(w->water_level); free(w->region);
    free(w->plate); free(w->fault); free(w->stress);
    free(w->cells);
    planet_destroy(w->planet);
    free(w);
}

void world_apply(World *w) {
    int target = w->pending_level;
    if (target < 0) target = 0;
    if (target > PLANET_MAX_LEVEL) target = PLANET_MAX_LEVEL;
    w->gen_target_level = target;
    w->gen_stage = GEN_GEOMETRY;
    w->gen_box_shown = false;
}

static void world_apply_sync(World *w) {
    world_apply(w);
    while (w->gen_stage != GEN_IDLE) world_step_generation(w);
}

void world_set_level(World *w, int level) {
    w->pending_level = level;
    world_apply_sync(w);
}

void world_set_pending_level(World *w, int level) {
    if (level < 0) level = 0;
    if (level > PLANET_MAX_LEVEL) level = PLANET_MAX_LEVEL;
    if (level != w->pending_level) { w->pending_level = level; w->dirty = true; }
}

int  world_pending_level(const World *w) { return w->pending_level; }
bool world_dirty(const World *w)         { return w->dirty; }

int world_selected(const World *w) { return w->selected; }

void world_clear_selection(World *w) { w->selected = -1; }

bool world_selected_info(const World *w, WorldTileInfo *out) {
    int c = w->selected;
    if (c < 0 || c >= w->planet->cell_count) return false;
    PlanetV3 p = w->planet->pos[c];
    out->cell        = c;
    out->elevation   = w->elevation[c];
    out->temperature = w->temperature[c];
    out->humidity    = w->humidity[c];
    out->rainfall    = w->rainfall[c];
    out->flow        = w->flow[c];
    out->river       = w->river[c];
    out->region      = w->region[c];
    out->plate       = w->plate[c];
    out->fault       = w->fault[c];
    out->stress      = w->stress[c];
    out->terrain     = w->terrain[c];
    out->lat         = asinf(p.y) * (180.0f / 3.14159265f);
    out->lon         = atan2f(p.z, p.x) * (180.0f / 3.14159265f);
    out->neighbors   = w->planet->degree[c];
    return true;
}

static void fill_tile_info(const World *w, int c, WorldTileInfo *out) {
    PlanetV3 p = w->planet->pos[c];
    out->cell        = c;
    out->elevation   = w->elevation[c];
    out->temperature = w->temperature[c];
    out->humidity    = w->humidity[c];
    out->rainfall    = w->rainfall[c];
    out->flow        = w->flow[c];
    out->river       = w->river[c];
    out->region      = w->region[c];
    out->plate       = w->plate[c];
    out->fault       = w->fault[c];
    out->stress      = w->stress[c];
    out->terrain     = w->terrain[c];
    out->lat         = asinf(p.y) * (180.0f / 3.14159265f);
    out->lon         = atan2f(p.z, p.x) * (180.0f / 3.14159265f);
    out->neighbors   = w->planet->degree[c];
}

int world_neighbor_count(const World *w) {
    int c = w->selected;
    if (c < 0 || c >= w->planet->cell_count) return 0;
    return w->planet->degree[c];
}

bool world_neighbor_info(const World *w, int slot, WorldTileInfo *out) {
    int c = w->selected;
    if (c < 0 || c >= w->planet->cell_count) return false;
    int deg = w->planet->degree[c];
    if (slot < 0 || slot >= deg) return false;
    int nb = w->planet->neighbors[c][slot];
    if (nb < 0 || nb >= w->planet->cell_count) return false;
    fill_tile_info(w, nb, out);
    return true;
}

static bool project_to_globe_rect(const World *w, PlanetV3 pos, float *out_x, float *out_y) {
    if (!w->globe_rect_valid) return false;
    Vector3 rel = Vector3Subtract((Vector3){ pos.x, pos.y, pos.z }, w->cam.position);
    Vector3 fwd   = Vector3Normalize(Vector3Subtract(w->cam.target, w->cam.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, w->cam.up));
    Vector3 up    = Vector3CrossProduct(right, fwd);
    float depth = Vector3DotProduct(rel, fwd);
    if (depth <= 0.001f) return false;
    float tanHalf = tanf(w->cam.fovy * 0.5f * DEG2RAD);
    float aspect  = w->globe_fw / w->globe_fh;
    float ndcx = Vector3DotProduct(rel, right) / (depth * tanHalf * aspect);
    float ndcy = Vector3DotProduct(rel, up)    / (depth * tanHalf);
    *out_x = w->globe_x + (ndcx * 0.5f + 0.5f) * w->globe_fw;
    *out_y = w->globe_y + (1.0f - (ndcy * 0.5f + 0.5f)) * w->globe_fh;
    return true;
}

bool world_selected_screen_pos(const World *w, float *out_x, float *out_y) {
    int c = w->selected;
    if (c < 0 || c >= w->planet->cell_count) return false;
    PlanetV3 p = w->planet->pos[c];
    Vector3 camDir = Vector3Normalize(w->cam.position);
    float facing = p.x * camDir.x + p.y * camDir.y + p.z * camDir.z;
    if (facing < 0.15f) return false;
    return project_to_globe_rect(w, p, out_x, out_y);
}

const Planet *world_planet(const World *w) { return w->planet; }

void world_set_input_suppressed(World *w, bool suppressed) {
    w->input_suppressed = suppressed;
}

int world_level(const World *w)      { return w->planet->level; }
int world_cell_count(const World *w) { return w->planet->cell_count; }

void world_set_generator(World *w, int index) {
    int n = world_gen_count();
    w->gen_index = ((index % n) + n) % n;
    w->dirty = true;
}
int          world_generator_index(const World *w) { return w->gen_index; }
const char  *world_generator_name(const World *w)  { return world_gen_name(w->gen_index); }
unsigned int world_seed(const World *w)            { return w->params.seed; }
WorldGenParams *world_params(World *w)             { return &w->params; }

void world_set_sea_level(World *w, float v) {
    w->params.sea_level = Clamp(v, -0.6f, 0.6f);
    w->dirty = true;
}
void world_set_warmth(World *w, float v) {
    w->params.warmth = Clamp(v, -0.5f, 0.5f);
    w->dirty = true;
}
void world_set_mountain_level(World *w, float v) {
    w->params.mountain_level = Clamp(v, 0.2f, 0.9f);
    w->dirty = true;
}
void world_set_noise_scale(World *w, float v) {
    w->params.noise_scale = Clamp(v, 0.6f, 4.0f);
    w->dirty = true;
}
void world_set_rain_shadow(World *w, float v) {
    w->params.rain_shadow = Clamp(v, 0.0f, 1.0f);
    w->dirty = true;
}
void world_set_moisture_reach(World *w, float v) {
    w->params.moisture_reach = Clamp(v, 0.2f, 3.0f);
    w->dirty = true;
}
void world_set_hydrology(World *w, bool on) {
    if (w->params.enable_hydrology != on) { w->params.enable_hydrology = on; w->dirty = true; }
}
bool world_hydrology_enabled(const World *w) { return w->params.enable_hydrology; }
void world_set_river_density(World *w, float v) {
    w->params.river_density = Clamp(v, 0.0f, 2.0f);
    w->dirty = true;
}
void world_set_plate_count(World *w, int v) {
    if (v < 2) v = 2; else if (v > 64) v = 64;
    if (w->params.plate_count != v) { w->params.plate_count = v; w->dirty = true; }
}

void world_set_view(World *w, int mode) {
    if (mode < 0 || mode >= WORLD_VIEW_COUNT) return;
    w->view_mode = mode;
    world_recolor(w);
}
int world_view(const World *w) { return w->view_mode; }

static void begin_mode3d_rect(Camera3D camera, float x, float y, float fw, float fh) {
    int screen_h = GetScreenHeight();
    rlDrawRenderBatchActive();
    rlViewport((int)x, screen_h - (int)(y + fh), (int)fw, (int)fh);

    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    double aspect = (double)fw / (double)fh;
    double top = RL_CULL_DISTANCE_NEAR * tan(camera.fovy * 0.5 * DEG2RAD);
    double right = top * aspect;
    rlFrustum(-right, right, -top, top, RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);

    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    Matrix view = MatrixLookAt(camera.position, camera.target, camera.up);
    rlMultMatrixf(MatrixToFloat(view));
    rlEnableDepthTest();
}

static void end_mode3d_rect(void) {
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableDepthTest();
    rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
}

static int world_pick(const World *w, float mx, float my, float x, float y, float fw, float fh) {
    float ndcx = 2.0f * (mx - x) / fw - 1.0f;
    float ndcy = 1.0f - 2.0f * (my - y) / fh;
    Vector3 fwd   = Vector3Normalize(Vector3Subtract(w->cam.target, w->cam.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, w->cam.up));
    Vector3 up    = Vector3CrossProduct(right, fwd);
    float tanHalf = tanf(w->cam.fovy * 0.5f * DEG2RAD);
    float aspect  = fw / fh;
    Vector3 dir = Vector3Normalize(Vector3Add(fwd,
                    Vector3Add(Vector3Scale(right, ndcx * tanHalf * aspect),
                               Vector3Scale(up,    ndcy * tanHalf))));
    Vector3 O = w->cam.position;
    float b = Vector3DotProduct(O, dir);
    float c = Vector3DotProduct(O, O) - 1.0f;
    float disc = b * b - c;
    if (disc < 0.0f) return -1;
    float t = -b - sqrtf(disc);
    if (t < 0.0f) t = -b + sqrtf(disc);
    if (t < 0.0f) return -1;
    Vector3 hit = Vector3Normalize(Vector3Add(O, Vector3Scale(dir, t)));

    int best = -1; float bestdot = -2.0f;
    for (int i = 0; i < w->planet->cell_count; i++) {
        PlanetV3 p = w->planet->pos[i];
        float d = hit.x * p.x + hit.y * p.y + hit.z * p.z;
        if (d > bestdot) { bestdot = d; best = i; }
    }
    return best;
}

static Vector3 nudge_to_cam(Vector3 v, Vector3 cam, float eps) {
    Vector3 d = Vector3Normalize(Vector3Subtract(cam, v));
    return (Vector3){ v.x + eps * d.x, v.y + eps * d.y, v.z + eps * d.z };
}

static void draw_selection_highlight(const World *w) {
    if (w->selected < 0 || w->selected >= w->planet->cell_count) return;
    const Planet *p = w->planet;
    int c = w->selected, deg = p->degree[c];
    Vector3 cam = w->cam.position;
    const float eps = 0.008f;
    Vector3 ctr = nudge_to_cam((Vector3){ p->pos[c].x, p->pos[c].y, p->pos[c].z }, cam, eps);
    for (int k = 0; k < deg; k++) {
        PlanetV3 a = p->corner_pos[p->cell_corners[c][k]];
        PlanetV3 b = p->corner_pos[p->cell_corners[c][(k + 1) % deg]];
        Vector3 A = nudge_to_cam((Vector3){ a.x, a.y, a.z }, cam, eps);
        Vector3 B = nudge_to_cam((Vector3){ b.x, b.y, b.z }, cam, eps);
        DrawTriangle3D(ctr, A, B, (Color){ 255, 236, 80, 110 });
        DrawLine3D(A, B, (Color){ 255, 230, 40, 255 });
    }
}

static void draw_loading_box(const World *w, float x, float y, float fw, float fh) {
    const int pad = 16, titleH = 26, lineH = 22, boxW = 232;
    int rows = GEN_STAGE_COUNT - 1;
    int boxH = pad * 2 + titleH + rows * lineH + 8;
    int bx = (int)(x + (fw - boxW) * 0.5f);
    int by = (int)(y + (fh - boxH) * 0.5f);

    DrawRectangle(bx, by, boxW, boxH, (Color){ 16, 18, 26, 235 });
    DrawRectangleLines(bx, by, boxW, boxH, (Color){ 70, 120, 180, 255 });
    DrawText("Generating world", bx + pad, by + pad, 18, (Color){ 220, 224, 232, 255 });

    int barY = by + pad + titleH - 6;
    float prog = (float)(w->gen_stage - 1) / (float)(GEN_STAGE_COUNT - 1);
    DrawRectangle(bx + pad, barY, boxW - 2 * pad, 4, (Color){ 40, 44, 54, 255 });
    DrawRectangle(bx + pad, barY, (int)((boxW - 2 * pad) * prog), 4, (Color){ 90, 150, 210, 255 });

    int ty = by + pad + titleH + 8;
    for (int s = 1; s < GEN_STAGE_COUNT; s++) {
        Color c; const char *mark;
        if      (s <  w->gen_stage) { c = (Color){ 120, 190, 130, 255 }; mark = "[x]"; }
        else if (s == w->gen_stage) { c = (Color){ 235, 205,  90, 255 }; mark = "[>]"; }
        else                        { c = (Color){ 110, 116, 128, 255 }; mark = "[ ]"; }
        DrawText(TextFormat("%s %s", mark, GEN_STAGE_NAMES[s]), bx + pad, ty, 16, c);
        ty += lineH;
    }
}

static void build_river_mesh(World *w) {
    const Planet *p = w->planet;
    if (w->river_valid) { planet_model_unload(&w->river_model); w->river_valid = false; }

    int nseg = 0;
    for (int c = 0; c < p->cell_count; c++)
        if (w->river[c] && w->downhill[c] >= 0) nseg++;
    if (nseg == 0) return;

    int segs = 8 - p->level;
    if (segs < 1) segs = 1; if (segs > 4) segs = 4;

    int tris  = nseg * segs * 2;
    int verts = tris * 3;
    Mesh mesh = { 0 };
    mesh.triangleCount = tris;
    mesh.vertexCount   = verts;
    mesh.vertices = (float *)MemAlloc((unsigned)verts * 3 * sizeof(float));
    mesh.normals  = (float *)MemAlloc((unsigned)verts * 3 * sizeof(float));
    mesh.colors   = (unsigned char *)MemAlloc((unsigned)verts * 4);

    float base = 0.128f / (float)p->frequency;
    const Color col = { 55, 120, 205, 255 };
    int vi = 0;
    for (int c = 0; c < p->cell_count; c++) {
        int order = w->river[c];
        if (order == 0) continue;
        int d = w->downhill[c];
        if (d < 0) continue;

        float r    = base * (0.5f + 0.5f * (float)order);
        float lift = 1.0f + r + 0.001f;
        Vector3 a   = { p->pos[c].x, p->pos[c].y, p->pos[c].z };
        Vector3 b   = { p->pos[d].x, p->pos[d].y, p->pos[d].z };
        Vector3 dir = Vector3Normalize(Vector3Subtract(b, a));

        Vector3 pL = {0}, pR = {0};
        for (int s = 0; s <= segs; s++) {
            float t = (float)s / (float)segs;
            Vector3 on   = Vector3Normalize(Vector3Lerp(a, b, t));
            Vector3 perp = Vector3Normalize(Vector3CrossProduct(dir, on));
            Vector3 L = Vector3Scale(Vector3Normalize(Vector3Add(on, Vector3Scale(perp, r))), lift);
            Vector3 R = Vector3Scale(Vector3Normalize(Vector3Subtract(on, Vector3Scale(perp, r))), lift);
            if (s > 0) {
                Vector3 quad[6] = { pL, pR, R, pL, R, L };
                for (int k = 0; k < 6; k++) {
                    Vector3 V = quad[k], N = Vector3Normalize(V);
                    mesh.vertices[vi*3+0] = V.x; mesh.vertices[vi*3+1] = V.y; mesh.vertices[vi*3+2] = V.z;
                    mesh.normals[vi*3+0]  = N.x; mesh.normals[vi*3+1]  = N.y; mesh.normals[vi*3+2]  = N.z;
                    mesh.colors[vi*4+0] = col.r; mesh.colors[vi*4+1] = col.g;
                    mesh.colors[vi*4+2] = col.b; mesh.colors[vi*4+3] = col.a;
                    vi++;
                }
            }
            pL = L; pR = R;
        }
    }

    UploadMesh(&mesh, false);
    w->river_model = LoadModelFromMesh(mesh);
    w->river_valid = true;
}

void world_draw_in_rect(World *w, float x, float y, float fw, float fh) {
    if (fw < 1.0f || fh < 1.0f) return;

    w->globe_x = x; w->globe_y = y; w->globe_fw = fw; w->globe_fh = fh;
    w->globe_rect_valid = true;

    bool suppressed = w->input_suppressed;
    w->input_suppressed = false;

    Vector2 m = GetMousePosition();
    bool over = !suppressed && CheckCollisionPointRec(m, (Rectangle){ x, y, fw, fh });
    if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        w->globe_pressed = true;
        w->press_pos = m;
    }
    if (over && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Vector2 d = GetMouseDelta();
        w->yaw   -= d.x * 0.005f;
        w->pitch  = Clamp(w->pitch + d.y * 0.005f, -1.4f, 1.4f);
        if (fabsf(d.x) + fabsf(d.y) > 0.5f) w->auto_rotate = false;
    }
    if (over) w->dist = Clamp(w->dist - GetMouseWheelMove() * 0.2f, 1.4f, 8.0f);
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (w->globe_pressed) {
            float dx = m.x - w->press_pos.x, dy = m.y - w->press_pos.y;
            if (dx * dx + dy * dy < 36.0f) {
                w->selected = world_pick(w, m.x, m.y, x, y, fw, fh);
                if (w->selected >= 0) w->auto_rotate = false;
            }
        }
        w->globe_pressed = false;
    }
    if (w->auto_rotate) w->yaw += GetFrameTime() * 0.15f;

    w->cam.position = (Vector3){
        w->dist * cosf(w->pitch) * sinf(w->yaw),
        w->dist * sinf(w->pitch),
        w->dist * cosf(w->pitch) * cosf(w->yaw),
    };

    if (w->model_valid) {
        BeginScissorMode((int)x, (int)y, (int)fw, (int)fh);
        begin_mode3d_rect(w->cam, x, y, fw, fh);
        DrawModel(w->model, (Vector3){ 0, 0, 0 }, 1.0f, WHITE);
        if (w->river_valid) {
            rlDisableBackfaceCulling();
            DrawModel(w->river_model, (Vector3){ 0, 0, 0 }, 1.0f, WHITE);
            rlEnableBackfaceCulling();
        }
        draw_selection_highlight(w);
        end_mode3d_rect();
        EndScissorMode();
    }

    if (w->gen_stage != GEN_IDLE) {
        draw_loading_box(w, x, y, fw, fh);
        if (w->gen_box_shown) {
            world_step_generation(w);
            w->gen_box_shown = false;
        } else {
            w->gen_box_shown = true;
        }
    }
}

#define WORLD_CTL_COUNT 9
enum { CTL_SEA, CTL_TEMP, CTL_MTN, CTL_DETAIL, CTL_LEVEL,
       CTL_RAINSHADOW, CTL_MOISTURE, CTL_RIVER, CTL_PLATES };

enum { FMT_FLOAT = 0, FMT_LEVEL, FMT_INT };

void world_draw_controls_in_rect(World *w, float x, float y, float fw, float fh) {
    if (fw < 40.0f || fh < 40.0f) return;

    const char *labels[WORLD_CTL_COUNT] = { "Sea level", "Global temp", "Mountain", "Detail",
                                            "Resolution", "Rain shadow", "Moisture", "Rivers",
                                            "Plates" };
    const float mins[WORLD_CTL_COUNT]   = { -0.6f, -0.5f, 0.2f, 0.6f, 0.0f, 0.0f, 0.2f, 0.0f, 2.0f };
    const float maxs[WORLD_CTL_COUNT]   = {  0.6f,  0.5f, 0.9f, 4.0f,
                                            (float)PLANET_MAX_LEVEL, 1.0f, 3.0f, 2.0f, 24.0f };
    const int   fmt[WORLD_CTL_COUNT] = { FMT_FLOAT, FMT_FLOAT, FMT_FLOAT, FMT_FLOAT, FMT_LEVEL,
                                         FMT_FLOAT, FMT_FLOAT, FMT_FLOAT, FMT_INT };
    WorldGenParams *p = &w->params;
    const float cur[WORLD_CTL_COUNT] = {
        p->sea_level, p->warmth, p->mountain_level, p->noise_scale, (float)w->pending_level,
        p->rain_shadow, p->moisture_reach, p->river_density, (float)p->plate_count
    };

    Vector2 m = GetMousePosition();
    bool pressed  = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    bool released = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    const float labelW = 104.0f, valueW = 60.0f;
    float trackX0 = x + labelW;
    float trackX1 = x + fw - valueW;
    if (trackX1 < trackX0 + 24.0f) trackX1 = trackX0 + 24.0f;
    float trackW = trackX1 - trackX0;
    float rowh = fh / (float)WORLD_CTL_COUNT;

    for (int i = 0; i < WORLD_CTL_COUNT; i++) {
        float trackY = y + (i + 0.5f) * rowh;
        Rectangle hit = { trackX0 - 8, trackY - 12, trackW + 16, 24 };
        if (pressed && CheckCollisionPointRec(m, hit)) w->ui_drag = i;

        float value = cur[i];
        int active = (w->ui_drag == i);
        if (active) {
            float t = Clamp((m.x - trackX0) / trackW, 0.0f, 1.0f);
            value = mins[i] + t * (maxs[i] - mins[i]);
            if (fmt[i] != FMT_FLOAT) value = floorf(value + 0.5f);
            w->ui_pending = value;
        }

        float tnorm = (value - mins[i]) / (maxs[i] - mins[i]);
        float hx = trackX0 + trackW * tnorm;
        DrawText(labels[i], (int)x, (int)(trackY - 8), 16, (Color){ 200, 204, 212, 255 });
        DrawRectangle((int)trackX0, (int)(trackY - 3), (int)trackW, 6, (Color){ 58, 62, 72, 255 });
        DrawRectangle((int)trackX0, (int)(trackY - 3), (int)(trackW * tnorm), 6, (Color){ 90, 140, 200, 255 });
        DrawRectangle((int)(hx - 5), (int)(trackY - 9), 10, 18,
                      active ? RAYWHITE : (Color){ 214, 218, 224, 255 });
        const char *vt = fmt[i] == FMT_LEVEL ? TextFormat("L%d", (int)value)
                        : fmt[i] == FMT_INT  ? TextFormat("%d", (int)value)
                                             : TextFormat("%+.2f", value);
        DrawText(vt, (int)(trackX1 + 8), (int)(trackY - 8), 16, (Color){ 200, 204, 212, 255 });
    }

    if (released && w->ui_drag >= 0) {
        int i = w->ui_drag;
        float v = w->ui_pending;
        w->ui_drag = -1;
        switch (i) {
            case CTL_SEA:        world_set_sea_level(w, v);        break;
            case CTL_TEMP:       world_set_warmth(w, v);           break;
            case CTL_MTN:        world_set_mountain_level(w, v);   break;
            case CTL_DETAIL:     world_set_noise_scale(w, v);      break;
            case CTL_LEVEL:      world_set_pending_level(w, (int)v); break;
            case CTL_RAINSHADOW: world_set_rain_shadow(w, v);      break;
            case CTL_MOISTURE:   world_set_moisture_reach(w, v);   break;
            case CTL_RIVER:      world_set_river_density(w, v);    break;
            case CTL_PLATES:     world_set_plate_count(w, (int)v); break;
        }
    }
}
