# template/include Overview

This document preserves the design notes, rationale, and usage details that used to live as
comments in the headers under `template/include/`. The headers themselves are now comment-free;
consult this file for the "why" behind the declarations.

## Components

### components/atlas.h

`Atlas`: a list of source `Rectangle`s placed on one texture. The atlas **borrows** its texture
through `TextureStore` — `Atlas_new(texture_id)` acquires it (returns NULL if it can't be made
resident) and `Atlas_free` releases it, so the texture stays on the GPU exactly as long as some
atlas (or other holder) is using it. Frames live in a libcore `Array_T`.

- `Atlas_add_frame` / `Atlas_add_grid` append frames and return the first new frame index.
- `Atlas_frame(atlas, i)` returns a zero rect when out of range; `Atlas_draw` draws one frame.
- Animation state is meant to hold a `const Atlas *` plus frame range/timing and resolve the
  current `src` via `Atlas_frame`, rather than owning a texture itself.

### components/chunk.h

Defines `CHUNK_SIZE` (64), `Tile` (`terrain`, `flags`, `height`) and `Chunk` (`cx`, `cy`, row-major
`tiles[CHUNK_SIZE * CHUNK_SIZE]`).
- `chunk_serialize()` / `chunk_deserialize()`: buffer round-trip in a fixed little-endian format
  (`"CHNK"` magic, `u16` version, `u16` chunk size, `i32` cx/cy, 4 bytes per tile), written field by
  field so struct padding never leaks into files. Deserialize rejects bad magic/version/size and
  leaves the output untouched on failure. Bump `CHUNK_SERIAL_VERSION` whenever the layout changes.
- `chunk_save()` / `chunk_load()`: file wrappers over raylib `SaveFileData` / `LoadFileData`.

### components/skinned_sprite.h

Defines `SkinnedSpriteComp`, the UV-remap ("skin") sprite component:

- `anim_texture` is the `TextureStore` id of a baked UV-animation atlas texture. Each texel's RG
  channels encode a pixel coordinate into `skin_texture` (see `tools/skin_baker`) instead of a
  literal color; alpha is the silhouette mask.
- `skin_texture` is the `TextureStore` id of the detailed art actually shown; 0 is invalid and
  won't draw.
- The skin_shader system decodes RG -> skin UV per-pixel at draw time, so swapping `skin_texture`
  recolors/reskins every baked animation for free — no new frames need to be authored.
- `src` is the current frame's rect within `anim_texture`.
- `w`, `h` are the world-space size, centered on the entity's transform.
- See `tools/skin_baker/README.md` for the author-time map/skin/frame workflow, and
  `docs/ARCHITECTURE.md` for how this fits into the sprite render path.

### components/sprite.h

POD components for the plain (non-skinned) sprite render path. Registered in `app.c`; their type
ids live in `GlobalState` (`transform_type` / `sprite_type`).

- `Transform2D`: `x, y` position, `rot` in degrees, `scale`.
- `SpriteComp`: `texture` is a `TextureStore` id (0 = untextured, draws a plain `w x h` tinted
  quad instead); `src` is the source rect in the texture (ignored when `texture == 0`); `w, h` is
  world size centered on the transform; `tint` is the draw color.

### components/tile.h

Defines the ECS component for one surface cell of the planet.

- Terrain is **not** authored directly — it is *derived* from the elevation/temperature/humidity
  fields by `tile_classify()` (see `services/world/world_gen.h`). The raw fields are stored (not
  just the classification) so systems like erosion, climate, and biomes can read and mutate them;
  `terrain` is just the cached classification the renderer colors by.
- `TileComp.cell` links back to immutable geometry in the `Planet` (position, neighbors) so this
  component stays small — geometry is shared, not duplicated per tile.
- `TileTerrain` enum values and their meaning: `DESERT` = hot+dry, `TUNDRA` = cold/sparse,
  `SAVANNA` = hot + moderate moisture (grass + scattered trees), `RAINFOREST` = hot+wet, `TAIGA` =
  cold moist boreal forest, `SWAMP` = warm very wet lowland, `RIVER` = hydrology high-flow
  watercourse, `LAKE` = hydrology enclosed inland water.
- `TileComp` field notes:
  - `cell`: index into Planet geometry (pos/neighbors/corners).
  - `elevation`: range `[-1, 1]`; below `sea_level` is underwater.
  - `temperature`: range `[0, 1]`, 0 = frozen, 1 = hot.
  - `humidity`: range `[0, 1]`, 0 = arid, 1 = wet — the moisture axis for biomes.
  - `rainfall`: range `[0, 1]`, climate-pipeline rainfall value that drives `humidity`.
  - `flow`: `>= 0`, accumulated water flow (0 when hydrology is off).
  - `water_level`: the water surface height — `sea_level` under ocean/shallow, a lake's (higher)
    spill level under `LAKE`, or `WORLD_NO_WATER` (see `world_pipeline.h`) on dry land. This is
    **not** the same as `elevation` — for a lake cell, `elevation` is the basin floor beneath the
    water. This is the value to seed a local water plane from.
  - `region`: province id, -1 = ocean/unassigned.
  - `plate`: tectonic plate id, -1 = none.
  - `fault`: one of `WORLD_FAULT_*` from `world_pipeline.h`; 0 = none.
  - `stress`: plate-boundary stress magnitude, 0 away from faults.
  - `river`: width 0 = none, 1..5 by Strahler order — this represents a line feature, not a
    terrain classification.
  - `terrain`: a `TileTerrain` value, derived from the fields above.
- `TILE_TERRAIN_COLORS[TILE_TERRAIN_COUNT]` is the render palette indexed by `TileTerrain`,
  defined in `components/tile.c`.
- `tile_terrain_name(int terrain)` gives a human-readable name for tooltips/legends; safe for any
  int, out-of-range values return `"?"`.

## Services

### services/chunk_manager.h

Currently empty (no declarations).

### services/clock.h

Defines `Clock` (`realtimeDelta`, `turnPending`, `tickToSimulate`) and `frame_tick()`.
`tickToSimulate` is in units of turns — a single action may take multiple ticks, or the game may
just use a simple turn system.

### services/stores/texture_store.h

