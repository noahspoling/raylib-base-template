# Local Chunk Service

`services/local_chunk` generates the fine-grained local guide raster for a
single planet hex: the structured-random terrain the player's view is built
from, at a target scale of roughly 15-50km per hex. It is a **separate
service from `services/world`**: it consumes World's read-only query surface
(`world_planet()`, `world_fields()`, `world_seed()`) as plain parameters and
has no `World*` in any of its own signatures. Nothing under `services/world`
knows this file exists; `local_chunk_lua.c` is the only file that depends on
both.

## Contents

- [Core idea: a field over the sphere, ownership by nearest center](#core-idea-a-field-over-the-sphere-ownership-by-nearest-center)
- [Determinism rules](#determinism-rules)
- [The pipeline](#the-pipeline)
- [File map](#file-map)
- [Debug views](#debug-views)
- [Known limitations](#known-limitations)
- [How to extend](#how-to-extend)

## Core idea: a field over the sphere, ownership by nearest center

Everything a chunk rasterizes is a **pure function of world position** (plus
the world seed and the coarse per-cell fields). The chunk is just a cached,
north-up square window onto that field, centered on its cell. Because two
chunks sampling the same physical point compute the same value, there is no
seam to stitch. This is the "seamless world" model (cf. the RimExodus
RimWorld mod): neighbors' terrain already exists beyond the border, and
crossing it is a change of which cache you read, not a remap.

Two global, symmetric functions on the sphere carry the structure:

- **Ownership** -- `local_chunk_owner_at()`: the cell whose center is nearest
  to the point after a small noise warp (`local_chunk_border_warp()`). That's
  a domain-warped spherical Voronoi diagram, i.e. the planet's own hex/pent
  cells with organic instead of straight borders. The nearest search is a
  greedy walk over `planet->neighbors`, so the answer is the true global
  nearest and **does not depend on which cell asks** -- the `hint` argument
  is only a starting point. Pentagons need no special case.
- **Blending** -- `local_chunk_blend_at()`: barycentric weights over the
  geodesic triangle (three cell centers; `planet->neighbors` is ring-ordered
  so consecutive slots form the triangles) containing the warped point.
  Coarse per-cell values interpolated with these weights are continuous
  everywhere and exactly equal the coarse value at each cell center.

Each chunk's square (`local_chunk_extent()`) covers its cell's farthest
polygon corner **plus the maximum warp displacement**, so everything a cell
can own fits inside its own raster. The rest of the square is an apron of
neighbor-owned terrain (`owner[] != LOCAL_CHUNK_OWNER_SELF`) -- it's correct
data, just not authoritative. Anything that must have exactly one home
(player edits, placed structures, points of interest) belongs to the owner.

An earlier version clipped each square to the hex's inscribed polygon and
dimmed the corners, leaving dead area nobody owned; a later version kept the
square as "the" hex area and invented a N/S half-tile brick stagger with a
per-edge reciprocity check (~10% of edges disagreed). Both are gone. Don't
reintroduce a shape that isn't a tessellation, or a per-edge rule that two
sides can compute differently.

## Determinism rules

The one property every stage must hold, because a chunk can be generated
starting from any of its neighbors:

- **Sampled data**: pure function of `(world_seed, world position)` and the
  coarse `WorldFields`. Elevation, climate, ownership, and terrain are all
  this kind.
- **Border-shared data** (river crossing points, edge seeds): pure function
  of the **unordered** cell pair -- `edge_midpoint()` is the midpoint of the
  two corners common to both cells.
- **Per-cell layout data** (river paths, future POIs): pure function of the
  owning cell, computed in that cell's own north frame. Any chunk can
  recompute a neighbor's layout without generating the neighbor -- that's how
  `stage_rivers` draws neighbors' rivers into the apron.

A quick way to break this without noticing: a per-chunk parameter (tangent
frame, extent, chunk seed) leaking into a sampled value. Rasterizing a
neighbor's layout must use the *neighbor's* frame and extent, as
`route_cell()` does.

## The pipeline

`LocalChunkStage { name, run }` entries in `STAGES[]`, run in order by
`local_chunk_run()`. Adding a stage is one function + one array entry.

1. **Frame** -- north-up tangent frame (`local_chunk_north_frame()`) and
   `extent`. Grid row 0 is north.
2. **Fields** -- per grid point:
   - elevation = active `WorldGenerator.sample()` at the point + a
     **blended coarse residual** (`world.elevation[c] - sample(pos[c])` for
     the three blend cells -- i.e. whatever tectonics/hydrology added on top
     of the raw generator, smoothly interpolated) + a sub-cell `fbm` detail
     layer (`world_seed + 9001`). The residual makes the fine terrain agree
     with the coarse planet at cell centers; the detail gives coastlines bays
     and coves. **Detail frequency and amplitude scale with the cell
     spacing** (`kDetailCyclesPerCell`, `kDetailAmpPerSpacing`): with fixed
     constants, at L8 the "detail" was coarser than a cell and acted as a
     regional bias big enough to sink near-sea-level land cells entirely.
     Never add a CONTINUOUS layer whose wavelength exceeds a cell unless the
     coarse elevation already includes it, and never a per-cell-seeded one.
   - temperature / humidity = blended coarse values + small `fbm` detail.
   - `lake` = blended weight of the blend cells that hydrology marked
     `TILE_TERRAIN_LAKE`, plus `fbm` edge noise. Lakes come from basin
     filling, not from elevation vs. sea level (they sit above sea level, and
     "grown" lake cells can sit above their own surface), so they can't be
     recovered from the fine elevation -- they're blended like climate.
3. **Ownership** -- `owner[]`: 0 = this cell, `1 + k` = neighbor slot `k`,
   255 = a second-ring cell (only in the far apron corners).
4. **Rivers** -- this cell's and every neighbor's rivers, each routed in its
   owning cell's frame over a 32x32 sample of the local topography
   (`route_build()`, same elevation function as `stage_fields`). Each stream
   is a least-cost path (`route_search()`) from a shared edge crossing point
   (or a high source point) toward the outflow crossing point: it prefers low
   ground, pays heavily to climb, avoids ground other cells own, and gets a
   noise term so it doesn't hug the obvious valley. It stops at the first
   local water it reaches, so rivers end on the actual coastline rather than
   the hex edge. Streams are routed largest-flow first and existing channels
   are cheap to follow, so later inflows and 0-2 extra headwater streams
   merge into them. Only cells on the global river network (`river > 0`)
   route, and only from inflows that are themselves on it, so the local map
   shows exactly the globe's network, connected end to end. `river_mask` holds
   a level: 1 = minor headwater stream (faint), `1 + river width` for network
   rivers. The first inflow carries the trunk at this cell's width; later ones
   are tributaries at their own width. Level >= 4 is drawn 2px wide. Paths are jittered, Chaikin-smoothed and meandered
   (wider swings on low ground, tapered to keep endpoints pinned), then
   projected into this chunk's grid. Routes depend only on (world, cell), so
   they match across borders. Tuning is the `kRoute*` / `kMeander*`
   constants.
5. **Biome** -- below sea level stays ocean/shallows; else `lake > 0.5` is
   `TILE_TERRAIN_LAKE`; otherwise `tile_classify()`. River samples on any
   water are cleared, so rivers end at lake shores and coasts. Classification
   is `tile_classify()` per grid point on the fine elevation and
   blended climate. A mountain hex next to a rainforest hex gets wetter
   terrain toward that border because humidity is interpolated across it;
   the warp makes the transition organic rather than hex-shaped.

## File map

| File | Role |
|---|---|
| `local_chunk.h` / `local_chunk.c` | The service: ownership/blend functions, pipeline, group view, debug renders. No `World*` anywhere. |
| `local_chunk_lua.h` / `local_chunk_lua.c` | Composition glue + Lua bindings (`gramarye.local_chunk.*`). The only place that imports both `services/world` and `services/local_chunk`. |
| `../world_noise.h` | Shared hash/value-noise primitives (also used by `world_gen.c`). Don't fork a second copy. |

## Debug views

All debug-only, not the production renderer. Each group view is baked into
textures once per `LocalChunkView` at creation, so switching views is free:

- `LOCAL_CHUNK_VIEW_TERRAIN` -- `TILE_TERRAIN_COLORS`, shaded by elevation.
- `LOCAL_CHUNK_VIEW_TOPOGRAPHY` -- hypsometric tint, NW hillshade, contour
  lines and a coastline. Contours and shading are on fixed scales (`kTopo*`:
  elevation mapped to meters, 20m contours, every 5th darker) so they compare
  across previews. The tint is intentionally stretched over the view's land
  range for debugging, so a hex's color depends on which neighbors are open.

To add a view: add an enum value before `LOCAL_CHUNK_VIEW_COUNT`, its name to
`VIEW_NAMES`, and a `*_colors()` branch in `bake_group_view()`. The local map
popup builds its button row from `gramarye.local_chunk.view_count/view_name`,
so it picks the new view up with no Lua change.

- `local_chunk_draw_in_rect()` -- one chunk's raw raster; the apron
  (non-owned samples) is dimmed.
- `local_chunk_draw_connected_in_rect()` -- **the default UI view**
  (`LOCAL_CHUNK_DEBUG_KIND`). North-up view of the center cell and its ring;
  each pixel reads from its owner's raster, with ownership borders drawn
  faintly. If borders were wrong you'd see discontinuities here.
- `local_chunk_draw_group_in_rect()` -- the same composite with the center
  cell tinted instead of border lines (`LOCAL_CHUNK_GROUP_DEBUG_KIND`).

The composite's pixel -> (tile, raster index) map is resolved once in
`local_chunk_create_group()` (`view_tile` / `view_index`), so drawing is pure
lookup. While building it, each pixel's owner is re-queried with the owner as
hint; any disagreement (or an owned point falling outside its owner's
raster) increments `owner_mismatches`, which the Lua glue logs. It should
always be 0.

## Known limitations

- **Apron overhead.** The square must contain the warped cell, so only ~25%
  of each raster is owned. Shrinking `warp_amp()` raises that (the extent
  shrinks with it automatically) at the cost of straighter borders.
- **Cache key covers identity, not parameters.** `local_chunk_cache_key()`
  packs `(world_seed, level, cell)` exactly into 64 bits (no hashing, so no
  collisions; the same cell index at a different level is a different hex).
  It does not cover generator index or slider params -- flush any cache on
  regenerate.
- **Scale is fixed per hex, not per planet.** `extent` is in unit-sphere
  tangent units; `planet_radius_m()` converts it, with hexes ~30km across at
  every level (L8 = Earth radius, each level down halves the radius).
  `local_chunk_res_for_cell()` sizes each raster so one pixel is
  `LOCAL_CHUNK_METERS_PER_PIXEL` (500m), giving ~100px rasters and a ~185px
  composite. Generation cost scales with 1/mpp^2. The composite is baked into textures once at group creation.
- **Second-ring cells' rivers** aren't drawn into the far apron corners.
- **Gameplay traversal** (moving an entity between chunk caches) isn't
  implemented; with this model it's just `local_chunk_owner_at()` on the
  entity's world position.

## How to extend

**Add a pipeline stage**: write `static void stage_foo(LocalChunkFields *f)`
in `local_chunk.c`, add `{ "Foo", stage_foo }` to `STAGES[]`.

**Add a coarse field to the fine raster**: blend it with
`local_chunk_blend_at()` in `stage_fields` exactly like temperature.

**Add linear features (roads, ridgelines, cliff bands)**: follow rivers --
crossing points from the unordered cell pair, path built in the owner's
frame, rasterized for self + neighbors.

**Add point features (settlements, POIs)**: place them from the owning
cell's `seed` only, so a neighbor can ask for the
list without generating the cell, and draw any whose footprint reaches into
the apron.

**Slope-aware classification** (e.g. steep coast -> cliff, not beach): compute
the gradient of `f->elevation` in a stage after Fields and feed it into the
Biome stage.

**Changing the noise/hash primitives**: edit `../world_noise.h` only.
