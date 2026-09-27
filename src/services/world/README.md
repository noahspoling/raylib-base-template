# World Service

The World service owns a procedurally generated planet: its geometry, the
per-tile simulation state (elevation, temperature, humidity, terrain), and the
rendered globe. It is the single handle the app ([main.c](../../main.c)), the Lua
bindings (`gramarye.world.*`), and the UI globe seam all talk to.

This document explains how the pieces fit together and, more importantly, how to
extend it: adding biomes, new generators, regions, terrain features, and rivers.

## Contents

- [Architecture at a glance](#architecture-at-a-glance)
- [File map](#file-map)
- [Data model](#data-model)
- [The generation pipeline](#the-generation-pipeline)
- [Terrain classification](#terrain-classification)
- [Rendering](#rendering)
- [UI and Lua seams](#ui-and-lua-seams)
- [How to extend](#how-to-extend)
  - [Add a biome](#add-a-biome)
  - [Add a generator algorithm](#add-a-generator-algorithm)
  - [Add more per-tile fields](#add-more-per-tile-fields)
  - [Add rivers](#add-rivers)
  - [Add regions](#add-regions)
  - [Add terrain features](#add-terrain-features)
- [Performance notes](#performance-notes)
- [Gotchas](#gotchas)

## Architecture at a glance

The planet is an icosahedral hex globe (a Goldberg polyhedron). Start from an
icosahedron, subdivide every triangle `level` times, and project each vertex onto
the unit sphere. Each vertex is one simulation cell. The cell count is fixed by
the level:

```
F = 2^level              (frequency)
cell_count = 10*F*F + 2
```

Every cell has exactly 6 neighbors EXCEPT the 12 cells sitting on the original
icosahedron corners, which have 5 (the pentagons). That is the only irregularity
at any resolution. There are no poles and no map edges to special-case.

State is stored in a deliberate hybrid:

- **ECS `TileComp` entities are the source of truth.** They are rich, queryable,
  and iterable by systems (erosion, climate) through the ECS dense mirror.
- **A cell-indexed flat mirror** (parallel `elevation` / `temperature` /
  `humidity` / `terrain` arrays in the `World` struct) shadows them, so
  neighbor-heavy passes and mesh building are pointer walks rather than a UUID
  hash lookup per neighbor.

Regeneration writes the flat mirror first, then syncs it into the `TileComp`s.
Keep that ordering in mind for every extension below.

## File map

| File | Role |
| --- | --- |
| [planet.h](../../../include/services/world/planet.h) / [planet.c](planet.c) | Pure geometry: builds the geodesic, derives per-cell neighbors and dual polygon corners. No raylib dependency. |
| [world_gen.h](../../../include/services/world/world_gen.h) / [world_gen.c](world_gen.c) | Generator interface + built-in generators, the noise toolbox (fBm, ridged, domain warp), climate helpers, and `tile_classify()`. |
| [world.h](../../../include/services/world/world.h) / [world.c](world.c) | The service: lifecycle, flat mirror, tile entities, stepped generation + loading box, the orbit camera, click-to-inspect picking, map-mode views, and the parameter sliders. |
| [world_pipeline.h](../../../include/services/world/world_pipeline.h) | The `WorldFields` view + the pass entry points shared by the modules below. |
| [world_tectonics.c](world_tectonics.c) | Tectonics pass: plate seeds + rigid motion bias elevation (mountains/rifts) and write fault data (see "Plate tectonics" below). |
| [world_climate.c](world_climate.c) | Climate pass: moisture advection (via the wind field below), orographic rain shadow, rainfall (+ smoothing). |
| [world_wind.c](world_wind.c) | The analytic wind model and the coarse `WindField` control layer climate samples (see "Wind field" below). |
| [world_biomes.c](world_biomes.c) | Biome pass: Gaussian classify (via `tile_classify`) + despeckle. |
| [world_hydrology.c](world_hydrology.c) | Rivers & lakes (opt-in): priority-flood, flow accumulation, lake basins. |
| [world_regions.c](world_regions.c) | Province labeling by terrain-weighted multi-source Dijkstra (borders follow ridges/rivers/coasts). |
| [planet_render.h](../../../include/services/world/planet_render.h) / [planet_render.c](planet_render.c) | Builds and recolors the raylib `Model` from the planet's dual faces (incl. per-cell field recolor for map-mode views). |
| [tile.h](../../../include/components/tile.h) / `components/tile.c` | The `TileComp` component, the `TileTerrain` enum, the render palette, and terrain names. |
| [world_lua.h](../../../include/services/world/world_lua.h) | Lua bindings (`gramarye.world.*`). |

## Data model

**Geometry (`Planet`, immutable once built):**

- `pos[cell]` unit-sphere center of each cell.
- `neighbors[cell][6]` adjacent cell indices, `-1` padded (slot 5 unused on
  pentagons).
- `degree[cell]` 5 or 6.
- `corner_pos[]` and `cell_corners[cell][6]` the dual polygon corners for
  rendering each cell's face as a triangle fan.

Geometry is built once and never mutated by the sim. Tiles reference it by cell
index, so the per-tile component stays small.

**Per-tile state (`TileComp`):**

```c
typedef struct TileComp {
    int32_t cell;        // index into Planet geometry
    float   elevation;   // [-1, 1]  (< sea_level is underwater)
    float   temperature; // [0, 1]
    float   humidity;    // [0, 1]
    uint8_t terrain;     // TileTerrain, DERIVED from the fields above
} TileComp;
```

Terrain is never authored directly. It is derived from the raw fields by
`tile_classify()` so the classification rule is shared across every generator.

**Flat mirror (in the `World` struct):** `elevation`, `temperature`, `humidity`
(float arrays) plus `terrain` (byte array) and `cells` (the `EntityId` per cell).
All are `cell_cap` long and reallocated whenever the level changes.

## The generation pipeline

A generator turns a cell's position on the unit sphere into the three raw fields.
It is a small vtable so the UI can offer several algorithms and switch at runtime:

```c
typedef struct WorldGenerator {
    const char *name;
    WorldGenSample (*sample)(const struct WorldGenerator *self,
                             const WorldGenParams *params,
                             PlanetV3 pos, int cell);
    void *state; // optional per-generator data (unused by the built-ins)
} WorldGenerator;
```

`world_regenerate()` (in [world.c](world.c)) is the heart of the loop. For each
cell it:

1. calls the active generator's `sample()` to get elevation/temperature/humidity,
2. writes them into the flat mirror,
3. runs `tile_classify()` to derive `terrain`,
4. syncs all four into the cell's `TileComp` in place (no per-cell allocation),
5. rebuilds or recolors the mesh once at the end.

`sample()` must be **pure** with respect to `(params, pos)` so regeneration is
deterministic for a given seed. Anything that needs neighbor context (rivers,
erosion, region growth) does NOT belong inside `sample()`; it belongs in a
**post-pass** over the flat mirror after the per-cell loop (see
[How to extend](#how-to-extend)).

### Staging and apply (the edit model)

Edits are **staged, not applied immediately.** Every parameter/generator/seed/
resolution change records the new value and sets a `dirty` flag; nothing rebuilds
until `world_apply()` runs. This keeps slider drags free on large worlds: the one
expensive path (regenerate, and a geometry rebuild if the resolution changed)
happens once, when the Regenerate button is clicked.

| Call | What it does |
| --- | --- |
| `world_set_sea_level` / `set_warmth` / `set_mountain_level` / `set_noise_scale` | Clamp and **stage** the param; set `dirty`. |
| `world_set_generator(index)` | Stage the algorithm; set `dirty`. |
| `world_reroll()` | Stage a new random seed; set `dirty`. |
| `world_set_seed(seed)` | Stage an explicit seed (e.g. from a text box); set `dirty`. |
| `world_set_pending_level(n)` | Stage a target resolution; set `dirty`. |
| `world_dirty()` | Whether staged edits are pending (drives the button's enabled state). |
| `world_apply()` | Commit everything: rebuild geometry + tile entities **only if** the staged level differs from the built one, then `world_regenerate()`, then clear `dirty`. |
| `world_regenerate()` | The internal per-apply pass (steps 1-5 above) at the current built level. Callers usually want `world_apply()`. |
| `world_set_level(n)` | Convenience: stage the level and `world_apply()` in one call. |

So `world_regenerate()` runs *inside* `world_apply()`. When you add a neighbor
post-pass, put it in `world_regenerate()` so every apply picks it up.

### The post-pass pipeline (implemented)

`world_regenerate()` no longer classifies terrain inline. It now runs an ordered
pipeline over the flat mirror, each stage a module operating on a `WorldFields`
view (see [world_pipeline.h](../../../include/services/world/world_pipeline.h)):

1. **Sample** — per-cell raw elevation/temperature/base-humidity from the active
   generator (context-free, pure).
2. **Tectonics** ([world_tectonics.c](world_tectonics.c)) — biases the elevation
   from Sample with a plate layer, so mountain ranges/rifts/continent shapes
   read as coherent structures instead of purely noise-driven blobs, and
   writes per-cell fault data for later systems. See "Plate tectonics" below.
3. **Climate** ([world_climate.c](world_climate.c)) — sample the wind field (see
   "Wind field" below) -> moisture advection inland, weighted across each
   cell's neighbors by alignment with the sampled wind -> orographic rain
   shadow (against the single most-upwind neighbor) -> rainfall. Land also
   **recycles a little of its own moisture back in** each advection sweep when
   it's hot (an evapotranspiration stand-in), self-limiting since there's
   nothing to recycle where moisture is already near zero — without it,
   rainfall only ever fringes the coast and can never sustain an Amazon/Congo-
   style wet interior. The rainfall field is then **box-blurred over neighbors
   (a few passes)** so biomes form coherent zones instead of salt-and-pepper;
   `humidity` mirrors the smoothed rainfall. RP2-inspired, trimmed to the
   high-impact layers.

   **Plate tectonics** ([world_tectonics.c](world_tectonics.c)) — runs BEFORE
   Climate (rain shadow needs the final heightfield), and is the same coarse
   control-layer architecture as the wind field below, applied to plate motion
   instead of wind direction: a `PlateField` of a few dozen plate seeds on a
   **jittered Fibonacci lattice** (jitter keyed off `seed`, so unlike wind this
   layout is seed-dependent and rebuilds every regenerate), indexed by the
   same libcore `KDTree_T`. Each plate carries **rigid motion parameterized
   the way real plates are**: a rotation axis (Euler pole) + signed angular
   speed, so a plate's velocity at any point is a cheap `axis x point * speed`
   cross product — no simulation. Each plate is also flagged oceanic or
   continental (~65% oceanic, to read Earth-like), which biases baseline
   elevation even far from any boundary, so continents and ocean basins get a
   coherent macro-shape instead of a threshold cut through undifferentiated
   noise. That baseline is blended over the nearest 6 plates by inverse
   distance (`KDTree_nearest(k=6)`, same idea as `world_wind_sample`) rather
   than taken from the single nearest plate — a hard nearest-1 pick makes the
   baseline JUMP the instant a cell's nearest plate changes, a visible
   straight-edged polygon cut across the noise; blending fades it in smoothly
   instead. The same query's nearest TWO results are also used for the
   boundary/fault signal: the GAP between nearest and second-nearest distance
   is a free, continuous "how close am I to a plate boundary" value (0 exactly
   on the Voronoi boundary, growing into either plate's interior — no separate
   neighbor-walk or falloff pass needed). Where that gap is small, the two
   plates' relative velocity there is decomposed
   against the boundary direction: dominantly closing the gap is
   **convergent** (uplift — mountain belts), dominantly opening it is
   **divergent** (rift valleys), dominantly sideways is **transform**
   (negligible elevation change, but still registers stress). Every cell's
   `plate` id, `fault` kind, and `stress` magnitude are kept in the flat
   mirror and `TileComp` — not just consumed for elevation — specifically so
   later systems (earthquakes, hot springs, resource placement, local-
   generation hazards, ...) can query "is this near an active fault" without
   re-deriving plate geometry themselves. `plate_count` [2..64] is a slider
   (`WorldGenParams.plate_count`, default 10); the boundary falloff width
   scales with plate spacing, so fewer/bigger plates draw proportionally
   wider mountain belts rather than thinner ones.

   **Wind field** ([world_wind.c](world_wind.c)) — wind is NOT computed per
   tile. `world_climate_wind_at(pos)` is the raw analytic model (trade winds /
   westerlies / polar easterlies by latitude band, closed form); it seeds a
   coarse `WindField` of a few hundred points on a **Fibonacci sphere lattice**
   (deterministic, independent of seed and planet level), indexed by a libcore
   `KDTree_T`. Every consumer — climate's advection, and the Wind debug view —
   samples that field by 3D proximity (`world_wind_sample`: inverse-distance-
   weighted blend of the nearest 4 control points) instead of reading a
   per-cell value. This is what fixed the old "streaky" rain shadow: the
   previous version snapped each cell's wind onto whichever single hex
   neighbor was closest to opposite it, and because the hex lattice's local
   neighbor directions rotate irregularly from cell to cell (there is no
   consistent local grid axis, unlike a square grid), nearby cells could snap
   to different neighbors under near-identical wind, and advection walked that
   disagreement into a visible thread over many sweeps. Sampling a smooth
   field by 3D distance removes the discrete choice entirely — nearby fine
   cells now get nearly identical wind regardless of local mesh topology — and
   the advection sweep itself blends across **all** of a cell's neighbors
   (weighted by alignment with the sampled wind, squared) rather than pulling
   from one winner-take-all pick. The `WindField` is built once in
   `world_create` (it costs nothing to keep — it doesn't depend on seed or
   level) and reused for every regenerate; the k-d tree also makes cheap
   radius queries available for later work (e.g. a drifting El-Nino-style
   anomaly overlay), which a linear scan over hundreds of thousands of tiles
   would not be.
4. **Biomes** ([world_biomes.c](world_biomes.c)) — `tile_classify()` (a **Gaussian
   ideal±tolerance scorer** with a `BiomeProfile` table in
   [world_gen.c](world_gen.c), kept there so `planet_demo` links) scores every
   cell, then a **despeckle** pass replaces isolated single-cell biomes with the
   dominant land biome around them. Axes combine **multiplicatively**
   (temperature × humidity, with elevation a soft modifier), so a cold biome whose
   temperature curve is ~0 can't win a warm tile on its broad humidity term — this
   is what keeps snow off warm lowlands. Data-driven, coherent, no if/else ladder.
5. **Hydrology** ([world_hydrology.c](world_hydrology.c)) — **opt-in**
   (`enable_hydrology`, default off). Priority-flood+ε (libcore `Heap`) removes
   sinks; drainage direction is then **steepest descent** on the filled surface
   (following valleys and merging tributaries into trunks — not the flood's
   discovery order, which fans into parallel lanes). Flow accumulates downhill.
   Rivers are then
   classified by **Strahler stream order** over that tree (headwaters = order 1;
   two equal orders merging raise it), so the network is a real hierarchy — fat
   trunks near the sea, finer tributaries upstream. Only order >= `min_order` is
   drawn, hiding the small rivulets that otherwise clutter the map; **`min_order`
   scales with resolution** (low at coarse levels so tiny networks still read as
   connected rivers, higher at fine levels for a clean trunk+tributary hierarchy)
   and is shifted by `river_density`. A river is a per-tile `river` width attribute
   (1..5 by order) — the tile keeps its biome. The river network is baked into **one
   cached mesh** (`build_river_mesh` in [world.c](world.c), rebuilt only on
   regeneration and drawn in a single call — thousands of per-frame primitives
   dragged badly at L8): every river cell contributes a flat ribbon from its own
   hex center to its `downhill` neighbor's center, arc-subdivided and re-projected
   onto the sphere so it hugs the surface. Because every river cell adds its
   outgoing ribbon, the drainage tree forms **one continuous line** — tributaries
   merging into fatter trunks — that **always runs into a water tile** (the segment
   into the sea/lake is included, so no river ends randomly on land). Ribbon width
   scales with Strahler order **and with cell size (~1/frequency)**, so a river
   stays ~1-2 cells wide at any resolution (a fixed width ballooned into blobs at
   high levels). Rivers look best at L6+; at L4 the
   networks are inherently tiny. Filled basins (libcore `UFind`) become **lakes**
   only where their catchment holds up under a water balance: each basin's inflow
   is the accumulated rainfall (`flow`) reaching its lowest point, weighed against
   an evaporative loss that scales with the basin's own area — a basin under a wet
   catchment fills, a basin in a rain shadow stays a dry pan no matter how deep it
   is. Not a tunable: there's no abundance knob, just the same rainfall field
   climate already computed. Lakes that qualify fill their whole basin and grow
   onto shallow shores (bounded by the water surface) so they read as sizeable
   bodies, while a one-cell land buffer keeps them from ever touching the ocean.
6. **Regions** ([world_regions.c](world_regions.c)) — **province** labeling by
   **terrain-weighted multi-source Dijkstra** (libcore `Heap`). Seeds start in
   lowlands and grow with a cost that's cheap on plains and expensive across
   mountains and rivers, so province borders settle onto ridgelines, rivers, and
   coasts — natural-looking, rather than the straight bisectors a plain hop-count
   BFS (graph Voronoi) produces, and never a single connected-component
   mega-region. A little cost noise keeps borders organic. Province count is
   resolution-independent (`REGION_TARGET_CELLS`); growth never crosses ocean.

The reusable structures the neighbor passes needed live in **gramarye-libcore**:
a value-semantics binary heap (`heap.h`), a union-find (`ufind.h`), and a static
k-d tree (`kdtree.h`, nearest-K and radius queries) backing both the wind field
and the plate field's coarse control-point sampling.

**Tunable parameters (`WorldGenParams`):** the base `seed`, `sea_level`,
`mountain_level`, `noise_scale`, `warmth`, plus pipeline knobs `rain_shadow`,
`moisture_reach` (climate), `plate_count` (tectonics), and `enable_hydrology`,
`river_density` (hydrology). Lakes have no knob — see the water-balance rule
above. Setters in [world.c](world.c) clamp each and **stage**
it (see the table above); the world only rebuilds on `world_apply()`. All are
exposed to Lua under `gramarye.world.*` (`set_hydrology`, `set_rain_shadow`,
`set_plate_count`, ...), and a debug map-mode `set_view(n)` recolors the globe
by terrain / temperature / rainfall / flow / region / wind / elevation /
moisture / plates (`WORLD_VIEW_*` in [world.h](../../../include/services/world/world.h))
without regenerating. Wind colors by sampled direction (hue) via `ColorFromHSV`;
Elevation shows the raw continuous height field pre-classification; Moisture
shows climate's pre-smoothing advected value (compare against Rainfall, which
is post-smoothing) — together they're the way to eyeball a climate change (like
the wind field above) before it reaches biomes. Plates colors by plate id
(random hue, like Region) with fault lines overdrawn in a saturated color per
boundary kind (red convergent, teal divergent, yellow transform), intensity by
stress — the way to eyeball the tectonics pass the same way, before its bias
reaches elevation-dependent downstream passes.

**The noise toolbox** (in [world_gen.c](world_gen.c), reusable by any generator):

- `fbm(pos, freq, octaves, seed)` fractal Brownian motion in `[-1, 1]`.
- `ridged(pos, freq, octaves, seed)` sharp crests for ridges and island chains.
- `warp(pos, freq, amt, seed)` domain warp so coastlines meander.
- `value_noise(...)` / `hashf(...)` the self-contained seedable base noise.

**Climate helpers** (shared so every algorithm speaks the same units):

- `warped_latitude(pos, seed)` `[0,1]`, 0 at equator, 1 at a pole, edges wiggled.
- `climate_temperature(params, lat, elevation)` latitude + lapse rate + `warmth`.
- `climate_humidity(lat)` idealized wet equator, dry subtropics, wet temperate belt.

## Terrain classification

`tile_classify()` is a Whittaker-style rule: elevation gates water/land/mountain,
then temperature x humidity picks the biome. It is the one place terrain is
decided, shared by all generators. Read it top to bottom in
[world_gen.c](world_gen.c) before adding biomes; the order of the early returns
matters (water and coast first, then high ground, then lowland climate).

## Rendering

[planet_render.c](planet_render.c) builds a flat-shaded, **indexed, chunked**
mesh. Each cell face is a triangle fan (`center -> corner k -> corner k+1`)
colored by `TILE_TERRAIN_COLORS[terrain[cell]]`.

- **Indexed:** a cell's fan shares its center + corner vertices, so a hexagon
  stores 7 vertices and 18 indices instead of 18 standalone vertices (~2.6x fewer
  vertices). Corners are shared *within one cell only*, never across cells: each
  cell's flat terrain color is a per-vertex attribute, so a boundary vertex shared
  between two cells could not carry both colors. (Cross-cell sharing would need a
  shader that colors per-face instead of per-vertex.)
- **Chunked:** raylib's `Mesh.indices` is 16-bit (`unsigned short`), so a single
  mesh can address at most 65,536 vertices. `planet_model_build()` splits the
  cells into chunks of whole cells, each under that cap
  (`PLANET_CHUNK_MAX_VERTS`), and emits one `Mesh` per chunk into a multi-mesh
  `Model`. `DrawModel` draws them all. This is what lets high levels render at
  all; a single indexed mesh would overflow the index type past about level 4.

The two build paths:

- `planet_model_build()` builds every chunk mesh (call on resolution change).
- `planet_model_update_colors()` rewrites only the per-vertex colors in place and
  re-uploads just each chunk's color VBO. It walks the **same deterministic chunk
  partition** that build produced (recomputed via `chunk_end()`, so no chunk table
  is stored) and bails if a mesh size no longer matches. This is the cheap path
  `world_regenerate()` takes whenever terrain changes without a resolution change
  (that is, an apply that did not re-level), so committing a batch of slider edits
  only recolors, it does not rebuild.

The globe is drawn into a screen-space rect mid-2D-frame by
`world_draw_in_rect()`, which also runs the orbit camera and click-to-pick.

If you add a per-vertex attribute (for example, blending terrain colors across
cell edges, or a feature overlay), remember it must be written in **both**
`build_chunk_mesh()` and, if it can change without a rebuild,
`planet_model_update_colors()` — and both walk cells in the same chunk order.

## Stepped generation and the loading box

`world_apply()` no longer runs the pipeline in one blocking call. It **starts a
stepped job** and returns; `world_step_generation()` then advances exactly one
stage per frame — geometry rebuild (only if the resolution changed), sample,
climate, biomes, hydrology, regions, finalize. The globe's own draw
(`world_draw_in_rect`) pumps the job each frame and, while it runs, draws a
**per-stage loading box** (a checklist with the current stage highlighted and a
progress bar) centered over the globe. Two wins: no multi-second frame freeze on
large worlds, and visible progress.

Notes for maintainers:

- The stage list and labels live in `world.c` (`GEN_STAGE_NAMES`). Add a stage by
  extending the enum, the names, and the `switch` in `world_step_generation()`.
- `world_regenerate()` is still the **synchronous** single-shot path (used at
  startup by `world_create`); it calls the same `stage_*` helpers in order.
- The job is pumped from `world_draw_in_rect`, so generation only advances while
  the globe node is drawn. If you host the globe somewhere it isn't drawn every
  frame, call `world_step_generation()` yourself, or use the synchronous
  `world_set_level()` path (which loops the job to completion internally).
- Progress is queryable: `world_generating()`, `world_gen_stage_name()`,
  `world_gen_stage()` / `world_gen_stage_count()`.
- Drawing the box does not mean it is on screen yet -- raylib only presents
  pixels at `EndDrawing()`, which happens back in `main.c` after this whole
  call returns. A heavy stage run in the same frame it announces itself (worst
  case: `GEN_GEOMETRY` on a big resolution jump, which respawns every tile
  entity) would stall *before* its own box ever reached the screen, leaving
  the previous frame frozen with no explanation. `gen_box_shown` fixes this:
  each stage gets one frame to only draw its box (guaranteeing it is
  presented), and runs its actual work starting the next frame. Costs one
  extra frame per stage (imperceptible; the job has 7 stages).

## UI and Lua seams

**Custom-draw seam:** a Lua `custom` UI node with `kind = WORLD_GLOBE_KIND` routes
to `world_draw_in_rect` (the globe **and** the loading box); `WORLD_CONTROLS_KIND`
routes to `world_draw_controls_in_rect` (the parameter sliders). Dragging a slider
only **stages** its value (on release it calls the staging setter, or
`world_set_pending_level` for resolution); nothing rebuilds until `world_apply()`
runs, so dragging stays responsive on the largest worlds.

**The C slider panel** now has eight rows: sea level, global temp, mountain,
detail, resolution, **rain shadow**, **moisture**, and **rivers** (river
density). Lakes have no row — they're derived, not tuned (see the hydrology
water-balance rule above). All stage-on-release. The hosting Lua `custom` node must be tall enough (~280px in
[planet.lua](../../../assets/scripts/scenes/planet.lua)); add a row by bumping
`WORLD_CTL_COUNT` and its parallel `labels`/`mins`/`maxs`/`is_level`/`cur` arrays
and the commit `switch`.

**Booleans and enums live in Lua, not the C slider panel** — a slider is the wrong
control for them. [planet.lua](../../../assets/scripts/scenes/planet.lua) uses:

- a **Checkbox** (`widgets.Checkbox`, a new reusable widget in
  [lib/widgets.lua](../../../assets/scripts/lib/widgets.lua)) for
  `enable_hydrology`, toggled via `gramarye.world.set_hydrology`;
- a **View** cycle button that calls `gramarye.world.set_view(n)` (map modes:
  terrain / temperature / rainfall / flow / region), recoloring immediately;
- the **Regenerate** button, which shows `Generating...` while a job runs
  (`gramarye.world.generating()`) and `Apply changes` when edits are staged
  (`dirty()`).

**Lua bindings** (`gramarye.world.*`, see [world_lua.h](../../../include/services/world/world_lua.h)):

- `regenerate()` — the Regenerate button: calls `world_apply()` to start the
  stepped job. The **only** binding that rebuilds the world -- everything else
  in this list only stages, so the panel can turn every control (seed box,
  reroll, algorithm cycle, sliders, checkboxes) into a free, instant edit.
- `reroll()` / `set_seed(n)` / `next_algorithm()` — **stage** a random seed /
  an explicit seed (the seed text box) / the next algorithm; `next_algorithm`
  returns the new (staged) name.
- `set_level(n)` / `level_up()` / `level_down()` — **stage** a target resolution.
- `set_sea_level` / `set_warmth` / `set_mountain_level` / `set_noise_scale` /
  `set_rain_shadow` / `set_moisture_reach` / `set_river_density`
  — stage a param; paired getters read the staged value.
- `set_hydrology(bool)` / `hydrology()` — toggle / read the rivers-and-lakes pass.
- `set_view(n)` / `view()` — map-mode recolor (immediate, no regenerate).
- `generating()` / `stage()` — job state (`stage()` returns label, index, total).
- `dirty()` — staged edits pending.
- `level`, `cells`, `algorithm`, `seed`, `selection`, `clear_selection` — reads.

When you add a public capability to the service, expose it here too so scripts and
the console can reach it.

**Click-to-inspect:** `world_selected_info()` returns a `WorldTileInfo` snapshot
(cell, raw fields, **rainfall/flow/region**, terrain, lat/lon, neighbor degree)
for an info box. Add new per-tile fields here when you want them in the inspector.

## How to extend

General rule: decide whether your feature is **per-cell and context-free**
(belongs in a generator's `sample()`) or **needs neighbors / multiple passes**
(belongs in a post-pass over the flat mirror inside `world_regenerate()`).

### Add a biome

A biome is a new `TileTerrain` value plus a color, a name, and a classification
rule. Four edits:

1. **[tile.h](../../../include/components/tile.h)** add the enum value BEFORE
   `TILE_TERRAIN_COUNT`:
   ```c
   TILE_TERRAIN_SWAMP,
   TILE_TERRAIN_COUNT
   ```
2. **`components/tile.c`** add its color to `TILE_TERRAIN_COLORS` and its label to
   `tile_terrain_name()`. The palette is indexed by the enum, so keep the
   designated initializer form (`[TILE_TERRAIN_SWAMP] = { ... }`) to stay
   order-independent.
3. **[world_gen.c](world_gen.c)** add a branch in `tile_classify()`. Mind the
   early-return order: put the swamp test inside the lowland-climate section, for
   example hot + very wet + low elevation:
   ```c
   if (temperature > 0.5f && humidity > 0.75f && elevation < sea + 0.15f)
       return TILE_TERRAIN_SWAMP;
   ```
4. Rebuild. No renderer changes are needed; the mesh colors by enum value.

That is the whole loop for "more biomes." Most of the design work is choosing the
temperature/humidity/elevation thresholds so the new biome appears where you
expect without stealing tiles from its neighbors.

### Add a generator algorithm

1. Write a `WorldGenSample gen_myworld(self, params, pos, cell)` in
   [world_gen.c](world_gen.c) following the existing three. Reuse the noise and
   climate helpers. Keep it pure.
2. Add an entry to the `GENERATORS[]` table with a display name. `GEN_COUNT` and
   the UI algorithm picker update automatically.
3. That is it. `world_set_generator()` clamps the index and the Lua
   `next_algorithm()` cycles through the table.

### Add more per-tile fields

Rivers, regions, and erosion all want fields beyond the current three. To add,
say, `rainfall` and `flow`:

1. **[tile.h](../../../include/components/tile.h)** add fields to `TileComp`.
2. **[world.c](world.c)** add matching flat-mirror arrays to `struct World`,
   `realloc` them in `alloc_and_spawn()`, and `free` them in `world_destroy()`.
3. **`world_regenerate()`** populate them (either from `sample()` output or in a
   post-pass) and sync into the `TileComp` alongside the existing fields.
4. Optionally surface them in `WorldTileInfo` / `world_selected_info()` for the
   inspector.

Adding a field is mechanical; the interesting work is the pass that fills it.

### Add rivers

> Already implemented in [world_hydrology.c](world_hydrology.c) (priority-flood +
> flow accumulation, opt-in via `enable_hydrology`). Note the shipped code renders
> rivers as **lines** following `downhill` (a per-tile `river` width attribute),
> not by overwriting terrain — the tile keeps its biome. Lakes *are* a terrain
> type. The recipe below is the general design; read it alongside that module.

Rivers are inherently a neighbor problem, so this is a **post-pass** in
`world_regenerate()` after the per-cell loop, using the flat mirror and
`planet->neighbors`. A standard flow-accumulation approach:

1. Add fields per [Add more per-tile fields](#add-more-per-tile-fields): a
   `downhill` neighbor index and a `flow` accumulator (and a `TILE_TERRAIN_RIVER`
   biome if you want to color them).
2. **Downhill pointers:** for each land cell, scan its neighbors and record the
   one with the lowest elevation (or `-1` if it is a local minimum or already
   below sea level). This is one linear pass over cells x degree.
3. **Flow accumulation:** give each land cell an initial flow proportional to its
   rainfall/humidity, then push flow downhill. The clean way is to sort cells by
   elevation descending and accumulate into each cell's `downhill` target in that
   order, so every cell is processed after everything that drains into it. One
   sort plus one linear pass.
4. **Carve or mark:** any cell whose accumulated flow exceeds a threshold becomes
   a river (set its terrain, or lower its elevation slightly to carve a valley and
   let classification handle the rest).
5. Handle local minima (lakes) by either filling depressions before step 2 (a
   priority-flood pass) or turning high-flow sinks into lake tiles.
6. Recolor the mesh (already done at the end of `world_regenerate()`).

Because `sample()` stays pure and context-free, do NOT try to compute rivers
inside it. Everything above reads the already-written flat mirror.

### Add regions

> Already implemented in [world_regions.c](world_regions.c) as **provinces**
> (terrain-weighted multi-source Dijkstra): borders follow mountains/rivers/coasts
> instead of cutting straight across, and one big continent splits into many
> provinces rather than a single connected-component mega-region. The recipe below
> covers the plain connected-component approach; the shipped code seeds and grows
> with cost weighting instead.

Regions (continents, provinces, climate zones, political territories) are a
labeling pass. Also a **post-pass** in `world_regenerate()`:

1. Add an `int32_t region` field (see [Add more per-tile fields](#add-more-per-tile-fields)).
   `-1` means unassigned.
2. **Flood fill / connected components:** walk cells; for each unassigned land
   cell, start a new region id and BFS/DFS across `planet->neighbors` to all
   connected land cells (stop at ocean). This gives you continents for free.
3. For finer regions, seed N points (poisson-ish by rejecting seeds too close on
   the sphere) and grow them simultaneously (multi-source BFS) so each cell joins
   the nearest seed's region: a Voronoi partition over the graph.
4. Store per-region aggregates (area, dominant biome, centroid) in a small side
   array indexed by region id if the game needs them.
5. Expose the region under the cursor via `WorldTileInfo`, and/or tint the globe
   by region for debugging (temporarily map region id to a color in a debug draw).

Regions are cheap: a couple of linear passes plus a queue. They pair well with
rivers (a river can define a region boundary) and features (a feature can be
placed per region).

### Add terrain features

Features are discrete things placed ON tiles rather than a change to the base
biome: volcanoes, resource deposits, forests-as-objects, settlements, ruins. Two
viable storage strategies:

- **As a tile field** (`uint8_t feature`) if a cell has at most one and you
  want it in the flat mirror for fast rendering. Follow
  [Add more per-tile fields](#add-more-per-tile-fields).
- **As separate ECS entities** referencing a `cell` index, if features are sparse,
  carry lots of their own state, or a cell can host several. This is the better
  fit for anything gameplay-heavy; it keeps `TileComp` small and lets feature
  systems iterate only over features.

Placement is a **post-pass**: after terrain (and rivers/regions) exist, scan for
eligible tiles (for example, "mountain tile adjacent to ocean" for a volcano, or
"forest tile in a temperate region" for a logging camp) and place features with a
seeded random roll so results stay deterministic. Render features as extra
geometry in a new draw pass or as billboards over `planet->pos[cell]`; the base
terrain mesh does not need to know about them.

## Performance notes

- `world_regenerate()` is O(cells) per pass. Rivers add a sort (O(cells log cells))
  and regions add a BFS (O(cells)). All comfortable at the default levels; watch
  memory and time at `PLANET_MAX_LEVEL` (currently level 8 = 655,362 cells).
- The flat mirror exists precisely so neighbor-heavy passes stay cache-friendly
  pointer walks. Write new multi-pass systems against the flat arrays, then sync
  the results into `TileComp` once, at the end, like the existing loop does.
- The mesh has a cheap recolor path (`planet_model_update_colors`) and an
  expensive rebuild path (`planet_model_build`). Terrain-only changes should use
  the recolor path; only a resolution change forces a rebuild.
- Slider drags only stage values; the regenerate/rebuild cost is paid once, when
  `world_apply()` runs (the Regenerate button), no matter how many sliders moved.
- **Mesh memory.** The mesh is indexed (vertices shared within each cell's fan),
  so it stores roughly `cell_count + total_triangles` vertices rather than
  `3 * total_triangles` — about 2.6x fewer for an all-hex globe — plus a 16-bit
  index buffer. It is still the single largest allocation at high levels; the
  vertex data (position + normal + color) dominates. If you need to go past the
  levels below, the next step is cross-cell corner sharing via a per-face color
  shader (removing per-vertex colors), then chunk-level frustum culling / LOD so
  off-screen chunks aren't uploaded at all.
- **Practical level ceilings** (desktop, rough): L7 (~164K cells) is comfortable;
  L8 (~655K) is fine; L9 (~2.6M) works but the mesh is well over a gigabyte; L10+
  needs the shader/culling rework above before the cell count itself bites.
  `PLANET_MAX_LEVEL` (in [planet.h](../../../include/services/world/planet.h)) is
  the cap; it is currently **8**. Raising it exposes the larger, memory-heavy
  sizes: L10 is ~10.5M cells, L11 ~42M cells (tens of GB — see the L11 math in the
  mesh-memory note above), so treat anything past L9 as needing the rework first.

## Gotchas

- **Cell indices change with the level.** `world_set_level()` respawns every tile
  entity and drops the current selection. Never persist a raw cell index across a
  resolution change.
- **Pentagons.** Twelve cells have degree 5; neighbor slot 5 is `-1`. Any loop
  over neighbors must respect `degree[cell]` (or check for `-1`), never assume 6.
- **Keep `sample()` pure.** Neighbor-dependent logic there breaks determinism and
  cannot see cells generated later in the loop. Use a post-pass.
- **Sync both stores.** After any pass that mutates the flat mirror, sync into the
  `TileComp`s (the ECS copy is the source of truth for systems) and recolor the
  mesh. `world_regenerate()` is the template to follow.
- **Terrain is derived, not authored.** If you want a tile to look different,
  change its raw fields or add a rule to `tile_classify()`; do not just poke
  `terrain` and expect it to survive the next regenerate.

## Source comment archive

Every file under `src/services/world/` and `include/services/world/` is now
comment-free by design: all explanatory comments that used to live inline have
been moved here instead, one subsection per file, in the order they appeared.
Nothing above is a substitute for this section and vice versa — the earlier
sections are the narrative walkthrough; this is the literal, symbol-by-symbol
reference the code comments used to carry. `components/tile.h` / `tile.c` are
outside `services/world/` and were left commented as-is.

### planet.h

- **File header.** Icosahedral hex globe (Goldberg polyhedron) geometry. A
  planet is a geodesic sphere: start from an icosahedron, subdivide each
  triangle `level` times (each level splits every edge, so frequency `F =
  2^level`), and normalize every vertex onto the unit sphere. Each *vertex* is
  one simulation cell. Cell count is fixed by the level: `F = 2^level,
  cell_count = 10F^2 + 2`. Every cell has exactly 6 neighbors EXCEPT the 12
  cells sitting on the original icosahedron corners, which have 5 (the
  pentagons). That is the only irregularity, at any resolution: there are no
  poles and no edges. The core here is pure C (math + malloc only); rendering
  lives in `planet_render.h`. Cells are stored as flat parallel arrays (SoA)
  so simulation passes stream linearly and neighbor traversal is a table read.
- **`PLANET_MAX_LEVEL`.** Highest subdivision level the planet may be built
  at. The cell count is `10*4^level + 2`, so each level up is ~4x the
  geometry, tiles, and mesh. Level 7 is ~164K cells (comfortable); level 11 is
  ~42M cells and needs tens of GB. Raise with care and see the memory notes in
  `planet_render.h`.
- **`PLANET_MAX_DEGREE`.** Maximum cell degree (6 for hexagons, 5 for the 12
  pentagons). Fixed-width neighbor and corner tables use this bound; the
  unused slot 5 on a pentagon is set to -1.
- **`PlanetV3`.** A 3D vector in unit-sphere space.
- **`Planet`.** Pure, immutable planet geometry: cell positions, adjacency,
  and dual polygon corners for rendering. Per-tile simulation state
  (elevation, temperature, humidity, derived terrain) lives in the ECS as
  `TileComp` (see `tile.h`) with a cell-indexed flat mirror in the World
  service (see `world.h`). Tiles reference this geometry by cell index;
  geometry is shared, never duplicated per tile.
  - `level`: subdivision level (0..`PLANET_MAX_LEVEL`).
  - `frequency`: `F = 2^level`.
  - `cell_count`: `10F^2 + 2`.
  - topology, built once, never mutated by the sim: `pos` (unit-sphere
    center per cell), `neighbors` (adjacent cells, -1 padded), `degree` (5 or
    6).
  - dual polygon corners, for rendering the hex/pentagon faces: `corner_pos`
    (triangle centroids on the sphere), `corner_count` (== triangle count of
    the mesh), `cell_corners` (indices into `corner_pos`, -1 padded).
- **`planet_create(level)`.** Build a planet at the given subdivision level.
  `level` is clamped to [0, `PLANET_MAX_LEVEL`]. Returns the newly allocated
  planet, or NULL on allocation failure.
- **`planet_rebuild(p, level)`.** Rebuild `p` in place at a new level (used by
  the resolution control). The existing planet's struct is reused; buffers
  are realloced. Returns `p` on success, or NULL (and `p` freed) on
  allocation failure.
- **`planet_destroy(p)`.** Free a planet and all its buffers.
- **`planet_cell_count_for_level(level)`.** Cell count for a level, without
  building anything (for UI labels). Returns `10F^2 + 2` for that level
  (level clamped internally).
- **`planet_cell_latitude(p, cell)`.** Latitude of a cell in [-1, 1] (its
  normalized Y). Handy for climate. Returns the cell's Y coordinate on the
  unit sphere.

### planet.c

- **File header.** Geodesic icosahedron construction and dual-cell topology.
  Builds the icosahedron, subdivides it `level` times sharing edge midpoints,
  then derives per-cell neighbors and dual polygon corners. Pure C: no
  raylib.
- **vec3 helpers.** Internal, so the core has no raylib dependency.
- **`VertBuf` / `FaceBuf`.** Growable vertex/face buffers used only during
  construction.
- **`MpCache` / `MpEntry`.** Edge-midpoint cache so adjacent triangles share
  subdivided vertices. Maps an undirected edge (min,max vertex index) to the
  shared midpoint vertex. Open-addressing hash, rebuilt each subdivision
  level. `key == 0` means empty (real keys are stored +1).
- **`mp_get`.** Returns the midpoint vertex index for edge (a,b), creating it
  if absent.
- **`build_icosahedron`.** Emits the 12 vertices and 20 faces of a unit
  icosahedron.
- **`subdivide`.** One subdivision pass: every triangle -> 4, sharing edge
  midpoints.
- **Topology assembly section.** From the geodesic (verts + triangle faces).
  Derives per-cell neighbors (adjacent verts) and dual corners (surrounding
  face centroids), both ordered CCW around the cell so the render fan is
  convex.
- **`order_ring`.** Sorts `items` by angle around `center` (a tangent-plane
  ordering). Builds a tangent basis at `center` and sorts `items` by angle
  around it; then an insertion sort (n <= 6) keeps `items[]` and `ids[]` in
  lockstep.
- **`build_topology`.** Fills a planet's pos/neighbors/degree/corners from
  the geodesic buffers.
  - `corner_pos`: one per triangle face (its centroid, on the sphere).
  - Gather, per vertex, its adjacent verts and surrounding faces. Temp
    scratch sized `[nv][MAX_DEGREE]`; degree counts filled as faces are
    scanned. Per face/vertex: record the surrounding face; record adjacent
    verts (deduplicated).
  - Commit to the planet, ordering each ring CCW: order neighbors, then order
    dual corners (surrounding face centroids).
- **Public API section marker.**
- **`clamp_level`.** Clamp a level to [0, `PLANET_MAX_LEVEL`].
- **`free_buffers`.** Free and NULL every geometry buffer (struct itself
  untouched).
- **`planet_build`.** Build a planet's geometry into `p` at `level`. Returns
  1 on success. With the 2:1 (`2^level`) subdivision the vertex count is
  exactly the cell count formula; assert-by-construction keeps regressions
  loud in tests.

### planet_render.h

- **File header.** Raylib rendering of a `Planet` as a flat-shaded, indexed,
  chunked mesh. Each cell face (hexagon or pentagon) is a triangle fan
  colored by its terrain. **Indexed:** a cell's fan shares its center +
  corner vertices (7 verts / 18 indices for a hexagon instead of 18
  standalone verts). Corners are shared within one cell only, never across
  cells: each cell's flat terrain color is a per-vertex attribute, so a
  shared boundary vertex could not carry two colors. **Chunked:** raylib's
  `Mesh.indices` is 16-bit, capping one mesh at 65,536 vertices. The cells
  are split into chunks of whole cells (each under the cap) and emitted as
  one Mesh per chunk in a multi-mesh Model. `DrawModel` draws all. Memory:
  the mesh is the dominant allocation at high levels. Indexing stores
  ~`cell_count + total_triangles` vertices instead of `3 * total_triangles`
  (about 2.6x fewer for an all-hex globe), plus a 16-bit index buffer. Going
  past ~level 9 needs cross-cell corner sharing via a per-face color shader,
  then chunk-level frustum culling / LOD. Terrain is passed in as a
  cell-indexed array (the World service's flat mirror of the ECS
  `TileComp.terrain` fields), so the renderer stays decoupled from how tiles
  are stored.
- **`planet_model_build(p, terrain)`.** Build a Model from the planet's dual
  faces (one Mesh per vertex chunk). `terrain` is a cell-indexed array of
  `TileTerrain` values (length `p->cell_count`); colors come from
  `TILE_TERRAIN_COLORS`. Returns a Model the caller owns (free with
  `planet_model_unload`).
- **`planet_model_update_colors(model, p, terrain)`.** Rebuild only the
  per-vertex colors in place (after reclassifying terrain without changing
  resolution). Walks the same chunk partition build produced and re-uploads
  each chunk's color buffer; bails if a mesh size no longer matches.
- **`planet_model_apply_colors(model, p, cell_colors)`.** Recolor by an
  arbitrary per-cell color array (length `p->cell_count`), for debug "map
  mode" views (rainfall, temperature, region, ...). Same in-place chunk
  recolor + VBO re-upload as `planet_model_update_colors`; bails on a size
  mismatch.
- **`planet_model_unload(model)`.** Unload the model and every chunk mesh,
  and zero the handle.

### planet_render.c

- **File header.** Indexed, chunked mesh builder for the planet globe. See
  `planet_render.h` for the full design rationale (indexed within a cell,
  chunked to respect raylib's 16-bit mesh indices).
- **`PLANET_CHUNK_MAX_VERTS`.** Max vertices per chunk mesh. Stays safely
  under raylib's 16-bit index ceiling and on a cell boundary. A cell adds at
  most `PLANET_MAX_DEGREE+1` (7) vertices, so this leaves room to finish any
  cell.
- **`chunk_end(p, start, out_verts, out_tris)`.** Advance from cell `start`
  over as many whole cells as fit under the cap. Always advances at least one
  cell (a single cell is well under the cap). Returns the end cell
  (exclusive). Inline: `cell_verts = degree + 1` is center + corners.
- **`build_chunk_mesh`.** Build and upload one Mesh for the cell range
  [start, end). Inline: `vi` is the vertex cursor (chunk-local), `ii` the
  index cursor. One vertex for the center, then one per corner — the cell
  normal (its position on the unit sphere) is shared by all, i.e. flat
  per-face shading. Fan indices: center, corner k, corner k+1 (all
  chunk-local, < 65520).
- **`planet_model_build`.** Identity transform, written by hand so this file
  stays raymath-free.
- **`planet_model_update_colors`.** Partition must match the one build
  produced; bail if the mesh moved. Re-upload just the color buffer (index 3
  in raylib's mesh VBO layout).
- **`planet_model_unload`.** `UnloadModel` frees every chunk mesh + material.

### world_gen.h

- **File header.** World generation interface and terrain classification. A
  generator turns a cell's geometry (its position on the unit sphere) into
  the three raw fields a tile carries: elevation, temperature, humidity.
  Terrain is then *derived* from those by `tile_classify()`. Generators are
  swappable behind a small vtable so the UI can offer several algorithms
  (continents, islands, latitude bands, ...) and switch between them at
  runtime.
- **`WorldGenParams`.** Tunables shared by all generators (exposed by the UI
  sliders).
  - `seed`: deterministic seed for all noise.
  - `sea_level`: elevation below this is ocean (default 0.0).
  - `mountain_level`: elevation above this trends to rock/snow (default
    0.55).
  - `noise_scale`: spatial frequency of the base noise (default 1.6).
  - `warmth`: global temperature bias, added to all (default 0.0).
  - climate pipeline (see `world_climate.c`): `rain_shadow` — orographic
    rain-shadow strength [0..1] (default 0.6); `moisture_reach` — advection
    reach inland, larger = wetter interiors (default 1.0).
  - hydrology (see `world_hydrology.c`), opt-in: `enable_hydrology` — run the
    rivers/lakes pass (default false; expensive); `river_density` — higher =
    more/finer rivers (lowers the min Strahler order drawn); default 0.5
    shows order >= 2. Lakes are NOT a tunable: a closed basin becomes a lake
    only if the rainfall its catchment collects can outrun evaporation off
    its own surface (see `world_hydrology.c`). No `lake_amount` knob — basins
    either hold water or they don't, same as real terrain.
  - tectonics (see `world_tectonics.c`): `plate_count` — plate seeds [2..64];
    default 10. More = more, smaller ranges/basins. Seed-dependent: a new
    seed draws a different plate layout, same count.
- **`world_gen_default_params(seed)`.** Default parameters for a fresh world.
  `seed` is stored in the returned params. Returns a populated
  `WorldGenParams` with sensible defaults.
- **`WorldGenSample`.** One generator's output for a single cell: `elevation`
  [-1, 1], `temperature` [0, 1], `humidity` [0, 1].
- **`WorldGenerator`.** A swappable world-generation algorithm. `sample` must
  be pure w.r.t. (params, pos) so regeneration is deterministic for a given
  seed. Neighbor-dependent work (rivers, erosion) belongs in a post-pass in
  the World service, not here.
  - `name`: display name, for the UI algorithm picker.
  - `sample(self, params, pos, cell)`: sample the three raw fields at one
    cell. `self` is the generator (for `state` access), `params` the active
    tunables, `pos` the cell's unit-sphere position, `cell` the cell index.
    Returns the elevation/temperature/humidity sample.
  - `state`: optional per-generator data (unused by the built-ins).
- **`world_gen_count()`.** Number of built-in generators (stable order).
- **`world_gen_get(index)`.** Fetch a built-in generator by index (clamped to
  a valid entry). Returns a pointer to the generator (never NULL).
- **`world_gen_name(index)`.** Display name of generator `index` (clamped).
- **`tile_classify(params, elevation, temperature, humidity)`.** Derive a
  terrain class from the raw fields (Whittaker-style). Kept separate from
  generation so the classification rule is shared across every algorithm:
  elevation gates water/land/mountain, then temperature x humidity picks the
  biome. Returns the derived `TileTerrain`.

### world_gen.c

- **File header.** Built-in world generators, the noise toolbox, and terrain
  classification. Self-contained seedable value noise (no stb dependency) so
  generation is portable and deterministic across platforms.
- **Noise section.** Seedable 3D value noise + fBm.
- **`hashf`.** Result in [0,1].
- **`fbm`.** Fractal Brownian motion in [-1, 1].
- **`ridged`.** Ridged noise (island chains / mountain ridges): sharp crests
  near +1. Result in [0,1], peaked at ridge lines.
- **`warp`.** Domain warp: perturb the sample point by low-frequency noise so
  coastlines meander instead of following the raw noise lattice.
- **Climate helpers section.** Shared so every algorithm speaks the same
  temperature/humidity units.
- **`warped_latitude`.** Latitude in [0,1]: 0 at the equator, 1 at a pole.
  Warped slightly so climate band edges wiggle rather than forming perfect
  circles — breaks up the perfect rings.
- **`climate_temperature`.** Temperature from latitude + elevation lapse rate
  + global warmth. Result [0,1]. `t = 1 - lat` (warm equator, cold poles),
  colder up high, plus the global thermostat (`warmth`).
- **`climate_humidity`.** Idealized humidity by latitude: wet ITCZ at the
  equator, dry subtropical deserts near lat 0.33, a wetter temperate belt
  near 0.6, drying to the poles. `eq` is the equatorial term, `mid` the
  temperate term.
- **Generators section.**
- **`gen_continents`.** Continents: a few big landmasses with meandering
  coasts. Domain-warped fBm for the shape; climate from latitude blended with
  local moisture noise so interiors dry out into deserts and coasts/temperate
  belts stay green. The second fBm term adds coastal detail. Humidity blends
  the idealized latitude humidity with noise, and dries out high interiors
  (rain shadow up high).
- **`gen_islands`.** Islands: an ocean world of archipelagos. A large-scale
  "uplift" mask decides WHERE island groups sit; blobby fBm (not ridged,
  which makes thin filaments) carves each group into a contiguous main island
  with scattered satellites and deep ocean between groups. Warm and wet ->
  tropical. `cluster`/`uplift`: regional archipelago mask — where cluster is
  high, the sea floor rises. `detail`: blobby island detail (contiguous
  shapes), on a warped domain for organic coasts. Land emerges inside
  clusters; detail fragments it into an archipelago. Temperature gets a
  "balmy" +0.1 bias.
- **`gen_bands`.** Latitude Bands: gentle terrain, climate dominated by
  latitude. The planet reads as clean horizontal zones (ice caps, tundra,
  temperate forest, subtropical desert, tropical belt) to make the
  classifier's rules legible. `e` is mild relief; humidity is pure banded
  climate.
- **`GENERATORS[]`.** Built-in generator registry, in the order the UI cycles
  them.
- **Terrain classification section (before `tile_classify`).** Elevation
  still hard-gates water/land/mountain (categorical), but lowland biomes are
  now chosen by continuous Gaussian scoring instead of an if/else ladder:
  each biome has an ideal +/- tolerance on temperature, humidity (moisture),
  and normalized land elevation; the score is a weighted product of per-axis
  bell curves and the highest score wins. This gives smooth transitions and
  is data-driven (edit the table, not the branches).
- **`BiomeProfile` / `BIOMES[]`.** Ideal climate per lowland biome.
  Tolerances set how picky each one is (smaller = rarer). Weights:
  temperature 0.45, humidity 0.45, elevation 0.10. Struct fields: terrain,
  ideal temperature + tolerance, ideal humidity + tolerance, ideal
  norm-elevation + tolerance (0=coast .. 1=mountain foot).
- **`gauss`.** Bell curve: 1 at the ideal, ~0.37 at +/-1 tolerance, ~0 by
  +/-2.
- **`tile_classify` body.**
  - Water, relative to the (adjustable) sea level — hard gates. No beach
    tile: coastal detail like that is local-map generation guided by this
    cell's fields, not a strategic-hex terrain type of its own.
  - High ground: bare rock, or snow-capped only when genuinely cold — hard
    gate. (A low threshold keeps cool-but-not-frozen equatorial peaks as
    rock.)
  - Lowland biomes: Gaussian scoring over temperature x humidity x
    elevation. Combined MULTIPLICATIVELY (weighted geometric-ish), not
    additively: a biome must match temperature AND humidity, so a cold biome
    whose temperature curve is ~0 can't win a warm tile on its (broad)
    humidity term alone. Elevation is a soft modifier. This is what keeps
    snow off warm lowlands.

### world.h

- **File header.** The World service: a planet's geometry, per-tile state,
  and rendered globe. Owns a planet's geometry, its per-tile simulation
  state, and the rendered globe. It wires the pieces together and is the
  single handle the app (`main.c`), the Lua bindings (`gramarye.world.*`),
  and the UI globe seam all talk to. Hybrid storage (chosen deliberately):
  ECS `TileComp` entities are the source of truth (rich, queryable, iterable
  by systems through the ECS dense mirror); a cell-indexed FLAT MIRROR
  (elevation/temperature/humidity/terrain arrays) shadows them, so
  neighbor-heavy passes and mesh building are pointer walks, not a UUID hash
  lookup per neighbor. Regeneration writes the flat mirror, then syncs it
  into the `TileComp`s.
- **`World`.** Opaque World handle.
- **UI custom-draw seam kinds.** A Lua `custom` node with one of these kinds
  routes to the matching draw fn (see `GramaryeUI_set_custom_draw`).
  `WORLD_GLOBE_KIND` -> `world_draw_in_rect` (the rotating planet).
  `WORLD_CONTROLS_KIND` -> `world_draw_controls_in_rect` (param sliders).
- **`world_create(ecs, tile_type, level)`.** Create the world: build the
  planet, spawn a tile entity per cell, and run an initial generation pass.
  `ecs` is the ECS the tile entities live in; `tile_type` the
  `ComponentTypeId` registered for `TileComp` (see `main.c`); `level` the
  initial subdivision level. Returns the new World, or NULL on failure.
- **`world_destroy(w)`.** Destroy the world, its tiles, mesh, and planet.
- **Resolution section.**
- **`world_set_level(w, level)`.** Rebuild geometry + tile entities at a new
  level, then regenerate. `level` is clamped to [0, `PLANET_MAX_LEVEL`].
  Expensive at high levels (respawns every tile entity).
- **`world_level(w)`.** Returns the current (built) subdivision level.
- **`world_cell_count(w)`.** Returns the current (built) cell count.
- **`world_set_pending_level(w, level)`.** Stage a target resolution without
  rebuilding (UI slider path). Takes effect on `world_apply()`.
- **`world_pending_level(w)`.** Returns the staged target level.
- **Generation section.**
- **`world_set_generator(w, index)`.** Stage a generator selection (apply via
  `world_apply`). Sets dirty.
- **`world_generator_index(w)`.** Returns the active generator index.
- **`world_generator_name(w)`.** Returns the active generator name.
- **`world_seed(w)`.** Returns the current seed.
- **`world_regenerate(w)`.** Re-run the active generator into the flat
  mirror, sync into the `TileComp`s, and refresh the mesh colors (at the
  current built level). Internal apply step; callers usually want
  `world_apply()`.
- **`world_apply(w)`.** Apply all staged edits (params, generator, seed,
  resolution) at once. Starts a STEPPED generation job: geometry rebuild
  (only if the staged resolution differs) then the pipeline, one stage per
  frame. This is what the Regenerate button calls, so the expensive work is
  spread across frames (a per-stage loading box shows progress) instead of
  freezing on click. Clears dirty when the job finishes.
- **`world_step_generation(w)`.** Advance a running generation job by one
  stage. Driven automatically from `world_draw_in_rect` each frame; call it
  yourself only to pump the job when the globe is not being drawn.
- **Generation-progress queries** (for the loading box / UI):
  `world_generating` (a job is in progress), `world_gen_stage_name` (current
  stage label, "" if idle), `world_gen_stage` (1-based stage index, 0 if
  idle), `world_gen_stage_count` (total stages + 1, sentinel at 0).
- **`world_reroll(w)`.** Stage a new random seed (apply to take effect).
  Sets dirty.
- **`world_set_seed(w, seed)`.** Stage an explicit seed, e.g. from a text box
  (apply to take effect). Sets dirty.
- **`world_dirty(w)`.** Returns true if there are staged edits not yet
  applied (for the button UI).
- **`world_params(w)`.** Returns mutable generation tunables (sea level,
  warmth, ...).
- **`WorldTileInfo`.** Snapshot of the currently selected tile, for a UI info
  box. `cell`: cell index, or -1 if none. `rainfall`: climate-pipeline
  rainfall [0, 1]. `flow`: accumulated water flow (0 when hydrology off).
  `river`: river width 0=none, 1..3. `region`: province id, -1 = ocean.
  `plate`: tectonic plate id, -1 if none. `fault`: `WORLD_FAULT_*`, 0 = none.
  `stress`: boundary stress magnitude, 0 away from faults. `terrain`:
  `TileTerrain`. `lat`, `lon`: degrees. `neighbors`: degree (5 for the 12
  pentagons, else 6).
- **Tile selection (click-to-inspect) section.** `world_selected` returns
  the selected cell, or -1. `world_selected_info` returns false if nothing
  selected. `world_clear_selection` clears the current selection.
- **Parameter setters section.** Each clamps to a sensible range and STAGES
  the value (sets dirty); the world only rebuilds on `world_apply()`. Values
  are raw `WorldGenParams` units (elevation-space for sea/mountain level, a
  bias for warmth). `world_set_sea_level` clamps to [-0.6, 0.6];
  `world_set_warmth` to [-0.5, 0.5]; `world_set_mountain_level` to [0.2,
  0.9]; `world_set_noise_scale` to [0.6, 4.0].
- **Climate + hydrology pipeline setters** (also staged; take effect on
  `world_apply`). `world_set_rain_shadow` clamps to [0, 1];
  `world_set_moisture_reach` to [0.2, 3.0]; `world_set_hydrology` toggles the
  rivers/lakes pass; `world_set_river_density` clamps to [0, 2]. No lake
  setter: lakes are derived from basin catchment rainfall vs. evaporation,
  not a tunable (see `world_hydrology.c`).
- **Tectonics setter** (also staged; take effect on `world_apply`). Rebuilds
  the plate layout too — unlike the wind field, plates are seed-dependent.
  `world_set_plate_count` clamps to [2, 64].
- **Debug "map mode" views.** Recolor the globe by a per-cell field. Purely
  a rendering choice — takes effect immediately (no regenerate), and
  persists across regenerations. TERRAIN is the normal biome coloring.
  `WORLD_VIEW_WIND`: smoothly-sampled wind direction (see `world_wind.c`).
  `WORLD_VIEW_ELEVATION`: raw continuous elevation, pre-classification.
  `WORLD_VIEW_MOISTURE`: climate's pre-smoothing advected moisture (vs.
  Rainfall). `WORLD_VIEW_PLATES`: plate id (random hue per plate) + fault
  lines overdrawn.
- **Rendering section.**
- **`world_draw_in_rect(w, x, y, w_px, h_px)`.** Draw the globe into a
  screen-space rect (used by the UI custom-draw seam). Also advances the
  orbit camera: drags inside the rect rotate, wheel zooms, otherwise it
  auto-rotates. Safe to call mid-2D-frame (flushes/restores GL).
- **`world_draw_controls_in_rect(w, x, y, w_px, h_px)`.** Draw the
  generation-parameter sliders into a screen-space rect and handle their
  drag. Dragging only STAGES values (via the parameter setters /
  `world_set_pending_level`); nothing rebuilds until `world_apply()` runs
  from the Regenerate button, so editing stays responsive even on very large
  worlds.

### world_pipeline.h

- **File header.** The world-generation post-pass pipeline. After the
  per-cell generator `sample()` writes raw elevation/temperature/base
  moisture into the flat mirror, these passes run in order over that mirror
  — the neighbor-heavy, multi-sweep work a pure per-cell `sample()` cannot
  do: tectonics -> plate motion biases elevation (mountains/rifts/faults);
  climate -> wind, moisture advection, orographic rain shadow, rainfall;
  (biomes) -> `tile_classify()` in `world_gen.c`, run as a pass by
  `world.c`; hydrology -> priority-flood, flow accumulation, rivers + lakes
  (opt-in); regions -> connected-component landmass ids. Each pass reads
  previously computed fields and writes new ones, keeping the whole thing a
  deterministic function of (seed, params, geometry). Modules operate on
  this `WorldFields` view rather than the private `struct World`, so they
  stay decoupled from how the service stores things.
- **`WORLD_NO_WATER`.** Sentinel for `WorldFields.water_level` on dry land
  (no water plane at this cell).
- **`WorldFields`.** `count` == `planet->cell_count`. Cell-indexed flat
  mirror (owned by the World service): `elevation` [-1, 1]; `temperature`
  [0, 1]; `humidity` [0, 1] (moisture axis; climate rewrites this);
  `rainfall` [0, 1] (climate output); `moisture` [0, 1] (climate scratch,
  base + advected); `downhill` (hydrology: steepest-lower neighbor, or -1);
  `flow` (hydrology: accumulated flow, >= 0); `river` (hydrology: river
  width 0=none, 1..3, drawn as a line); `water_level` (hydrology: water
  surface height — lake spill level, or `sea_level` under ocean/shallow;
  `WORLD_NO_WATER` on dry land. This is the number local-map generation
  should seed a water plane from — NOT `elevation`, which for a lake cell is
  the (lower) basin floor under the water); `region` (regions: province id,
  -1 = ocean); `terrain` (`TileTerrain`); tectonics (see
  `world_tectonics.c`): `plate` (plate id, nearest plate seed), `fault`
  (`WORLD_FAULT_*`, NONE unless near a boundary), `stress` (boundary stress
  magnitude, 0 away from faults).
- **`WORLD_FAULT_*` enum.** Plate boundary kind, written to
  `WorldFields.fault`. Also the source of the elevation bias tectonics
  applies: CONVERGENT uplifts, DIVERGENT rifts, TRANSFORM barely moves
  elevation but still registers high stress (shearing, not
  compression/extension) — kept for later systems (earthquakes, local
  generation hazards, ...) that care where the active faults are.
- **`world_climate_wind_at(pos)`.** Analytic prevailing wind at an arbitrary
  point on the sphere: trade winds (out of the east, toward the equator)
  below 30 deg latitude, westerlies (out of the west, toward the pole) 30-60
  deg, polar easterlies above — closed form, no mesh involved. This is the
  raw physical model; used to seed `WindField`'s control points below.
- **`world_climate_wind(p, cell)`.** Convenience: wind at a planet cell (=
  `world_climate_wind_at(p->pos[cell])`).
- **`WindField`.** Coarse wind-field control layer, decoupled from the tile
  mesh: a handful of points spread over the sphere (Fibonacci lattice, so
  coverage is deterministic and seed/level-independent), each holding the
  analytic wind at that point. Fine cells sample it by 3D proximity
  (inverse-distance-weighted over the nearest few control points, via a
  libcore `KDTree_T`) instead of snapping to a discrete hex neighbor — that
  removes the old advection "streak" artifact: the hex lattice's local
  neighbor directions are irregular from cell to cell, but two nearby query
  points are close in 3D space regardless, so they get nearly identical
  sampled results. Build once (e.g. in `world_create`) and reuse for every
  regenerate; it never needs rebuilding unless `count` changes. Fields:
  `pos` (control-point positions on the unit sphere), `wind` (wind vector at
  each control point), `index` (nearest-neighbor index over `pos`).
- **`world_wind_sample(wf, pos)`.** Smoothly-sampled wind at any point (not
  just a control point or a cell).
- **`world_climate_run(f, wind)`.** Climate layers: wind -> base moisture ->
  advection -> orography -> rainfall. Writes moisture, rainfall, and
  humidity. `wind` is the coarse field above; the caller builds it once (it
  does not depend on seed or level) and passes it to every climate run.
- **`PlateField`.** Coarse plate-tectonics control layer, same shape as
  `WindField`: a handful of plate seeds on the sphere (a jittered Fibonacci
  lattice, jitter keyed off `seed` so different seeds draw different plate
  layouts — unlike wind, this rebuilds every regenerate), a libcore
  `KDTree_T` over the seeds for fast nearest-plate lookup, and per-plate
  rigid motion parameterized the way real plates are: a rotation axis (Euler
  pole) + signed angular speed, so a plate's velocity at any point is a
  cheap `axis x point * speed` cross product, no simulation. Each plate is
  also flagged oceanic or continental (oceanic more likely, ~65%, to read
  Earth-like), which biases baseline elevation even away from any boundary —
  continents and ocean basins get a coherent shape instead of purely
  noise-driven blobs. Fields: `seed` (plate seed positions), `axis`
  (Euler-pole rotation axis, unit vector), `speed` (signed angular speed),
  `oceanic` (1 = oceanic plate, 0 = continental), `index` (nearest-plate
  lookup over `seed`).
- **`world_tectonics_run(f, pf)`.** Assigns each cell's plate (nearest
  seed), applies each plate's oceanic/continental baseline elevation bias,
  and near boundaries (where two cells' nearest plates differ) computes the
  relative plate velocity there to classify + bias further: CONVERGENT
  uplifts (mountain belts), DIVERGENT rifts (valleys), TRANSFORM barely
  moves elevation but still registers stress. Writes plate, fault, stress,
  and biases elevation — run this after Sample and before Climate (rain
  shadow needs the final heightfield).
- **`world_biomes_classify(f)`.** Classify every cell with the Gaussian
  scorer (`tile_classify`), then despeckle — replace isolated single-cell
  biomes with the dominant land biome around them, so biomes read as
  coherent zones. Runs after climate.
- **`world_hydrology_run(f)`.** Rivers and lakes. No-op unless
  `params->enable_hydrology`. Uses libcore Heap (priority-flood) and UFind
  (basins). Writes downhill, flow, and may overwrite land terrain with
  `TILE_TERRAIN_RIVER` / `TILE_TERRAIN_LAKE`.
- **`world_regions_run(f)`.** Connected-component landmass labeling over
  `planet->neighbors` (stops at ocean). Writes region (ocean cells get -1).
  Uses libcore UFind.

### world.c

- **File header.** World service implementation: planet, flat mirror, tiles,
  globe, UI.
- **`WORLD_WIND_FIELD_COUNT`.** Control-point count for the coarse wind
  field (`world_wind.c`). A few hundred is plenty to smoothly cover the
  sphere and stays cheap to sample; it does not depend on planet level or
  seed, so it is built once and reused forever.
- **`struct World`.** The World service instance (opaque to callers).
  - flat mirror of the ECS `TileComp`s (cell-indexed), for fast sim +
    rendering: `elevation`, `temperature`, `humidity`, `terrain`.
  - climate + hydrology + region pipeline fields (see `world_pipeline.h`):
    `rainfall`, `moisture`, `downhill`, `flow`, `river`, `water_level`
    (hydrology: water surface height, lake spill / `sea_level`; -1e30f on
    dry land), `region`, `plate` (tectonics: plate id, -1 = none), `fault`
    (tectonics: `WORLD_FAULT_*`), `stress` (tectonics: boundary stress
    magnitude).
  - `cells`: one tile entity per cell. `cell_cap`: allocated length of the
    arrays above.
  - `wind`: coarse control field for climate + the Wind view; built once.
    `plate_field`: coarse control field for tectonics; seed-dependent,
    rebuilt every apply.
  - Edits (sliders, algorithm, seed, resolution) are STAGED into the fields
    above + `pending_level`, and only take effect on `world_apply()` — so a
    full regenerate/rebuild happens once, on the Regenerate button, not on
    every slider tweak. `dirty` is set by any staging setter, cleared by
    apply.
  - `river_model`: cached river-ribbon mesh (rebuilt per generation).
    `view_mode`: `WORLD_VIEW_*`, which per-cell field the globe is colored
    by.
  - stepped generation job (one stage per frame; drives the loading box):
    `gen_stage` (`GEN_IDLE` when no job is running), `gen_target_level`
    (resolution the running job builds at), `gen_box_shown` (has the CURRENT
    stage's box been presented yet?).
  - orbit camera fields.
  - slider UI drag state (see `world_draw_controls_in_rect`): -1 = nothing
    held.
  - tile selection (click-to-inspect): `selected` = -1 when nothing is
    picked; `globe_pressed` — left button went down inside the globe rect;
    `press_pos` — where it went down (to tell a click from a drag).
- **`despawn_tiles(w)`.** Destroy every tile entity and its `TileComp`. Part
  of the hybrid store lifecycle: ECS entities are the source of truth.
- **`alloc_and_spawn(w)`.** (Re)allocate the flat mirror and resize the
  tile-entity population to exactly `n` entities. This used to be
  despawn-everything-then-spawn-everything (see `despawn_tiles`, still used
  for the one-time full teardown in `world_destroy`), which costs
  `O(old_count + new_count)` regardless of direction — so shrinking from a
  big level to a small one paid the FULL despawn cost of the big population,
  which is the "changing back down to L1 takes a while to unload" symptom:
  at L9 that is 2.6M individual `ECS_remove_component` + `Entity_destroy`
  calls before the (nearly instant) L1 spawn even starts. Resize
  incrementally instead: the overlapping prefix `[0, min(old,new))` keeps
  its EXISTING entities untouched — their `TileComp` is about to be fully
  overwritten by the very next `stage_sample()` pass regardless, so there is
  nothing in them worth preserving OR worth tearing down. Only the delta is
  touched: despawn the excess tail on a shrink, spawn a new tail on a grow.
  Cost is `O(|new - old|)`, not `O(old + new)`. Inline: shrinking despawns
  only the excess tail (a no-op when growing); growing spawns only the new
  tail (a no-op when shrinking) — the overlapping prefix reuses its existing
  entity, untouched here, fully overwritten by `stage_sample()` right after.
- **`world_fields(w)`.** Bundle the flat-mirror arrays into the view the
  pipeline passes operate on.
- **Debug map-mode recolor section** (see `WORLD_VIEW_*` in `world.h`).
- **`view_color`, `WORLD_VIEW_WIND` case.** Hue = direction (a simple x/z
  compass bearing, not a true great-circle heading — plenty to eyeball the
  circulation bands and gyres), full saturation/value on land, dimmed over
  ocean so coastlines still read.
- **`view_color`, `WORLD_VIEW_MOISTURE` case.** Pre-smoothing advected value
  (vs. Rainfall's post-smoothing field) — this is the one that shows the
  wind/advection pass's raw output.
- **`view_color`, `WORLD_VIEW_PLATES` case.** Fault lines overdrawn on
  plate-id coloring: a saturated color per boundary type, intensity by
  stress, so you can eyeball both the plate layout AND where the
  mountain/rift/shear bias came from.
- **`world_recolor(w)`.** Recolor the globe for the active view. Terrain
  view uses the cheap terrain path; field views build a per-cell color array
  and apply it. No mesh rebuild.
- **Pipeline stage helpers section** (shared by the sync and stepped paths).
- **`stage_sample(w)`.** Raw per-cell fields from the active generator
  (context-free). `humidity` is later overwritten by climate from rainfall.
- **`stage_tectonics(w)`.** (Re)build the plate layout and bias elevation by
  it. Unlike the wind field, plates are seed-dependent, so the layout
  rebuilds every apply rather than being built once (cheap: a few dozen
  points, same cost class as the wind field's own build).
- **`stage_biomes(w)`.** Classify from the finished climate fields (Gaussian
  scorer + despeckle).
- **`build_river_mesh` forward decl.** Defined below (near the globe
  renderer).
- **`stage_finalize(w)`.** Sync the flat mirror into the ECS `TileComp`s (in
  place), then (re)build/recolor. `world_recolor` applies the active
  map-mode view (terrain or a field); `build_river_mesh` caches the river
  network as one mesh (only when hydrology ran).
- **`world_regenerate(w)`.** Generation: write the flat mirror first (raw
  fields -> climate -> biomes -> hydrology -> regions), then sync into the
  `TileComp`s. Synchronous single-shot path (used at startup); the UI uses
  the stepped path below for the loading box.
- **Stepped generation section.** One stage per call, so the UI can show a
  loading box and huge worlds don't freeze the frame. `GEN_STAGE_NAMES[stage]`
  is the label. `gen_stage == GEN_IDLE` means no job is running.
- **`stage_geometry(w)`.** Rebuild geometry + tile entities only if the
  staged resolution changed. `alloc_and_spawn` incrementally resizes the
  tile-entity population (see above). Cell indices changed, so the old pick
  is dropped.
- **`world_step_generation(w)`.** Run the current stage, then advance. On
  the last stage, sync bookkeeping.
- **`world_reroll(w)`.** Stage a new random seed. Apply (`world_apply`) to
  see it — the reroll button stages then applies in one step.
- **`world_set_seed(w, seed)`.** Stage an explicit seed (e.g. typed into the
  seed text box). Apply to take effect.
- **Lifecycle section.**
- **`world_create`.** The wind field is seed/level-independent, so it is
  built once here and reused for every regenerate.
- **`world_apply(w)`.** Start applying all staged edits: kick off the
  stepped generation job (geometry rebuild only if the resolution changed,
  then the pipeline). The job advances one stage per frame via
  `world_step_generation()` — driven from `world_draw_in_rect` — so the UI
  can show a per-stage loading box and big worlds don't freeze. Restarts the
  job from the top.
- **`world_apply_sync(w)`.** Run a full generation job synchronously to
  completion (no loading box). Used by the direct-API immediate path and
  callers that need the world ready now.
- **`world_set_level(w, level)`.** Immediate resolution change (direct
  API). The UI stages via `world_set_pending_level` instead and applies on
  the button.
- **Parameter setters section.** Stage into params + mark dirty; the world
  only rebuilds on `world_apply()` (the Regenerate button), so dragging
  sliders is free.
- **`world_set_view(w, mode)`.** Map-mode view is a rendering choice:
  recolor immediately, no regenerate.
- **Rendering section.** Draw the globe into a screen-space rect,
  mid-2D-frame.
- **`begin_mode3d_rect`.** A confined version of `BeginMode3D`. Sets the GL
  viewport + projection to the rect with the rect's own aspect, so the globe
  isn't stretched to the full screen. Inline: flush pending 2D before
  switching; GL origin is bottom-left, hence the `screen_h - (y + fh)` flip.
- **`world_pick`.** Ray-pick the cell under (mx,my) within the globe rect.
  Builds the same ray the sub-rect projection uses, intersects the unit
  sphere, and returns the nearest cell to the hit point. O(N), but only runs
  on a click. Returns the picked cell index, or -1 on a miss. Inline: `c`
  term is against the unit sphere; early-out if the ray misses the globe.
- **`nudge_to_cam`.** Nudge a point a hair toward the camera along its view
  ray. Because it stays on the same ray, it projects to the same pixel (no
  lateral shift) but sits closer, so it wins the depth test. A radial lift
  instead parallax-shifts the outline off the tile when zoomed in or near
  the limb — which is the bug this avoids.
- **`draw_selection_highlight`.** Outline + translucent fill over the
  selected cell, drawn exactly on the tile (no radial lift) and nudged
  toward the camera so it sits on top without z-fighting or offset. Inline:
  translucent fill triangle, then a bright outline line.
- **`draw_loading_box`.** Per-stage loading box: a small checklist centered
  in the globe rect, drawn while a stepped generation job is running.
  `rows` covers stages 1..N.
- **`build_river_mesh`.** Build the river network into ONE cached mesh
  (rebuilt only when the world regenerates), instead of thousands of
  immediate-mode primitives every frame (which dragged badly at L8). Every
  river cell contributes a flat ribbon from its own hex center to its
  downhill neighbor's center, so the drainage tree forms one continuous
  line down to a water tile. The ribbon is subdivided along the sphere arc
  (each vertex re-projected onto the sphere) so it hugs the surface, and its
  width scales with CELL SIZE (~1/frequency) so a river stays ~1-2 cells
  wide at any resolution — and with Strahler order, so a "larger river" is a
  thicker line. Inline: `a`/`b` are unit-sphere positions of the cell and
  its downhill neighbor.
- **`world_draw_in_rect`.** Orbit input, only when the pointer is over the
  globe rect. On release: if the pointer barely moved since press, treat it
  as a click and pick a tile (a real drag rotates instead); a miss (empty
  space) clears it. The cached river mesh has culling disabled so ribbons
  show from both sides. Stepped generation: draw the loading box for the
  current stage, THEN run that stage. Drawing first only queues the pixels —
  they aren't actually presented until `EndDrawing()` back in `main.c`'s
  loop, which happens AFTER this whole call returns. A heavy stage
  (`GEN_GEOMETRY` at a big resolution jump respawns the entire ECS tile
  population) run in that same frame would therefore stall before its own
  box ever reached the screen, leaving the previous frame frozen with no
  explanation. Give each stage one frame to announce itself — draw the box
  and do nothing else — so it is guaranteed to be presented before that
  stage's (possibly expensive) work runs.
- **Generation-parameter sliders section** (before `WORLD_CTL_COUNT`). Drawn
  into a UI rect (custom seam kind `WORLD_CONTROLS_KIND`). Dragging a slider
  only moves the handle and updates the shown value; the world regenerates
  ONCE, on release — a full regenerate (and a resolution change even more
  so) is expensive on large worlds. Click anywhere on a track to jump +
  regenerate on release.
- **`FMT_*` enum.** Value display/rounding per slider: 0 = float "+0.00", 1
  = level "Ln", 2 = plain int "n".
- **Release handler in `world_draw_controls_in_rect`.** Stage on release —
  the world only changes when Regenerate is clicked.

### world_biomes.c

- **File header.** Biome assignment: Gaussian classification + despeckle.
  `tile_classify()` (in `world_gen.c`) scores each cell's
  temperature/humidity/elevation against a `BiomeProfile` table and takes
  the argmax — continuous, but per-cell, so cells right on a decision
  boundary flip individually and produce salt-and-pepper. This pass
  classifies, then removes isolated single-cell biomes by reassigning them
  to the dominant land biome among their neighbors. Water and coast are
  elevation-gated and left untouched.
- **First classify loop.** Sea's water surface is just `sea_level`; set here
  so it's correct even with hydrology off. Lake cells get their (higher,
  basin-spill) water level from `world_hydrology_run` instead, which runs
  after this pass.
- **Despeckle section.** A cell with no same-biome neighbor is speckle ->
  replace it with the most common biome among its land neighbors. Two
  passes over a snapshot so results don't depend on iteration order. `land
  only` guards the elevation check.

### world_climate.c

- **File header.** Climate layers over the flat mirror: prevailing wind ->
  base moisture (ocean sourced) -> advection inland along wind -> orographic
  rain shadow -> rainfall. A trimmed, game-scale version of the RP2
  pipeline: enough for the visible wins (wet coasts, dry interiors, leeward
  rain shadows) without the full 11-layer simulation. Every step is a
  deterministic function of (seed, params, geometry). Wind itself lives in
  `world_wind.c`: a coarse, mesh-independent control field, sampled smoothly
  (see `world_wind_sample`). This module used to snap each cell's wind to
  whichever single hex neighbor was most opposite it, which aliased into
  visible streaks (the hex lattice's local neighbor directions are irregular
  from cell to cell, so nearby cells could legally snap to different
  neighbors under near-identical wind). It now blends across ALL of a
  cell's neighbors, weighted by alignment with the smoothly-sampled wind,
  which washes that irregularity out instead of amplifying it.
- **vec3 helpers section.** Self-contained, no raylib.
- **`hashf`.** Deterministic per-cell hash in [0,1], for rainfall jitter.
- **Step 1 (base moisture).** Oceans are full sources; land starts dry
  (slightly wetter near the equator) and only gets wet through advection
  below.
- **Weight precompute block.** Precompute, once, each land cell's upwind
  PULL WEIGHTS over its neighbors from the smoothly-sampled wind field:
  `weight[i*DEG+k]` is how much of neighbor k's moisture flows into cell i
  per sweep, and `primary[i]` is the single most-upwind neighbor (for
  orography below, which wants one slope, not a blend). Weight is
  alignment-with-"the direction air arrives from", squared (concentrates the
  blend without collapsing to a single winner) and normalized to sum to 1.
  Wind is static for the whole run, so this is paid once, not per sweep.
- **Step 2 (advection).** Sweep moisture downwind from ocean sources with
  per-hop decay. More reach = more sweeps = wetter interiors. Sweep count is
  bounded so cost stays `O(sweeps * cells)` regardless of resolution. Plain
  decay alone caps how far inland any biome can get wet enough to be
  rainforest — it fringes the coast and never reaches an interior, unlike
  the real tropics (Amazon/Congo basins), which stay wet deep inland because
  hot, already-moist land re-emits much of its moisture back into the air
  (evapotranspiration — "flying rivers") instead of just losing it to decay.
  Model that as a small recycling term: hot land adds back a fraction of its
  own current moisture each sweep. It is self-limiting — there is nothing to
  recycle where moisture is already near zero — so deserts are unaffected;
  only a tropical corridor that's already picked up coastal moisture can
  ride it further inland. Inline: oceans stay saturated sources; `warmth`
  term gates recycling to tropical cells only.
- **Step 3 (orographic rain shadow).** Rising air (windward, cell higher
  than its primary upwind neighbor) wrings out extra rain; descending air
  (leeward) dries out.
- **Step 4 (rainfall).** Moisture scaled by temperature capacity (warm air
  holds more) plus a touch of noise. Oceans read as fully wet. `jitter` is
  kept small: coherence over speckle.
- **Step 5 (smoothing).** Smooth the rainfall field so biomes form coherent
  zones instead of salt-and-pepper. A couple of neighbor-average (box blur)
  passes over land; oceans are held fixed as wet boundaries. Then humidity
  mirrors rainfall as the biome moisture axis.

### world_gen.c (noise/climate helpers) — see world_gen.c above

### world_hydrology.c

- **File header.** Hydrology: rivers and lakes (opt-in; skipped unless
  `params->enable_hydrology`). Pipeline: (1) priority-flood (Barnes) from
  ocean outlets -> a "filled" surface with no internal sinks, plus a
  drainage receiver per cell (libcore Heap); (2) flow accumulation down the
  receiver forest, seeded by rainfall; (3) cells above a flow threshold
  become rivers; (4) cells the fill raised above their terrain are lake
  candidates; contiguous groups (libcore UFind) become lakes where the
  basin's catchment rainfall (flow accumulation at its pour point) outruns
  evaporation off its area — a water balance, not a tunable abundance.
  Reuses the flat mirror's `downhill` (receiver) and `flow` fields.
- **`hcell_cmp`.** Min-heap on key: lowest fill elevation pops first
  (priority-flood order).
- **`touches_sea`.** True if any neighbor of cell i is sea (ocean/shallow).
  Used to keep a land buffer so lakes never touch the ocean.
- **Seed outlets block.** Every ocean cell drains to the sea. If the world
  has no ocean, seed the single lowest cell so flooding still terminates.
- **Priority-flood block.** Priority-flood+epsilon: pop the lowest cell,
  flood into unvisited neighbors, raising their fill to at least the water
  level. The tiny epsilon on flats makes the filled surface strictly descend
  to the outlet, so steepest descent below has a unique, acyclic direction
  (no flat plateaus to fan across).
- **Drainage direction block.** Drainage direction = STEEPEST DESCENT on
  the filled surface (lowest-fill neighbor), not the flood-discovery
  receiver. Steepest descent follows the valleys and concentrates
  tributaries into trunks; the flood receiver instead fans down slopes as
  parallel lanes (the "parallel rivers" artifact). The epsilon fill
  guarantees every non-outlet land cell has a strictly lower neighbor, so
  this is well-defined and cycle-free.
- **Flow accumulation block.** Local runoff = rainfall on land, then push
  each cell's flow into its receiver, processing high fill -> low (reverse
  discovery).
- **Rivers block.** Rivers via STRAHLER STREAM ORDER over the drainage
  tree. Each headwater is order 1; where two streams of equal order merge,
  the order rises. This gives a real hierarchy — fat trunks near the sea,
  ever-finer tributaries upstream. Draw only order >= `min_order`, hiding
  the order-1 rivulets that otherwise clutter as dense parallel lines, and
  set line width by order so trunks are clearly larger than the feeders
  branching into them. `river_density` lowers `min_order` -> more, finer
  rivers. Rivers are drawn as LINES following `downhill`; the tile keeps its
  biome. `ord`/`mx1`/`mx2` arrays: Strahler order, top incoming order,
  second incoming order. Process headwaters -> mouth (reverse of the
  ascending-fill pop order), so each cell's upstream contributors are
  counted before it; feed our order downstream when a downhill neighbor is
  land. Which stream orders to draw scales with RESOLUTION: coarse worlds
  have tiny networks (low max order), so more of them are shown (connected
  rivers instead of scattered high-order fragments); fine worlds have deep
  networks, so the small orders are hidden (a clean trunk+tributary
  hierarchy instead of a dense mat). `river_density` shifts it: higher ->
  more/finer.
- **Lakes block.** A filled basin becomes standing water (its own LAKE
  terrain) only if it can physically hold water — rainfall collected across
  its catchment has to outrun evaporation off the lake's own surface. This
  is a water balance, not a tunable: a basin sitting under a wet catchment
  fills; a basin in a dry rain-shadow stays a dry pan, regardless of how
  deep it is. No abundance slider — basins either hold water or they don't,
  same as on a real planet. A small depth threshold (`lake_eps`) finds basin
  candidates at all (filled surface raised above bare terrain); a fixed
  minimum group size (`lake_min`) drops single-cell puddles. Group
  contiguous candidates with UFind.
- **Water balance block.** Inflow is the accumulated rainfall reaching the
  basin's lowest point (its pour point) — `flow` already sums rainfall over
  the whole upstream catchment, so the max flow within a basin's cells IS
  its total catchment input. Evaporative loss scales with the basin's own
  surface area (more cells to evaporate off = more loss to outrun). A basin
  becomes a lake only where inflow clears that loss. Keep a land buffer from
  the sea: a lake must never sit against ocean.
- **Shore growth block.** Grow lakes onto their shallow shores so they read
  as bodies, not dots. `surf` tracks each lake's water surface (its spill
  level) and is inherited by grown cells, so growth is bounded to shores
  within `lake_grow` of that surface — it can't creep up an open slope, and
  never onto a coast cell. `keep off the coast` guards growth away from the
  coastline; growth inherits the lake surface, not the grown cell's own
  elevation.
- **Water-level persist block.** Persist the water surface — NOT
  `elevation`, which for a lake cell is the (lower) basin floor under the
  water — so local-map generation can later seed a water plane that's
  consistent with dry neighbors instead of reading the basin floor and
  thinking the water sits below grade.
- **River cleanup.** A cell that became a lake is no longer a river line
  (the river feeds it).

### world_regions.c

- **File header.** Regions: province labeling by terrain-weighted
  multi-source growth. Plain connected-components make one mega-region per
  continent; plain hop-count BFS makes Voronoi blobs whose borders cut
  straight across the land. Neither looks natural. Instead we grow seeds
  with a cost-weighted flood (multi-source Dijkstra, libcore Heap):
  entering a mountain or a river is expensive, so where two provinces meet
  they settle their border onto those barriers — the way real regions
  follow ridgelines, rivers, and coasts. A little per-cell cost noise keeps
  the borders from being geometrically clean. Seeds start in lowlands, so
  province cores sit in habitable plains and mountains end up on the edges.
  Runs after hydrology, so river tiles are known. Ocean cells get region -1;
  islands with no seed become their own region. Deterministic in (seed,
  geometry).
- **`enter_cost`.** Cost to grow into cell `nb`: cheap on plains, dear on
  mountains and rivers, with organic noise so borders wiggle. `mtn` ramps
  up approaching peaks; bigger rivers make harder borders.
- **Habitable band.** `low_thresh` defines the "habitable" band seeds are
  scattered into; an all-mountain world falls back to treating every land
  cell as lowland.
- **Seed scatter block.** Seeds scattered across lowland only, so province
  cores sit in plains.
- **Dijkstra block.** Multi-source Dijkstra over the land graph; stale heap
  entries (`cur.cost > dist[c]`) are skipped.
- **Island fallback block.** Land unreached by any seed (isolated islands)
  -> its own region via BFS.

### world_tectonics.c

- **File header.** Tectonics: a coarse plate layer biases elevation so
  mountain ranges, rift valleys, and continent/ocean-basin shapes read as
  coherent structures instead of purely noise-driven blobs — and, as a side
  effect of computing that, produces per-cell fault data (boundary type +
  stress) for later systems (earthquakes, hot springs, resource placement,
  local-generation hazards, ...) to consume without having to re-derive it.
  Same architecture as `world_wind.c`: a handful of control points (here,
  plate seeds) on a libcore `KDTree_T`, sampled by 3D proximity rather than
  encoded per tile. Two differences from the wind field: plates carry RIGID
  MOTION (an Euler-pole axis + angular speed), not a static direction, so
  "velocity at a point" is a cross product; the layout is SEED-DEPENDENT (a
  new world seed draws different plates), so it rebuilds every regenerate
  instead of being built once.
- **vec3 helpers.** Self-contained; see `world_climate.c`.
- **`hashf`.** Deterministic hash in [0,1): same cell/salt/seed always draws
  the same value, so the whole plate layout is a pure function of `seed`.
  `salt` picks out an independent draw per quantity (position jitter, axis,
  speed, oceanic flag, ...) for the same plate index.
- **`tangent_basis`.** Any orthonormal tangent basis at a point on the unit
  sphere.
- **`world_plate_field_build`.** `golden_angle = pi * (3 - sqrt(5))`.
  `avg_spacing`/`jitter_radius`: rough angular spacing between `count`
  evenly-spread points on the unit sphere; used to scale both the seed
  jitter and (in `world_tectonics_run`) the boundary falloff width, so both
  stay proportioned to plate size. Base position: Fibonacci lattice, same
  formula as the wind field, for even coverage — then jitter it so
  different seeds draw different plate layouts (unlike wind, which is not
  seed-dependent). Euler-pole axis: a uniformly random unit vector
  (independent of the plate's position — a plate's rotation axis has
  nothing to do with where its seed happens to sit). Signed angular speed
  in [-1, 1] — sign is spin direction, not speed vs. its neighbors (that
  only matters relative to another plate, computed per-cell in
  `world_tectonics_run`). Oceanic plates more likely (~65%), to read
  Earth-like (~70% ocean coverage) without hard-coding continent placement.
- **`plate_velocity`.** Rigid-plate velocity at `pos`: `axis x pos *
  speed`. A general cross product is always perpendicular to both operands,
  so this is automatically tangent to the sphere at `pos` — no extra
  projection needed.
- **`TECT_BASELINE_K`.** Baseline blend width: nearest-1 (a hard Voronoi
  cell) makes the ocean/continent baseline JUMP the instant a cell's
  nearest plate changes — a visible straight-edged polygon cut across the
  noise. Blend it across the nearest few plates instead (same
  inverse-distance-weighted idea as `world_wind_sample`), so it fades into
  the noise like everything else.
- **`world_tectonics_run`.** Boundary falloff: a gaussian in "gap" (the
  difference between distance to the nearest and second-nearest plate seed
  — 0 exactly on the Voronoi boundary, growing into either plate's
  interior). Width scales with plate spacing, so fewer/bigger plates get
  proportionally wider mountain belts, not thinner ones. The 0.05 factor
  was picked by measuring the gap distribution (mean ~0.28x avg_spacing,
  max ~0.9x) and tuning until boundaries came out a clear minority of cells
  (~30% at the default plate count) rather than blanketing the whole plate
  — real mountain ranges are belts, not the entire continent. Constants:
  `uplift` (max convergent elevation bias), `rift` (max divergent elevation
  bias), `ocean_bias`/`land_bias` (baseline, even away from any boundary).
  Boundary normal: tangent-plane-projected direction from our plate's seed
  toward the neighbor's — "which way is across the boundary" at this
  point. `normal_comp > 0`: closing the gap (converging).

### world_wind.c

- **File header.** Wind field: the analytic circulation model, plus a
  coarse, mesh-independent control layer that samples it smoothly. See
  `world_pipeline.h` for why this layer exists (it replaces snapping wind
  direction to a discrete hex neighbor, which was the source of the old
  advection "streak" artifact).
- **vec3 helpers.** Self-contained; every pipeline module keeps its own tiny
  set rather than sharing a header (see `world_climate.c`).
- **`world_climate_wind_at`.** Tangent basis: east (+longitude), north
  (toward +Y pole). At the poles, pick any tangent. `y == sin(latitude)`.
  Trade winds: out of the east, toward the equator. Westerlies: out of the
  west, toward the pole. Polar easterlies: out of the east, toward the
  equator.
- **`WIND_SAMPLE_K`.** Nearest control points blended per sample: enough to
  smooth across the lattice, small enough to stay cheap at 655K+ query
  cells.
- **`world_wind_field_build`.** Fibonacci sphere lattice: deterministic and
  seed/level-independent, near-uniform coverage with no discrete "grid" for
  a sampled direction to alias against (unlike the planet's own hex mesh).
  `golden_angle = pi * (3 - sqrt(5))`. `PlanetV3` is 3 tightly-packed
  floats, so it can go straight to the kd-tree as a flat (count*3)
  row-major float array — no repacking.
- **`world_wind_sample`.** Inverse-distance-weighted blend of the k nearest
  control points' wind vectors. Smooth in 3D space: two nearby query points
  share almost the same nearest set and almost the same weights, so the
  result varies continuously regardless of the fine mesh's local topology.

### world_lua.h

- **`world_lua_register(host, world)`.** Installs `gramarye.world.*` into
  the ScriptHost's Lua state, bound to `world`:
  ```
  gramarye.world.regenerate()        -- re-run the active generator
  gramarye.world.reroll()            -- new random seed, then regenerate
  gramarye.world.next_algorithm()    -- cycle generator -> returns its name
  gramarye.world.set_level(n)        -- set subdivision level (rebuilds tiles)
  gramarye.world.level_up() / level_down()
  gramarye.world.level()   -> int
  gramarye.world.cells()   -> int
  gramarye.world.algorithm() -> string
  gramarye.world.seed()    -> int
  ```
  `world` must outlive the Lua state.

### world_lua.c

- **`WORLD_REGISTRY_KEY`.** The bound World is stashed in the Lua registry
  (same pattern as `entities_lua`).
- **`l_world_regenerate`.** The Regenerate button: apply all staged edits
  (params, algorithm, seed, resolution). This is the only thing that
  rebuilds the world.
- **`l_world_reroll`.** Reroll STAGES a new seed; Regenerate/Apply is the
  only thing that builds it (same staging contract as every other edit —
  see `l_world_regenerate`).
- **`l_world_next_algorithm`.** Cycle to the next generator (staged) and
  return its name. Apply via Regenerate.
- **`l_world_set_level`.** Resolution edits STAGE
  (`world_set_pending_level`); apply via Regenerate.
- **`l_world_level` / `l_world_cells`.** Level/cells report the STAGED
  target so the panel reflects what Regenerate will produce (the globe
  still shows the last-applied world).
- **`l_world_set_seed`.** Seed text box: stage an explicit seed. Apply via
  Regenerate, same as reroll.
- **Generation params section** (sea level, global temperature, ...).
- **Climate + hydrology pipeline section.**
- **Debug map-mode view section.** Recolors immediately, no regenerate.
- **Generation progress section.** Stepped job: one stage per frame.
- **`l_world_stage`.** `gramarye.world.stage()` -> current stage label ("" when
  idle), stage index, total.
- **Tile selection section.**
- **`l_world_selection`.** `gramarye.world.selection()` -> table `{cell,
  terrain, elevation, temperature, humidity, lat, lon, neighbors}`, or `nil`
  when nothing is selected.