A path-deduplicating texture registry so components can hold a plain `int32_t` id instead of a raylib
texture handle. Ids are `>= 1` and stable for the process lifetime; `0` means "no texture". All
bookkeeping memory goes through gramarye-libcore (`Atom` interns paths, `Table` maps path -> id,
`Array_T` holds the slots) — no direct libc allocation.

Registering and loading are separate: a registered entry is only its asset path until something
uses it.

- `TextureStore_register(path)`: records the path, returns its id. No GPU load.
- `TextureStore_acquire(id)`: refcount++, loads on the 0 -> 1 transition. Returns false (and takes
  no reference) if the load fails.
- `TextureStore_release(id)`: refcount--, unloads on 1 -> 0. The path and id stay registered.
- `TextureStore_load(path)`: register + acquire; returns 0 on failure (the old behavior).
- `TextureStore_get(id)`: zeroed `Texture2D` when the id is unknown or not resident.
  `TextureStore_get_ref` returns NULL in the same cases. `TextureStore_is_resident`,
  `TextureStore_path` for inspection.
- `TextureStore_shutdown()` unloads anything still resident regardless of refcount.

Lua: `gramarye.textures.load/register/acquire/release` map 1:1 onto the above.

### services/world_noise.h

Shared deterministic hash + value-noise primitives (`world_noise_hash3`, `world_noise_hashf`,
`world_noise_value`, `world_noise_fbm`, plus small smoothing/lerp/clamp helpers).

- Lives beside, not inside, any one service on purpose: both `services/world` (global generation)
  and `services/local_chunk` (per-hex fine detail) need the *identical* hash, so a value computed
  from either side of a hex border agrees bit-for-bit.
- Pulled out of `world_gen.c` unchanged — do not fork a second copy of this logic, the two copies
  will drift and break border agreement.

## Services/local_chunk

### services/local_chunk/local_chunk.h

Per-hex local guide raster. See `local_chunk/README.md` in that directory for the full design
(field-over-the-sphere model, ownership/blending, determinism rules, pipeline stages, known
limitations). This is a service separate from `services/world` — nothing in this file references
`World*`.

- `LOCAL_CHUNK_MAX_NEIGHBORS` = `PLANET_MAX_DEGREE` (6; only 5 at the 12 pentagon cells of the
  icosphere).
- `LocalChunkNeighbor`: coarse sample of one neighboring hex. `cell` is -1 if the slot is unused
  (degree < 6). `flows_into_me` / `i_flow_into_it` record the coarse downhill-flow relationship
  between this cell and the neighbor in each direction.
- `LocalChunkFields`: the full per-chunk generation state.
  - `res`: grid resolution (an `res x res` grid, row 0 = north).
  - `seed`: `hash(world_seed, level, cell)` (generation seed); `key`: exact
    `local_chunk_cache_key()` identity.
  - `elevation` / `temperature` / `humidity` / `terrain` / `river_mask` / `owner`: `res*res`
    row-major arrays filled in by the pipeline stages. `owner` is 0 for samples this cell owns,
    `1 + k` for neighbor slot `k`, 255 for anything farther. `river_mask` is 0 for no river,
    1 for a minor stream, `1 + river width` for a river on the global network.
  - `extent`: grid half-width in local tangent-plane units; `east` / `north`: the grid's frame.
  - `planet` and `world` are stored **by value, not by pointer** — `local_chunk_create()` is
    usually called with a `WorldFields` built on the caller's stack, and debug renderers read this
    stored copy again on later frames, so a pointer would dangle.
  - `generator_index` selects which `WorldGenerator` (see `world_generator_index()`) the fields
    stage samples continuously.
- `LocalChunkStage`: one named pipeline stage (`run` function). `local_chunk_stage_count()` /
  `local_chunk_stage_get()` enumerate the registered stages.
- `local_chunk_create()`: gathers self + neighbor coarse samples for `cell`. Does *not* run any
  stages; call `local_chunk_run()` afterward.
- `local_chunk_draw_in_rect()`: debug-only render of one chunk's raster, apron dimmed.
- `LocalChunkGroup`: a cell plus every one of its generated neighbors; `tiles[0]` is always the
  center. `view_*` is the precomputed composite (pixel -> owning tile + raster index);
  `owner_mismatches` should always be 0.
- `local_chunk_draw_connected_in_rect()` (default UI view, ownership borders drawn) /
  `local_chunk_draw_group_in_rect()` (center tinted): debug-only seamless composites, drawn in
  the given `LocalChunkView` (`TERRAIN`, `TOPOGRAPHY`; names from `local_chunk_view_name()`).
- `local_chunk_res_for_cell()`: raster size for a cell at `LOCAL_CHUNK_METERS_PER_PIXEL`.
- `local_chunk_cache_key(world_seed, level, cell)`: exact, collision-free 64-bit identity
  (`seed << 32 | level << 27 | cell`) for an eventual cache. The level matters: cell indices
  are only unique within one resolution. Parameters are not part of the key.
- Sphere-global, hint-independent queries (pure functions of world position):
  - `local_chunk_north_frame()`: north-up tangent frame for a cell center.
  - `local_chunk_extent()`: half-width a cell's raster needs to contain everything it can own.
  - `local_chunk_border_warp()`: the noise warp applied before ownership/blend lookups.
  - `local_chunk_owner_at()`: which cell owns a world point.
  - `local_chunk_blend_at()` / `LocalChunkBlend`: barycentric weights over the geodesic triangle
    containing a world point, for interpolating coarse per-cell fields.

### services/local_chunk/local_chunk_lua.h

Composition glue: the only file in the codebase that knows about both `services/world` and
`services/local_chunk` (see `local_chunk/README.md`).

- `LOCAL_CHUNK_DEBUG_KIND` (3) and `LOCAL_CHUNK_GROUP_DEBUG_KIND` (4) must match the `kind` values
  `planet.lua` passes for its debug custom nodes — connected-squares (default UI view) and
  hex-mosaic verification view, respectively.
- `local_chunk_lua_current()`: returns the most recently `generate()`-d group, or an empty
  (`count == 0`) one if none has been generated yet.
- `local_chunk_lua_view()`: the `LocalChunkView` picked via `gramarye.local_chunk.set_view(i)`
  (also `view()`, `view_count()`, `view_name(i)`, all 0-based).

## Services/tilemap

### services/tilemap/tilemap.h

