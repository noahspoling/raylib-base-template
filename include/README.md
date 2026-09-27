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

Defines `CHUNK_SIZE` (64) and a `Chunk` struct (`cx`, `cy` plus a `Tile` member). Note: this
header is already broken/incomplete in the codebase (missing type name and semicolons on the
`Chunk` typedef, a stray bare `#` line) — that is pre-existing broken code, not a comment, and was
left untouched per the no-behavior-change rule.

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

POD components for the plain (non-skinned) sprite render path. Registered in `main.c`; their type
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

Per-hex local map generation. See `local_chunk/README.md` in that directory for the full design
(HEX = SQUARE model, determinism rules, staggered borders, pipeline stages, known limitations).
This is a service separate from `services/world` — nothing in this file references `World*`.

- `LOCAL_CHUNK_MAX_NEIGHBORS` = `PLANET_MAX_DEGREE` (6; only 5 at the 12 pentagon cells of the
  icosphere).
- `LocalChunkNeighbor`: coarse sample of one neighboring hex. `cell` is -1 if the slot is unused
  (degree < 6). `flows_into_me` / `i_flow_into_it` record the coarse downhill-flow relationship
  between this cell and the neighbor in each direction.
- `LocalChunkFields`: the full per-chunk generation state.
  - `res`: grid resolution (an `res x res` grid).
  - `seed`: `hash(world_seed, cell)`.
  - `elevation` / `river_mask`: `res*res` row-major arrays filled in by the elevation and rivers
    pipeline stages respectively.
  - `extent`: grid half-width in local tangent-plane units.
  - `planet` and `world` are stored **by value, not by pointer** — `local_chunk_create()` is
    usually called with a `WorldFields` built on the caller's stack, and debug renderers read this
    stored copy again on later frames, so a pointer would dangle. See the directory README for
    more.
  - `generator_index` selects which `WorldGenerator` (see `world_generator_index()`) the elevation
    stage samples continuously.
- `LocalChunkStage`: one named pipeline stage (`run` function). `local_chunk_stage_count()` /
  `local_chunk_stage_get()` enumerate the registered stages.
- `local_chunk_create()`: gathers self + neighbor coarse samples for `cell` from `planet`/`fields`
  (typically `world_planet()`/`world_fields()`) and `world_seed` (`world_seed()`).
  `generator_index` (`world_generator_index()`) selects which `WorldGenerator` the elevation stage
  samples continuously — see the directory README's "The pipeline" section. This call does *not*
  run any stages; call `local_chunk_run()` afterward.
- `local_chunk_draw_in_rect()`: debug-only render — an auto-contrast heightmap of one cell in
  isolation.
- `LocalChunkGroup`: a cell plus every one of its generated neighbors; `tiles[0]` is always the
  center.
- `local_chunk_draw_group_in_rect()`: debug-only hex-polygon mosaic composite (see directory
  README, "Debug views").
- `local_chunk_cache_key()`: deterministic key for an eventual on-disk cache (no serialization
  exists yet).
- Staggered-border support (see directory README for the full model):
  - `LocalChunkEdgeRole` (`CLEAN` / `NORTH` / `SOUTH`).
  - `local_chunk_edge_role()`, `local_chunk_cross_border()`, `LocalChunkCrossing`.
  - `local_chunk_debug_check_reciprocity()`: diagnostic sweep that logs
    `local_chunk_edge_role()`'s actual reciprocity fallback rate across the whole planet — see the
    directory README's "Known limitations" section for the last measured number.
  - `local_chunk_draw_square_in_rect()`: debug-only render of one hex as a plain square grid.
  - `local_chunk_draw_connected_in_rect()`: debug-only, the current default UI view — center
    square plus connected neighbor squares laid out per the flat-top-hex offset scheme.

### services/local_chunk/local_chunk_lua.h

Composition glue: the only file in the codebase that knows about both `services/world` and
`services/local_chunk` (see `local_chunk/README.md`).

- `LOCAL_CHUNK_DEBUG_KIND` (3) and `LOCAL_CHUNK_GROUP_DEBUG_KIND` (4) must match the `kind` values
  `planet.lua` passes for its debug custom nodes — connected-squares (default UI view) and
  hex-mosaic verification view, respectively.
- `local_chunk_lua_current()`: returns the most recently `generate()`-d group, or an empty
  (`count == 0`) one if none has been generated yet.

## Services/world

### services/world/planet.h

Defines the icosphere-based `Planet` geometry: `PlanetV3` (a 3D point), and `Planet` itself
(cell positions, per-cell neighbor list up to `PLANET_MAX_DEGREE` (6), degree, and corner
geometry). `PLANET_MAX_LEVEL` defaults to 9 unless predefined. Functions: `planet_create`,
`planet_rebuild`, `planet_destroy`, `planet_cell_count_for_level`, and the inline
`planet_cell_latitude`.

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

- persistent arena (owned by `main.c`): engine lifetime, never reset.
- `scene_arena`: rewound on every full scene change (owned by `ScriptHost`).
- `frame_arena`: rewound at the top of every render frame (owned by `main.c`); systems can
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
normal ECS system so Lua can toggle it, but it is **not** added to any scene — `main.c` drives it
directly from the render phase via `ECS_update_system`, under the `GlobalState` camera and before
the UI pass. It loads the skin shader on first registration and keeps it for the process lifetime;
call `skinned_sprite_system_shutdown()` once, after the last `ECS_update_system` call, to release
it early.

### systems/sprite/sprite_system.h

The plain sprite render system. `sprite_system_register(ecs, state)` registers it as a normal ECS
system so Lua can toggle it (`gramarye.systems.enable("sprite_render", ...)`), but it is **not**
added to any scene: drawing must happen inside `BeginDrawing`, so `main.c` drives it from the
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

The Lua scene host. Scenes are Lua files at `assets/scripts/scenes/<name>.lua` that return a table
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

### viewport.h

Currently empty (no declarations).