Fixed-size, layered tile maps (the tilemap editor's data, and what a game
draws). `Tilemap` is opaque; up to `TILEMAP_MAX_SIDE` cells a side and
`TILEMAP_MAX_LAYERS` layers. A cell is a `TileRef`: 0 = empty, else
`tileset * TILEMAP_TILE_BASE + tile` (decimal so saved files read easily);
`tile_ref()` / `tile_ref_tileset()` / `tile_ref_tile()` pack and unpack it.

- Tilesets (`Tilemap_add_tileset`): an image (resolved through the mount
  stack) sliced into `tile_w x tile_h` tiles; an optional `skin` makes it an
  index map drawn through `skin.fs`. A missing image keeps its slot so saved
  data survives.
- Edits: `Tilemap_set`, `Tilemap_stamp` (a block from the tileset),
  `Tilemap_fill_rect`, `Tilemap_flood`. All set the dirty flag.
- Undo: `Tilemap_snapshot` starts a step before a stroke; the stroke's
  first write into each 32x32 block of a layer saves that block
  (copy-on-write). `Tilemap_undo` / `Tilemap_redo` swap the saved blocks
  with the map's. Capped at 64 entries and 64 MB.
- Files: `Tilemap_save` writes a Lua data file via a temp file + rename,
  each layer's cells as `rows` of runs (version 3); `Tilemap_load` runs one
  with an empty environment (`Packages_load_data`), reads `rows` or the
  older `cells`, and validates sizes and tileset references.
- Terrain layers (`TILEMAP_LAYER_TERRAIN`): a cell is a terrain slot (bits
  0-7), an optional flow direction (8-16) and collision flags (17-18:
  `TILEMAP_CELL_SOLID`, `TILEMAP_CELL_WATER`); terrains come from
  `Tilemap_add_terrain` (`lib/terrains.lua`).
- Levels: a map has `levels` (its z size, up to `TILEMAP_MAX_LEVELS` 32),
  `level_height` px each, and terrain is blocks: a cell on the level-k
  terrain layer is a block one level tall. A block with none on it is a
  floor, so a cell can have several (caves, bridges). One level of
  difference between floors is an incline (`slope_run` px), two or more a
  cliff, and a `stairs` terrain cuts a staircase into a cliff.
  `Tilemap_level_at` / `Tilemap_height_at` answer what's underfoot at the
  top, `Tilemap_floor_at` / `Tilemap_floor_height` at or below a level, and
  `Tilemap_covered` whether a floor is under a block.
- Layers: `Tilemap_add_layer` / `Tilemap_remove_layer` /
  `Tilemap_layer_empty`; an undo step covers one or several layers
  (`Tilemap_snapshot_layers`); removing a layer clears the history.
- Water: a `water` terrain is a see-through block; a floor is the bed under
  it. `Tilemap_water_at` gives the depth over a floor.
- Gameplay: `Tilemap_can_step` (the walking rule for a `TilemapWalker`, per
  floor, with headroom, wading and swimming, and what the step costs),
  `Tilemap_reach` / `Tilemap_path` (cheapest-first grid movement over
  floors), `Tilemap_arc` / `Tilemap_trace` / `Tilemap_raycast` (jumps,
  throws, falls, picking; the ray can skip blocks above a level, and
  `Tilemap_raycast_room` what an inside view of a room cuts), all in board
  space. `Tilemap_mesh_stats` reports what the geometry costs.
- Drawing: `Tilemap_draw` renders the board through a 3D camera
  (`TilemapView`: x, y, zoom, rotation, tilt, fov, plus grid, cursor, ghost,
  marker, sprites, arcs, marks, rings, supersample, and the inside views
  `room`, `section`, `see_through`, `cut_level`) into a render texture drawn
  into the UI rect; the levels above the ground are meshes (per level a top drawn
  by the ground shader, and walls by `wall.fs`) built per chunk when cells
  change.
  `Tilemap_pick` maps a screen point back to the board.
- Shaders load on first use; `Tilemap_reload_shaders` (F5) and, in dev
  builds, `Tilemap_watch_shaders` (a file changed on disk) reload them.
  `Tilemap_draw_tileset` draws a palette.

The Lua mirror of this API (`docs/SCRIPTING.md`, "Tilemaps") describes what
each call means for a game. What follows is what's specific to the C side.
Indices: cells `x, y` and layers are **0-based** here, tileset and terrain
slots **1-based**. Out-parameters marked "or NULL" may be NULL.

#### Constants and cell packing

| Name | Value / meaning |
|---|---|
| `TILEMAP_MAX_SIDE` | 1024 cells a side |
| `TILEMAP_MAX_LAYERS` | 40 layers: terrain levels and tile layers together |
| `TILEMAP_MAX_TILESETS` / `TILEMAP_MAX_TERRAINS` | 16 / 16. The terrain limit is the size of the ground shader's uniform arrays; change both together |
| `TILEMAP_MAX_LEVELS` | 32: a map's levels (its z size) are `0 .. levels - 1`; `TILEMAP_MAX_ELEVATION` is the top one |
| `TILEMAP_MAX_LEVEL_HEIGHT` | 32 world px per level, at most |
| `TILEMAP_TILE_BASE` | 10000: `TileRef = tileset * BASE + tile` on a tile layer (decimal so saved files read: 20037 = tileset 2, tile 37). A tileset can't have `BASE` tiles or more |
| `TILEMAP_REL_MAX` | longest package-relative resource path |
| `TILEMAP_DIR_SHIFT` | 8: a terrain cell is the slot in bits 0-7, the direction + 1 in bits 8-16 (0 = none), flags in 17-18 |
| `TILEMAP_CELL_SOLID` | bit 17: an obstruction; a walker can't enter unless its `pass` has it |
| `TILEMAP_CELL_WATER` | bit 18: water; a walker can't enter unless it swims |

`terrain_cell(slot, dir_deg)` packs a terrain cell (`dir_deg < 0`: no
direction), `terrain_cell_slot` / `terrain_cell_dir` unpack it (`dir` is
compass degrees 0..359, or -1). `tile_ref` / `tile_ref_tileset` /
`tile_ref_tile` do the same for tile layers. A tileset whose image is an
index map (`skin` set) is how the foliage and cliff art is stored.

#### Structs

**`TilemapTileset`**: `id`, `image` (e.g. `"textures/terrain/ground.png"`),
`skin` (palette for an index map, `""` = plain image), `tile_w` / `tile_h`,
`cols` / `rows` (0 when the image failed to load), `texture` /
`skin_texture` (TextureStore ids, 0 = not loaded).

**`TilemapTerrain`** (one ground terrain; the list order is the stacking
order, later terrains drawing over earlier ones):

| Field | Meaning |
|---|---|
| `image` | seamless fill texture; `""` = flat `color` |
| `color` | flat fill when there's no image (`a = 0`: unset) |
| `noise` | edge wander, 0..0.45 of a cell (0.5 or more would punch holes mid-terrain, so it's clamped) |
| `blades` | px its lower edges hang down, per column |
| `foam` / `foam_speed` / `foam_color` / `waves` | px band drawn on it where a higher terrain meets it; how fast it washes in and out (0 = still); its color; how far it washes, 0..1 (a calm lake: low) |
| `flow` | px/s its surface drifts along each cell's direction (rivers) |
| `sway` / `sway_speed` | px its blade tips (and half as far its fill) sway; how fast (1: a breeze) |
| `cliff` | face color under it on a raised level (`a = 0`: derived from its color) |
| `stairs` | square cells; the face under them draws as steps, and walkers climb cliffs there |
| `water` | a see-through block: walkers stand on the bed under it (wading or swimming by its depth); falls as a waterfall where it drops |
| `solid` | every cell is an obstruction |

**`TilemapWalker`** (what a walker can do; `TILEMAP_WALKER_DEFAULT` is
`{ 0, 1, 1, 2, 2 }`): `pass` (flags it goes through: `TILEMAP_CELL_SOLID`
phasing, `TILEMAP_CELL_WATER` swimming), `clearance` (levels of room it
needs over a floor, >= 1; water counts as room), `wade` (levels of water it
walks through on the bed), `wade_cost` / `swim_cost` (what a step into
wading / swimming water costs, >= 1).

**`TilemapStep`** (why `Tilemap_can_step` refused): `SOLID` (an
obstruction, and the walker can't pass solids), `WATER` (water, and it
can't swim), `CLIFF` (two or more levels up or down, no stairs), `VOID`
(off the map or no ground), `LOW` (a floor in reach, too little room above).

**`TilemapReach`**: a cell and the floor in it (`x, y, z`), `steps` on the
cheapest route there (0 = the start), its `cost` (steps plus extra for
water), and `from_x, from_y, from_z` (-1s for the start).

**Overlays** (`TILEMAP_MAX_*` caps how many a view draws):
`TilemapArc` (board points, a color and a width in screen px: a line
through the board's 3D, drawn as a ribbon with its shadow on the ground),
`TilemapMark` (a highlighted cell on floor `level`, < 0 the top one),
`TilemapRing` (a circle lying flat at a board point, `radius` in world px),
`TilemapSprite` (an upright card: `src` part of `texture`, its foot at
board `x, y`, `w, h` its size on screen at the target in world units,
standing on `level`, or at height `z` when `z >= 0`; it stays upright on
screen whatever the rotation, and the depth buffer sorts it against the
levels).

**`TilemapView`**:

| Fields | Meaning |
|---|---|
| `x, y, zoom` | the camera target (board point at the rect's center) and screen px per world px there |
| `bg` | fills the rect; the map area gets a darker shade |
| `grid`, `bounds` | cell grid (only when cells are 6 px or more on screen), outline of the map |
| `active_layer`, `dim` | the layer being edited (-1 = none); with `dim`, other layers draw faded |
| `cursor`, `cx, cy, cw, ch`, `erase_cursor` | cell-rect outline, in the erase color |
| `ghost`, `gx, gy, g_tileset, g_tx, g_ty, g_w, g_h`, `g_terrain` | translucent stamp preview at cell `gx, gy`; `g_terrain > 0` previews that terrain's swatch instead of tiles |
| `marker`, `mx, my`, `marker_level`, `marker_z` | a dot standing at world `mx, my`, on `marker_level` (when `marker_z < 0`) or with its foot at `marker_z` |
| `flags`, `arrows` | collision overlay and flow arrows on the active terrain layer |
| `sprites`, `arcs`, `marks`, `rings` (+ counts) | not owned by the view |
| `tilt`, `rotation`, `fov` | degrees from straight down (< 0: the map's), degrees clockwise, perspective field of view (0 = orthographic) |
| `supersample` | 2: render at twice the size and filter down; else 1 |
| `cut_level` | >= 0: cutaway everywhere above that level (the slices get dark caps); < 0: off |
| `room`, `room_x, room_y, room_z` | the cutaway that follows the room around that floor |
| `section` | with a cut: what's under each cell's floor draws dimmed |
| `see_through`, `st_x, st_y, st_z, st_radius` | the room's cut only in a circle (radius in screen px) around that board point, in front of it; the room defaults to the floor under the point |

**`TilemapMeshStats`**: `chunks` in the map and `built` so far, `vertices`
in the built ones, `built_last_frame` and `build_ms_last_frame`, `pending`
(in view, not built yet), `far` (the last frame drew the far view).

#### Function contracts

| Function | Contract |
|---|---|
| `Tilemap_new(w, h, tile, layers, names)` | `names` may be NULL (layers become "layer1", "layer2"...); NULL on bad sizes or out of memory |
| `Tilemap_add_layer` | appends an empty layer on level `elevation`; -1 when there are `TILEMAP_MAX_LAYERS` |
| `Tilemap_remove_layer` | the later layers move down one; clears the undo history (steps name layers by index) |
| `Tilemap_set_layer_kind` | clears the layer: its cells mean something else now |
| `Tilemap_set_levels` | 1..`TILEMAP_MAX_LEVELS`; lowering it moves layers above the new top down onto it |
| `Tilemap_set_elevation_style(level_height, tilt, slope_run)` | 1..`MAX_LEVEL_HEIGHT` px per level; default tilt 0..90 (drawn up to 85); incline run in world px. All saved with the map |
| `Tilemap_layer_height` | `level * level_height`: world px the layer stands above the board |
| `Tilemap_level_at(x, y, slot)` | the top floor: the highest solid block with none on it (water isn't a floor: a river's floor is its bed), or open water on level 0; -1 when none. `slot` (or NULL) gets its terrain |
| `Tilemap_floor_at(x, y, z, slot)` | the highest floor at or below level `z` (a cave's floor under the mountain on it) |
| `Tilemap_covered(x, y, z)` | a block anywhere above level `z`: something on floor `z` is inside |
| `Tilemap_water_at(x, y, z, deep)` | levels of water on floor `z` (< 0: the top floor), 0 when dry; `deep` (or NULL) is true when it's too deep to wade whatever the count (open water on level 0, which has no bed, or a floor painted as water) |
| `Tilemap_height_at(wx, wy, level)` / `Tilemap_floor_height(wx, wy, z, level)` | world px height of the top floor (or the highest at or below `z`) at a board point, lowered along inclines; `level` (or NULL) gets its level, -1 when nothing |
| `Tilemap_flags` / `Tilemap_paint_flags` | a terrain cell's flags, its own plus its terrain's (0 for empty cells and tile layers) / sets or clears flags on a rect's non-empty cells, returns cells changed. Repainting a cell's terrain keeps its flags; erasing clears them |
| `Tilemap_can_step(x0, y0, z0, x1, y1, w, z1, cost)` | `w` NULL = the default walker. Looks for a floor in the neighbor within one level, or further where the higher one is stairs, with `clearance` room; the nearest wins and goes in `z1`, the step's cost in `cost`. In water up to `wade` levels it wades on the bed; deeper needs `TILEMAP_CELL_WATER` in `pass`, and a swimmer counts as at the surface, so it can push off from a bank level with the water |
| `Tilemap_reach(x, y, z, max_cost, w, out, cap)` | every floor reachable for at most `max_cost` (capped at `TILEMAP_MAX_STEPS`), 4-way by `can_step`: the start first, then cheapest first; writes up to `cap`, returns the count |
| `Tilemap_path(x0, y0, z0, x1, y1, z1, max_cost, w, xyz, cap)` | the cheapest walk onto floor `z1` (< 0: whichever is cheapest) as x, y, z triples into `xyz` (room for `cap` cells); returns the cell count, 0 when none |
| `Tilemap_arc(a, b, peak, obstacle, out, n, hit)` | board space (x, y on the board, z = height; the renderer's 3D is X = x, Y = z, Z = y). Samples a parabola whose top is `peak` px above the higher end into `n >= 2` points evenly spaced along the board; returns the index of the first that dips into the ground or an obstruction counted `obstacle` px tall, -1 when clear; `hit` (or NULL) gets that point, refined |
| `Tilemap_trace(p, v, gravity, dt, out, n, landed)` | from `p` at velocity `v` (px/s, z up) under `gravity` px/s², one point per `dt`, until it lands or `n` points are written; returns the count. `landed` false: it never came down (e.g. flew off the map) |
| `Tilemap_raycast(origin, dir, max_dist, max_level, hit)` | the first point that meets the ground or a block within `max_dist`, skipping blocks above `max_level` (< 0: none skipped; a cutaway passes its cut) |
| `Tilemap_raycast_room(..., x, y, z, hit)` | the same against what an inside view of the room around that floor shows |
| `Tilemap_add_tileset` | the existing slot when `id` is already there; 0 when full. A missing image still gets a slot so saved data survives; its tiles just don't draw |
| `Tilemap_add_terrain` | adds, or updates the terrain with the same `id` in place (a map picks up tweaks to the game's list); 0 when full or a string is too long |
| `Tilemap_terrain_swatch` | the color for UI: `color` if set, else its image's average |
| `Tilemap_terrain_from_lua(L, t, out)` | reads a terrain table (a saved map's `terrains` entry fields) at stack index `t`; missing fields get defaults, a missing id leaves it `""` |
| `Tilemap_stamp(layer, x, y, tileset, tx, ty, w, h)` | copies the `w x h` block at column `tx`, row `ty` of a tileset to cells `x, y...`, clipped to the map; returns cells changed |
| `Tilemap_fill_empty` | `Tilemap_fill_rect` into empty cells only (a column brush filling the levels under it) |
| `Tilemap_flood` | 4-connected fill of the region matching the cell at `x, y` |
| `Tilemap_snapshot` / `_snapshot_layers` | starts an undo step (every layer is recorded; the list form is kept for callers that have one) |
| `Tilemap_drop_snapshot` | discards the newest step (a stroke that changed nothing) |
| `Tilemap_undo` / `Tilemap_redo` | the layer that changed, or -1 when there's nothing to undo / redo |
| `Tilemap_load(L, path, err, errcap)` | runs the data file with `L` in an empty environment; NULL with `err` filled on failure |
| `Tilemap_draw_tileset(slot, x, y, scale, hx, hy, sx, sy, sw, sh)` | one tileset image with a grid, the hovered tile (`hx, hy`; -1 = none) and the selected block outlined. Doesn't scissor: it sits inside a UI scroll frame, which does |

### services/tilemap/tilemap_lua.h

`gramarye.tilemap` (see `docs/SCRIPTING.md`): map handles are userdata with
`__gc`. `TILEMAP_VIEW_KIND` (9) and `TILEMAP_PALETTE_KIND` (10) are the UI
custom nodes, exposed to Lua as `gramarye.tilemap.VIEW_KIND` /
`PALETTE_KIND`; `tilemap_lua_draw` is called from the app's custom-draw hook
and draws whatever `view()` / `palette()` last bound (held by registry ref,
so a bound map can't be collected mid-frame), returning false for kinds that
aren't the tilemap's. `tilemap_lua_shutdown` runs after the Lua state is
closed and before the window closes. `tilemap_lua_reload_shaders` (F5) and
`tilemap_lua_watch_shaders` (dev builds, once a frame) wrap
`Tilemap_reload_shaders` / `Tilemap_watch_shaders`.

## Services/world

### services/world/planet.h

Defines the icosphere-based `Planet` geometry: `PlanetV3` (a 3D point), and `Planet` itself
(cell positions, per-cell neighbor list up to `PLANET_MAX_DEGREE` (6), degree, and corner
geometry). `PLANET_MAX_LEVEL` defaults to 9 unless predefined. Functions: `planet_create`,
`planet_rebuild`, `planet_destroy`, `planet_cell_count_for_level`, and the inline
`planet_cell_latitude`.

Hexes keep a fixed physical size (about 30 km across) at every level, so the level sets the
planet's size: `PLANET_EARTH_LEVEL` (8) is Earth's radius (`PLANET_EARTH_RADIUS_M`), and each
level below it halves the radius.

### services/world/planet_render.h

Builds and updates a raylib `Model` for rendering the planet: `planet_model_build`,
`planet_model_update_colors` (recolor from a `terrain` array), `planet_model_apply_colors`
(recolor from an explicit per-cell `Color` array), and `planet_model_unload`.

### services/world/world_gen.h

Defines the pluggable terrain-generation interface.

- `WorldGenParams`: seed, sea level, mountain level, noise scale, warmth, rain shadow, moisture
  reach, hydrology toggle + river density, plate count. `world_gen_default_params(seed)` builds a
  reasonable default set.
- `WorldGenSample`: elevation/temperature/humidity output of sampling one point.
- `WorldGenerator`: named generator with a `sample()` function pointer and opaque `state`.
  `world_gen_count()`, `world_gen_get(index)`, `world_gen_name(index)` enumerate the registered
  generators.
- `tile_classify()`: turns raw elevation/temperature/humidity into a `TileTerrain` (this is what
  `components/tile.h` refers to as the derivation step for `TileComp.terrain`).

### services/world/world.h

The main `World` service — the planet/world simulation used by the rest of the game (referenced
from `GlobalState.world`).

- `WORLD_GLOBE_KIND` (1) / `WORLD_CONTROLS_KIND` (2): kind ids for Lua custom-draw nodes.
- Level/generation lifecycle: `world_create`, `world_destroy`, `world_set_level` /
  `world_level` / `world_cell_count`, `world_set_pending_level` / `world_pending_level`,
  `world_set_generator` / `world_generator_index` / `world_generator_name`, `world_seed`,
  `world_regenerate`, `world_apply`, `world_step_generation` (incremental — check
  `world_generating`, `world_gen_stage_name`, `world_gen_stage`, `world_gen_stage_count` for
  progress), `world_reroll`, `world_set_seed`, `world_dirty`.
- `world_params()` exposes the live `WorldGenParams` for direct tuning.
- `WorldTileInfo`: a flattened snapshot of one tile's simulation state (cell, elevation,
  temperature, humidity, rainfall, flow, river, region, plate, fault, stress, terrain, lat/lon,
  neighbor count) for UI/Lua consumption.
- Selection: `world_selected`, `world_selected_info`, `world_clear_selection`.
- `world_neighbor_count` / `world_neighbor_info`: the selected cell's neighbors, indices
  `0..world_neighbor_count()-1` (may be 5 at the 12 icosphere pentagon cells, otherwise 6); same
  field set as `world_selected_info`, for the neighbor in that slot.
- `world_selected_screen_pos`: projects the selected cell's world-space position into the rect it
  was last drawn into via `world_draw_in_rect`, so UI can anchor a popup on it. Returns false if
  nothing is selected, the globe hasn't drawn this frame, or the cell is currently rotated to the
  far side of the globe.
- `world_set_input_suppressed`: `world_draw_in_rect` does its own raw-mouse click/drag/zoom
  handling against the whole globe rect, with no awareness of any Lua UI (e.g. a popup) drawn on
  top of part of that rect later in the same frame — so a click meant for the popup would also
  reselect/rotate the globe underneath it ("click-fighting"). Call this once per frame, before the
  globe draws, with `true` whenever the mouse is over such an overlay; `world_draw_in_rect` then
  ignores mouse input entirely for that frame (it still renders normally).
- `world_planet()` / `world_fields()`: read-only query surface for other services (e.g.
  `services/local_chunk`) that need to derive fine-grained per-hex data from the coarse world.
  `world_fields()` returns a `WorldFields` *view* over `World`'s own arrays — no copy is made, and
  no field in it should be written through by callers outside `services/world`.
- Tuning setters: `world_set_sea_level`, `world_set_warmth`, `world_set_mountain_level`,
  `world_set_noise_scale`, `world_set_rain_shadow`, `world_set_moisture_reach`,
  `world_set_hydrology` / `world_hydrology_enabled`, `world_set_river_density`,
  `world_set_plate_count`.
- `WORLD_VIEW_*` enum: the available debug/visualization render modes (terrain, temperature,
  rainfall, flow, region, wind, elevation, moisture, plates); `world_set_view` / `world_view`.
- Drawing: `world_draw_in_rect`, `world_draw_controls_in_rect`.

### services/world/world_lua.h

Registers the `World` service's Lua bindings: `world_lua_register(host, world)`.

### services/world/world_pipeline.h

The generation pipeline's shared data structures and stage entry points.

- `WORLD_NO_WATER` (`-1e30f`): sentinel value for `WorldFields.water_level` on dry land.
- `WorldFields`: parallel per-cell arrays (elevation, temperature, humidity, rainfall, moisture,
  downhill, flow, river, water_level, region, terrain, plate, fault, stress) plus a `planet`
  pointer, `params`, and `count`. This is the struct `world_fields()` in `world.h` returns a view
  of.
- `WORLD_FAULT_*` enum: `NONE`, `CONVERGENT`, `DIVERGENT`, `TRANSFORM`.
- Wind: `world_climate_wind_at`, `world_climate_wind`, `WindField` (+ `world_wind_field_build`,
  `world_wind_field_free`, `world_wind_sample`), and `world_climate_run`.
- Plates: `PlateField` (+ `world_plate_field_build`, `world_plate_field_free`) and
  `world_tectonics_run`.
- Remaining stages: `world_biomes_classify`, `world_hydrology_run`, `world_regions_run`.

## States

### states/global_state.h

`GlobalState` is the shared state handed to C systems as `userData`. It also carries the arena
tiers used throughout the engine:

- persistent arena (owned by `app.c`): engine lifetime, never reset.
- `scene_arena`: rewound on every full scene change (owned by `ScriptHost`).
- `frame_arena`: rewound at the top of every render frame (owned by `app.c`); systems can
  allocate per-frame scratch here at zero free cost.

`World` is forward-declared in this header so `global_state.h` doesn't have to pull in the whole
`world/` header set.

Field notes: `total_time` is accumulated sim time in fixed steps; `frame_count` is the sim tick
count; `tile_type` is the `ComponentTypeId` for `TileComp` (planet surface cells); `world` is the
planet/world sim service (see `services/world/world.h`).

## Systems

### systems/skinned_sprite/skinned_sprite_system.h

The UV-remap skinning render system (see `components/skinned_sprite.h` for the technique).
`skinned_sprite_system_register(ecs, state)` registers it the same way `sprite_system` does: as a
normal ECS system so Lua can toggle it, but it is **not** added to any scene — `app.c` drives it
directly from the render phase via `ECS_update_system`, under the `GlobalState` camera and before
the UI pass. It loads the skin shader on first registration and keeps it for the process lifetime;
call `skinned_sprite_system_shutdown()` once, after the last `ECS_update_system` call, to release
it early.

### systems/sprite/sprite_system.h

The plain sprite render system. `sprite_system_register(ecs, state)` registers it as a normal ECS
system so Lua can toggle it (`gramarye.systems.enable("sprite_render", ...)`), but it is **not**
added to any scene: drawing must happen inside `BeginDrawing`, so `app.c` drives it from the
render phase via `ECS_update_system`, under the `GlobalState` camera and before the UI pass. It
iterates sprite components densely — the full entity set is never scanned.

## Systems/tilemap

### systems/tilemap/draw_tilemap.h

Declares `TilemapRenderType` (`TOPDOWN`, `HEXAGONAL`, `ISOMETRIC`) and a `draw_tilemap()`
declaration. Note: this header is already broken/incomplete in the codebase (missing semicolons
after the enum and the function parameter list) — pre-existing broken code, left untouched per the
no-behavior-change rule.

## Top-level

### entities_lua.h

`entities_lua_register(host, ecs, state)` installs the `gramarye.entities.*` Lua API
(spawn/despawn/set_transform/set_sprite) and `gramarye.textures.load`. Entity handles exposed to
Lua are full userdata wrapping an `EntityId`.

### game_config.h

Compile-time game constants: `GAME_WIDTH` (1600), `GAME_HEIGHT` (900), and `GAME_TITLE`
(a template placeholder, `"__APP_TITLE__"`, substituted per-project).

### game.h

Currently empty (no declarations).

### global_system.h

`global_system_register(ecs, userData)` registers the global ECS system.

### scene.h

A `Scene` is a small value type: a name plus the C systems that run while it is active. It is
embedded *by value* in `ScriptHost`'s scene stack, so scene transitions allocate nothing. The
resolved `System*` list (`sorted`) is cached and kept sorted by priority; it is only rebuilt when
the system set changes (`dirty` tracks this). `name` points at the owner's storage, it is not
owned by the `Scene`. `SCENE_MAX_SYSTEMS` caps a scene to 32 systems.
`Scene_init`, `Scene_add_system`, `Scene_run` round out the API.

### script_host.h

The Lua scene host. Scenes are Lua files at `scripts/scenes/<name>.lua` inside a mounted package
(see `services/mounts/mounts.h`) that return a table
of hooks: `on_enter`, `on_update(dt)`, `on_draw`, `on_exit`, and (for the scene stack) `on_pause` /
`on_resume`. Lua orchestrates which systems run, UI, and transitions; heavy per-frame work stays
in C systems.

- Timing model: C systems of the active scene run at a fixed timestep via
  `ScriptHost_update_fixed` (deterministic simulation); `on_update`/`on_draw` run once per render
  frame via `ScriptHost_update`/`ScriptHost_draw`, so raylib's per-frame input edges
  (`key_pressed`) behave naturally in Lua.
- Scenes form a stack: `change()` replaces the whole stack, `push()` suspends the top scene
  (`on_pause`) and enters a new one, `pop()` exits the top (`on_exit`) and resumes the one below
  (`on_resume`). Only the top scene updates/draws.
- `ScriptHost_scene_arena`: the arena rewound on every full scene change (not on push/pop) —
  scene-lifetime C allocations go here.
- `ScriptHost_register_system`: makes a C system addressable from Lua as
  `gramarye.systems.add/enable(name)`.
- `ScriptHost_register_function`: installs an extra C function as `gramarye.<module>.<name>`
  (`module == NULL` installs at the root).
- `ScriptHost_request_scene` / `_push` / `_pop`: deferred transitions (backing
  `gramarye.scene.change/push/pop`); applied after `on_update`, never mid-hook.
- `ScriptHost_update_fixed`: runs the active scene's C systems; call 0..N times per render frame
  with a constant `dt`.
- `ScriptHost_update`: per-render-frame — runs Lua `on_update(dt)`, then applies any pending scene
  transitions.
- `ScriptHostLoadedFile`: a script file the host loaded (`rel`, resolved `path`, the `mount` it
  came from, `mtime`) and `below`, the mount limit its lookup used (`INT32_MAX` for a normal
  lookup, the caller's mount for `require_base`), so a watcher can redo the same lookup and notice
  when a different mount would now win. `ScriptHost_loaded_count` / `_get` list them (up to
  `SCRIPT_HOST_MAX_TRACKED`).
- `ScriptHost_clear_modules`: drops the cached `gramarye.require` modules (and the loaded-file
  list), so the next require re-resolves through the current mount stack.
- `ScriptHost_unload_all`: exits every scene on the stack (`on_exit` hooks run) and clears any
  error.

### viewport.h

Currently empty (no declarations).

### app.h / app_mode.h

`gramarye_app_run(const AppConfig *)` is the runtime entry point that each executable's `main()`
calls (`apps/*/main.c`). `AppMode` (launcher / game / editor) selects which packages mount and which
home scene runs; in a single-binary build it says which half is running. Package manifests list
the modes they mount in. `AppMode_name` / `AppMode_parse` convert to and from `"launcher"`,
`"game"`, `"editor"`.

| Executable | `AppConfig` | Does |
|---|---|---|
| launcher | `APP_LAUNCHER` | picks mods, spawns game / editor |
| game | `APP_GAME` | runs the game with the chosen mods |
| editor | `APP_EDITOR` | edits the game with the chosen mods |
| single (`apps/single`) | `APP_LAUNCHER`, `single = true` | web / Android: one binary; the launcher switches modes in-process (`gramarye.app.launch`) instead of spawning |

`AppConfig` also carries `start_scene` (NULL: the mode's home scene), `argc` / `argv`, and
`native` / `native_count`: the `NativeModule`s (a name and a `luaopen` function) compiled into this
build, exposed as `package.preload[name]` so Lua gets them with `gramarye.require(name)`, and
checked against a mod's `native` list. Every executable includes the same list from
`apps/native_modules.h`, so the launcher can tell which mods the game and editor builds accept
(how to add one: `docs/SCRIPTING.md`, "Genre C libraries"). The list starts with a placeholder
entry so the array is never empty.

Command line, the same for every executable:

| Flag | Meaning |
|---|---|
| `--mods id1,id2` | mods to mount, lowest priority first |
| `--launch-file PATH` | read the mods from a launch file (default `<user>/last_launch.lua`) |
| `--scene NAME` | start in this scene instead of the app's home |

### services/mounts/mounts.h

The mount stack: the one place that turns a resource path (`"scripts/scenes/x.lua"`,
`"textures/ui_atlas.png"`) into a real file path. Packages and mods are pushed as roots; index 0
is the lowest priority (pushed first), and a lookup searches from the top, so a mod overrides a
base file just by shipping the same relative path.

Where the roots live is decided once in `Mounts_init`, never by the working directory, so gdb,
raddbg, `run_desktop.sh` and a double-click all see the same files:

| Build | Roots |
|---|---|
| dev (`GRAMARYE_SOURCE_DIR` defined) | the absolute source tree |
| release desktop | next to the executable |
| web | MEMFS `packages/` |
| Android | the APK assets root (`""`) |

A `Mount`'s `root` always ends in `/` (or is `""` for the APK root).
`Mounts_packages_dir` / `Mounts_mods_dir` (`""` when the platform has none) / `Mounts_user_dir`
(saves, the last launch, logs) are the three folders. Runtime data never goes into a mount; the
one exception is the editor saving authored content (`maps/`) into the package being edited.

| Function | Contract |
|---|---|
| `Mounts_find(rel, out, cap)` | silent lookup from the top; returns the mount index (or -1) and writes the full path |
| `Mounts_find_below(below, ...)` | the same, searching only mounts strictly below `below` (for `require_base`) |
| `Mounts_resolve` | `Mounts_find`, but logs every root it tried when nothing matches |
| `Mounts_index_of_path(full)` | which mount a resolved path belongs to, or -1 |
| `Mounts_file_exists` | `FileExists` that also works for APK assets on Android |
| `Mounts_log` | logs the stack: `MOUNT [n] id -> root` |

### services/mounts/packages.h

Packages are folders of content (scripts, textures, fonts...) with a `mod.lua` manifest.
Built-ins are listed in `packages/index.lua`; user mods are found by scanning the mods folder
(desktop only). `Packages_build_stack` turns the built-ins for an app mode plus the user's chosen
mods into the mount stack.

`mod.lua` is plain data, run with an empty environment:

```lua
return {
  id = "com.you.mymod", name = "My Mod", version = "1.0.0",
  depends  = { "com.gramarye.engine" },
  native   = { "gramarye.tactics" },     -- C modules the app must provide
  apps     = { "game", "editor" },       -- default for mods: game + editor
  replaces = { "com.gramarye.core" },    -- total conversion: drop these
  home     = { game = "my_menu" },       -- the scene splash / quit return to
}
```

A `Package` holds those fields (`apps` as a bit per `AppMode`), plus `builtin`, `valid` (the
manifest loaded and has an id) and `error` (why it isn't valid).

| Function | Contract |
|---|---|
| `Packages_load_data(L, path, err, cap)` | runs a Lua data file (manifest, launch file) with an empty environment and leaves its table on the stack; false (nothing pushed) with `err` filled on failure |
| `Packages_scan(L)` | reads `packages/index.lua` and scans the mods folder; call again to rescan |
| `Packages_check(p, mode, check, user, reason, cap)` | can this package mount in `mode`; writes a readable reason when not. `check` answers "is this C module compiled in?" for `native` |
| `Packages_build_stack(mode, mods, n, ...)` | replaces the mount stack with the built-ins for `mode`, then `mods` in order, minus what a mod `replaces`, minus anything with missing `depends` / `native`, sorted so dependencies sit below their dependents. Problems are logged and the first is copied to `err`; false only when nothing could be mounted |
| `Packages_active(i)` | the mounted stack, 0 = lowest priority |
| `Packages_home(mode)` | the home scene from the highest mounted package that sets one, or NULL |

### platform/spawn.h

`ChildProcess_spawn` / `ChildProcess_poll`: the launcher starting `game`/`editor` without blocking.
The child inherits stdout/stderr, so its log lands in the same terminal. Desktop only:
`ChildProcess_supported()` is false on web and Android, where the launcher switches modes
in-process instead. `argv` is NULL-terminated with `argv[0]` the executable path. `poll` updates
`running` / `exit_code` and returns `running`; `handle` is the pid on POSIX, the HANDLE on Windows
(0 when none), and `exit_code` is valid once `running` is false.

### dev/file_watch.h

Dev-only auto hot reload (compiled with `GRAMARYE_DEV_TOOLS`). `FileWatch_update` polls the
scripts the host loaded, by resolved path, and reloads the current scene when one changes, is
deleted, or is newly shadowed by a higher mount (e.g. you add a mod override while the game
runs).

### dev/shader_editor.h

Dev-only multi-line code editor drawn into a UI rect (the shader playground's). One global
instance; it handles its own keyboard and mouse input while focused.
- `shader_editor_set_text(text, keep_view)` replaces the text and resets the undo history.
- `shader_editor_revision()` bumps on every edit; `shader_editor_last_edit()` is the `GetTime()`
  of the last one (the playground recompiles a moment after it).
- `shader_editor_set_info` fills the left side of the status bar; `shader_editor_set_errors(log)`
  shows a GLSL error log under the code and parses its line numbers (NULL or `""` clears).

### dev/shader_playground.h

`SHADER_PLAYGROUND_KIND` (7) is the live preview canvas and `SHADER_EDITOR_KIND` (8) the code
editor, the custom-node kinds `shader_playground.lua` draws. `shader_playground_unload` saves
unsaved edits first. See `dev/README.md` for what the playground does.
