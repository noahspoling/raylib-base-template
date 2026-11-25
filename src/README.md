# template/src — implementation notes

This file collects the documentation that used to live as inline comments
throughout `template/src/`. It's organized to mirror the directory layout.
Files with no entry had no comments worth preserving (they were either
empty or the comments were purely boilerplate).

## Components

### components/atlas.c
The entire file was a `#include "components/atlas.h"` plus a commented-out
stub `Atlas_frame_rect` function (it would have returned a zero `Rectangle`
if the atlas/frame data was missing or out of range, otherwise indexed into
`atlas->frames`). No active code existed, so the file is now empty.

## Top-level

### entities_lua.c
Each Lua-exposed entity/texture function had a one-line doc comment giving
its Lua-side signature, e.g. `gramarye.entities.spawn() -> entity`,
`gramarye.entities.despawn(entity)`, `gramarye.textures.load(path) -> id`.
`gramarye.textures.load` resolves the path through the mount stack and
falls back to the raw path (which then fails with a normal raylib warning),
so a missing texture never aborts the script.
Two carried extra rationale:
- `set_transform` updates the existing component in place when present, so
  the per-frame Lua move path does no ECS allocation.
- `set_skinned_sprite`: `anim_tex_id` is a baked UV-animation atlas frame
  (produced by `tools/skin_baker`), while `skin_tex_id` is the detailed art
  texture — see `components/skinned_sprite.h` for more detail.

### app.c
The runtime entry point, `gramarye_app_run(const AppConfig *)`. The thin
`main()` in each of `apps/{launcher,game,editor,single}/main.c` fills an
`AppConfig` (mode, single-binary flag, start scene, native C modules) and
calls it. See `include/app.h` for the command line.
- Startup order matters. Create the Lua state, put native modules in
  `package.preload`, scan packages and build the mount stack, then load
  anything that reads files (skin shader, UI font, world). Manifests are
  parsed with the Lua state, and `native` checks need the preload entries.
- Launch file: the launcher writes `<user>/last_launch.lua` before spawning.
  `game`/`editor` read it when started without `--mods`, so starting them
  bare from a debugger reproduces the launcher's last choice.
- `gramarye.app.*` (mode, home, launch, child, quit) and `gramarye.mods.*`
  (available, active, selected, problem) are registered here.
- Single-binary builds switch mode in-process at the top of the next frame
  (`apply_pending_switch`), never from inside a Lua callback.
- On desktop every log line is prefixed `[launcher]`/`[game]`/`[editor]`
  (`log_callback`), so a launcher and the child it spawned can share one
  terminal.
- `WORLD_START_LEVEL` = 4 is subdivision level F16 (2562 tiles), chosen to
  balance visual detail against ECS entity-spawn cost; Lua/UI can raise it
  later via `gramarye.world`.
- `world_custom_draw` is the composition seam where a Lua `custom` UI node
  of kind `WORLD_GLOBE_KIND` gets routed to draw the globe into its rect.
  `LOCAL_CHUNK_DEBUG_KIND` belongs to the separate `services/local_chunk`
  service — this function is just the dispatch point, not the service
  itself.
- `UI_FONT_BASE_SIZE`: the font is rasterized at 48px and the renderer
  scales glyphs by ratio to base size — chosen high enough that ~28px
  titles stay crisp while smaller text still downscales cleanly via
  bilinear filtering.
- `SIM_DT` / `MAX_FRAME_DT` document the fixed-timestep model: C systems
  step at constant dt, while Lua's `on_update`/`on_draw` run once per
  render frame. The dt clamp prevents a "spiral of death" catch-up burst
  after a window drag, debugger pause, or scene load.
- `FrameLoop` is the per-frame state struct that lets the same loop body
  run from a plain while-loop (desktop/Android) or as an Emscripten
  main-loop callback, since the browser drives frame timing on web.
- Dev builds also run `FileWatch_update` (`src/dev/file_watch.c`), which
  reloads the scene when a loaded script changes or a different mount
  starts providing it.
- The F5-reload key handling debounces because some X11/GLFW setups
  without detectable autorepeat resend key-PRESS every repeat tick rather
  than a single edge — without the debounce this would spam reloads and
  burn slots in gramarye-ui's fixed-size texture table.
- The per-frame flow is: fixed-step sim loop, then a once-per-frame Lua
  orchestration call, then the world sprite-render pass (drawn under the
  UI). `GramaryeUI_begin` opens a Clay layout so Lua's `on_draw` can call
  `gramarye.ui.render(tree)`.
- `frame_arena` is per-frame scratch memory for C systems, rewound each
  render frame.
- World-service creation must happen after the window is ready, because it
  needs a live GL context for mesh upload.
- Font loading: font 0 is the default UI font; Roboto is loaded with a
  fallback to raylib's built-in font. The codepoint set covers ASCII
  32–126 plus ×, —, …, • (Roboto lacks arrow glyphs, hence the demo uses
  `</>`). Bilinear filtering smooths glyph scaling for non-base sizes.
- `sprite_render` is registered only so Lua can toggle it on/off — it's
  never added to a scene's update list because `main` drives it directly
  from the render phase.
- `gramarye.app.screenshot(path)` (dev builds) is taken just before
  `EndDrawing`: it flushes what's still batched, then reads the back buffer,
  so the PNG is the app's own finished frame and nothing else on screen.
- Single-binary mode switches (`apply_pending_switch`) exit every scene
  first, so `on_exit` hooks run against the old mounts, then remount and
  enter the new mode's home scene.
- `gramarye.mods.*`: `available([app])` (every package found, with
  `ok` / `reason` for that app, default `"game"`), `active()` (the mounted
  stack, lowest priority first), `selected()` (the ids chosen last time,
  from the launch file or `--mods`), `problem()` (the first problem from
  building the stack, or nil).
- The tilemap shuts down after the Lua state closes: map `__gc` releases
  textures first.
- `emscripten_set_main_loop_arg` never returns on web (the browser owns
  frame timing via `requestAnimationFrame`), so the shutdown/cleanup code
  below it never runs on that platform — only a tab close or a future
  `emscripten_cancel_main_loop` call ends it.

### scene.c
`scene_rebuild_cache` rebuilds the resolved `System*` cache via insertion
sort by priority (stable, bounded by `SCENE_MAX_SYSTEMS`), and only runs
when the system set is dirty/changed.

### script_host.c
- Every file path comes from the mount stack (`services/mounts`); nothing
  here knows about platform asset roots.
- `SceneEntry` represents one live scene on the stack. Suspended
  (non-top) entries keep their Lua table ref and system list so
  push/pop preserves state; only the top entry updates/draws. Update/draw
  hook refs are cached at load time so the hot path is a cheap
  `lua_rawgeti` rather than a per-frame string lookup.
- `ScriptHost` struct: `arena` is engine-lifetime persistent, while
  `scene_arena` is rewound on a full scene change.
- `call_hook0`: rare hooks (`on_enter`/`on_exit`/`on_pause`/`on_resume`)
  are looked up by name and skipped while the entry is in an error state,
  since calling into an unknown Lua state risks cascading errors (see
  `docs/SCRIPTING.md`).
- Chunk loading: raylib's file IO abstracts over desktop / web-MEMFS /
  Android-APK. `run_file` loads and runs a path, leaving its single
  return value on the stack.
- `load_into_entry` resolves `scripts/scenes/<name>.lua` and loads it into an
  entry (refs + Scene) without calling `on_enter`. It stores the name first,
  so a scene that fails to load can still be retried by F5 or the watcher.
- `do_change` is a full transition: exits/unrefs the whole stack, rewinds
  the scene arena, then loads the named scene as the sole entry.
- `do_push`: if the new scene fails to load, the paused scene below it
  resumes; the error screen persists until the next successful transition
  or F5.
- `do_pop`: popping an errored scene discards its error state, presuming
  the scene below it is healthy.
- The `gramarye` Lua table is orchestration-level API only.
- `l_require`: `gramarye.*` modules are embedded in the gramarye-ui binary
  and registered via `package.preload`, so they resolve through the
  standard `require` with no filesystem access. Package modules resolve
  through the mount stack. `l_require_base` loads the copy one mount below
  its caller.
- `checkkey` accepts either a key id (from `gramarye.input.key`) or a key
  name — the intended usage is to resolve names once in `on_enter` and
  pass ids in hot paths to skip string comparisons.
- `gramarye.draw.*` stays immediate-mode deliberately: `on_draw` runs
  inside `BeginDrawing`/raylib's `rlgl` batching, so a Lua→C command
  buffer would add overhead with no batching benefit. Bulk drawing
  belongs in C systems (see `systems/sprite`) — these are just
  prototyping helpers.
- "Demo data source" functions stand in for a game's real C/ECS-owned
  data: the UI's List/Grid pull rows by index, and the full dataset never
  lives in Lua. A real game would back this with its own components and
  do sorting/filtering in C. `l_demo_row` is `gramarye.demo.row(i) ->
  name, value`, 1-based to match the List API; the returned value is
  marked "processed" in C.
- `install_module`/`install_bindings`: the `gramarye` table sits on the
  stack top during module installation. `gramarye.resolve(rel)` is the
  Lua-side path lookup for textures and other non-script resources.
- `ScriptHost_new`: generational GC mode is used because per-frame
  workload (UI tables, closures) is almost entirely short-lived garbage
  that the minor collector reclaims cheaply.
- `ScriptHost_reload_current` drops the game-side module cache so
  `lib/*.lua` changes reload, while leaving `package.preload` (embedded
  `gramarye.*` modules) untouched.
- `track_loaded` records each script's resolved path, mount and mtime. The
  dev watcher reads them through `ScriptHost_loaded_*`.
- The watchdog: a Lua infinite loop (or tail-call recursion, which never
  overflows the stack) raises no error, so the game would just freeze. A
  count hook (`lua_sethook`, every `WATCHDOG_CHECK_EVERY` = 100,000 VM
  instructions) checks the clock and raises a normal Lua error once a host
  call has run past its deadline (`WATCHDOG_SECONDS` = 3), so a hang lands
  on the error screen with a traceback. `watchdog_arm` sets the deadline
  before each call from C into Lua. Long work in Lua has to be sliced
  across frames (a coroutine, like `lib/stress_map`'s job).
- `require_base(name)` loads the same module from the next mount *below*
  the calling file, so a mod wraps a base module instead of copying it:
  `local base = gramarye.require_base("lib/widgets")`.
- Input: mouse buttons are raylib's (0 left, 1 right, 2 middle); wheel
  movement is positive away from the user (scroll up).

### services/mounts
- `mounts.c`: the mount stack and resolver. Roots are absolute in dev builds
  (`GRAMARYE_SOURCE_DIR`), so lookups don't depend on cwd, and no copy or
  symlink in the build dir can shadow the source. On Android, Gradle stages
  the contents of `packages/` at the APK assets root, and existence checks
  use `fopen` (raylib wraps it to read through `AAssetManager`), because
  `access()` can't see APK assets.
- `packages.c`: reads `packages/index.lua` and `mods/*/mod.lua` (manifests
  run with an empty `_ENV`: data files can build tables but can't touch
  `gramarye`, `io`, `os`...), then builds the stack for an app mode. A list
  field that's a bare string counts as a one-item list. Building filters by
  `apps` (e.g. a game-only mod while in the editor), drops what a kept mod
  `replaces`, then drops packages with missing `depends` / `native`,
  repeating until nothing changes (dropping one can strand another), and
  orders depth-first so dependencies come before their dependents;
  otherwise the input order (built-ins, then mods as the user ordered them)
  holds.

### platform/spawn.c
`posix_spawn`/`waitpid(WNOHANG)` (POSIX) or `CreateProcess` (Windows) for
the launcher's child process. It deliberately doesn't include raylib.h,
because windows.h clashes with it.
- `draw_wrapped_text` handles tracebacks with embedded newlines as well as
  long unbroken lines (like asset paths) via character-wrapping.

## Dev tools (`src/dev/`, dev builds only)

### dev/file_watch.c
Redoes each loaded file's lookup the way it was loaded (a plain lookup over
the whole stack, or a `require_base` lookup only below the caller's mount),
so it notices a file changing, disappearing, or being shadowed. After a
reload the host's loaded list is rebuilt.

### dev/shader_playground.c
Every `<name>.fs` in `GRAMARYE_DEV_DIR "shaders/"` is an entry (sorted
alphabetically for the UI). The selected one is recompiled from the editor
text `PLAYGROUND_DEBOUNCE` (0.3 s) after the last keystroke; `Ctrl+S` writes the file.
It draws as one quad with `texture0` = `<name>.png` when it exists (else
1x1 white) and an optional `<name>.vs`. A failed compile keeps the last good
shader: while compiling, raylib's `TraceLog` is routed through the
playground, which prints it as raylib would and keeps the error text for the
editor. File modification times are whole seconds, so changes on disk are
detected by comparing contents. A file deleted or renamed outside the app
triggers a rescan, keeping the text if it went away; an outside edit is
followed unless the editor has unsaved edits (then it warns). New shaders
start from `starter.fs` (or a built-in copy). raylib's Esc-to-quit is turned
off in the scene, since the editor uses Esc to drop focus. The Lua API is
1-based.

### dev/shader_editor.c
A small code editor drawn with raylib calls inside the custom-draw rect:
ASCII only (GLSL is ASCII; incoming text drops `\r` and non-ASCII and
expands tabs to spaces), a monospace font from `GRAMARYE_DEV_DIR "fonts/"`.
It keeps a byte offset per line and whether each line starts inside a
`/* */` comment (for highlighting), a selection as `[min, max)`, and the
column kept while moving up and down. Typing and deleting coalesce into one
undo step until the kind of edit changes or you pause. `Ctrl+/` toggles
`// ` on every selected line; a `}` typed on an otherwise empty line closes
one indent level; characters typed with Ctrl held are dropped;
`Ctrl+Enter` recompiles. GLSL error logs are parsed for line numbers across
drivers (Mesa `0:12(5): error`, NVIDIA `0(12) : error`, AMD/Intel
`ERROR: 0:12:`: all are `0`, then `:` or `(`, then the line) and wrapped
into rows that remember the line they mention, so a click jumps there.

## Services

### services/texture_store.c
No non-trivial comments in the original file.

### services/local_chunk/local_chunk.c
Builds a cell's fine, north-up square guide raster as a cached window onto
fields that are pure functions of world position, using a small
vector/frame toolkit (`v3add`/`v3sub`/`v3cross`/`project_local`/
`unproject_local`/`local_chunk_north_frame`).

- `local_chunk_owner_at` (domain-warped nearest cell center, found by a
  greedy walk over neighbors) and `local_chunk_blend_at` (barycentric
  weights over the geodesic triangle) are sphere-global: the result never
  depends on which cell asks. They replace the old N/S stagger, edge roles,
  and reciprocity check.
- `local_chunk_extent` = farthest polygon corner + max warp displacement, so
  everything a cell owns lies inside its own raster; the rest is apron.
- `edge_midpoint` is a symmetric function of the unordered cell pair, so
  both sides agree on the same river crossing point.
- `stage_fields`: generator sample + blended coarse residual (what
  tectonics etc. added at each cell center) + fixed-seed fBm detail for
  elevation; blended coarse temperature/humidity + small detail. No
  per-cell parameter touches a sampled value.
- `stage_ownership` fills `owner[]`; `stage_rivers` rasterizes self's and
  every neighbor's river paths, each built in its owning cell's own frame,
  so rivers run through the apron and match across borders.
- `stage_biome` classifies every sample with `tile_classify`.
- `build_group_view` resolves the 7-tile composite once at group creation
  (pixel -> owner tile + raster index) and counts `owner_mismatches` as a
  self-check; the draw functions are lookups only.
- Rivers are routed per cell over a coarse sample of the local topography:
  a least-cost path (Dijkstra from the source until it reaches the
  destination, an existing channel or water) that prefers low ground and
  avoids climbing, perturbed by noise so valleys aren't followed too
  predictably, then smoothed and meandered. Every stream stops at the first
  local water it reaches, and later streams are drawn into earlier channels
  so they merge. Routes depend only on (world, cell), so every chunk that
  draws a cell's rivers draws the same ones. The cost weights: pull toward
  the cell's lowest ground, a random ground preference (higher = less
  predictable), crossing ground another cell owns, climbing the cell's full
  elevation range, a multiplier along an existing stream, and land touching
  water (keeps rivers off the shoreline). Headwater streams shorter than a
  minimum are dropped as stubs.
- Meanders are a perpendicular noise offset along the arc length, tapered
  to zero at both ends so endpoints (cell edges, junctions) stay pinned, and
  wider on low, flat ground. A path that stopped on a channel snaps onto
  that stream. The first (largest) inflow carries the trunk to the outflow
  at this cell's width; later ones are tributaries at their own width until
  they join it. `river_mask` levels: 1 = minor stream (a faint wash),
  1 + Strahler order for the network (bolder with order).
- The topography view maps unitless elevation to meters above sea level so
  every preview uses one scale: hypsometric tint, north-west hillshade and
  contour lines (every 5th, 100 m, darker). Contours and shading are
  absolute; the tint is stretched over the view's own land range so small
  relief shows while checking generation (the same hex can tint differently
  in different previews).

### services/local_chunk/local_chunk_lua.c
Exposes the local_chunk debug system to Lua as
`local_chunk.generate/clear/current_cell`, tracking a currently-active
7-tile group (`g_current_group`). Grid resolution comes from the planet's
scale (`LOCAL_CHUNK_METERS_PER_PIXEL`), not from this file.
`generate` logs a warning if the group's `owner_mismatches` is nonzero.

### services/tilemap/tilemap.c
Map data, edits, undo, file IO and drawing (API in
`include/services/tilemap/tilemap.h`). Design notes:

- Undo is copy-on-write by tile: `Tilemap_snapshot` starts an empty step,
  and the first write into a `UNDO_TILE` (32x32) block of a layer during the
  stroke copies that block as it was (`undo_touch`, from every write path).
  Undo/redo swap the saved blocks with the map's. A step costs what the
  stroke touched, not the map's size. `drop_snapshot` lets a stroke that
  changed nothing leave no undo step.
- Inclines (`slope_window`): two distance fields per layer. The ground's
  counts empty cells and water on a floor one level down as an incline's
  low side (banks slope into water); the water surface's (`wslopes`, only
  on layers with water) counts empty cells only, so a surface stays level
  along its banks. A level with water gets a second top mesh from it
  (`wtop`), which the water pass draws; bank pixels under the surface get
  a shore code in the cover (32 + depth) and are drawn as water there.
- Edits mark dirty rectangles (`ids_rect`, `slope_rect` per terrain layer,
  grown by `SYNC_REACH`), and `sync_ids` / `slope_field` redo and upload
  just those (`UpdateTextureRec`; `slope_window` recomputes the incline
  distances from a window wide enough to be exact). A covered cell next to
  an open floor on its level takes that floor's terrain in the texture
  only, so the sliver a blended outline leaves at an incline's foot doesn't
  show the terrain hidden under the hill.
- Drawing counting-sorts the visible cells by tileset, then draws each
  tileset in one pass, so textures don't alternate cell by cell. Each
  skinned tileset gets its own Begin/EndShaderMode: raylib binds extra
  samplers (the skin) per batch, so two skins must never share one.
- Terrain layers draw as one quad each through `shaders/<glsl>/ground.fs`:
  the cells go up as a one-texel-per-cell texture (`sync_ids`, redone only
  where stale) and the shader blends each pixel from the 4x4 cells around it.
- The view is a 3D camera over the board (`view_camera`; `lib/camera.lua`
  mirrors its math for picking) rendering into its own render texture (at 2x
  and filtered down when `supersample` is set). The ground level draws in
  painter's order; raised layers are meshes, sorted with sprites by the depth
  buffer.
- Levels above the ground (`build_chunk`, per chunk of cells, rebuilt only
  when cells near it change): one footprint atlas per chunk, every level
  rendered by `ground.fs` in slot mode and read back once, gives each pixel
  two bitmasks: the levels with a solid block there, and with water. Per
  level, the top is a mesh at the drawn height (flat cells one quad, incline
  cells a grid of `SMOOTH_PX` quads whose pixel corners are interpolated from
  it, stairs per run of equal height) drawn by `ground.fs`, with a one-byte
  cover per pixel (solid above / water above / water depth) so covered tops
  are discarded early and water knows how deep it is. Solid faces come from
  subtracting the runs of solid blocks on either side of each pixel edge
  (water counts as air, so a river's banks show through it); water's faces
  from its runs against anything. A face covers only the band where one side
  has a block the other doesn't; split per level band but shaded as one face,
  drawn by `wall.fs`. Most edges have the same stack both sides and take a
  fast path; runs being merged are tracked in a bitmask, so ending them costs
  only what's open.
- Building is spread out (`update_meshes`): stale chunks in view, nearest the
  middle first, for `BUILD_BUDGET_MS` a frame (at least one), so a teleport
  fills in over a few frames; a chunk with geometry is rebuilt at most every
  `REBUILD_COOLDOWN_S` (painting); past `MAX_RESIDENT_CHUNKS` the chunk out of
  view longest is unloaded. `Tilemap_mesh_stats` reports it.
- The far view: below `FAR_ZOOM` no chunks are built; the board is an
  overview texture (one texel per cell: the top surface's color,
  hill-shaded) on a coarse mesh per chunk (`build_far`), both updated where
  cells change. A whole 1024 map draws at vsync.
- The frame draws the ground first, in painter's order with the depth test
  off and no depth writes (depth-tested after the levels, it lost to the
  1 px foot of an incline at a distance), then the levels highest first with
  the depth test on (hidden tops are rejected before they're shaded), raised
  tile layers, sprites, water in its own blended pass, then overlays.
- Saving (version 3) writes each layer as `rows` of run strings ("v*n"):
  a 1024x1024x16 stress map is 1.7 MB. Versions 1 and 2 still load.
- Inside views (`cut_request`, "The cut map"): a per-cell cut, the highest
  level shown. For a room it's flooded from the character's air under a roof
  (`cut_room`), each room column cut a level above its own floor, the rest of
  the map taking the nearest room column's cut; `cut_sync_gpu` uploads it as
  a texture and builds the dark section faces where the cut steps between
  two solid cells. The shaders cut, cap and dim by it (everywhere, or inside
  the see-through circle), and `raycast_cut` picks against it.
- Gameplay over blocks: `solid_point` (a stack of solid blocks is solid up
  to its top floor's height, inclines and stairs included; water isn't) is
  what arcs, traces and rays test; a floor is a solid block with no solid
  block on it (`floor_here`); `can_step` finds the neighbor's floor nearest
  the walker's (0, +1, -1, +2, ...), checks headroom and how it'd be in the
  water there (`floor_state`: wading on the bed, or swimming at the surface),
  and returns the step's cost. `block_layer` goes through a per-level index
  of terrain layers (`level_index`).
- Heights come in two flavors side by side: `layer_surface` for gameplay
  (`Tilemap_height_at`: inclines from the chamfer distance field in
  `slope_field`, stairs as a ramp) and `incline_draw_height` /
  `stairs_draw_height` for drawing (the incline foot 1 px up, stairs in
  treads). Keep them in step.
- Movement (`Tilemap_reach` / `Tilemap_path`) is Dijkstra (a binary heap
  of cost-keyed states, stale entries skipped) over floors `(x, y, level)` in
  a fixed window the budget can reach, so its cost doesn't depend on the
  map's size.
- Version 1 maps (raised layers as columns from the ground) convert on load
  (`blocks_from_columns`): each level takes the cell of the lowest old layer
  that reached it.
- Arcs, traces and ray casts sample coarsely, then bisect to the exact hit.
- The visible range is widened up/left by the largest tile overhang, so a
  2x2 tree whose anchor cell is just off screen still draws.
- Loading checks every tileset entry lands in its own slot (cells reference
  tilesets by position) and clears cells that point past the list.

**Where things are**, in file order (the techniques are in `docs/Notes`;
the note for each part is named):

| Part | Functions | Notes |
|---|---|---|
| Change tracking | `cells_dirty`, `cell_changed`, `cells_changed`, `region_changed`, `overview_dirty`, `rect_grow` | an edit marks the dirty rects of every terrain layer within `SYNC_REACH` (9) cells: incline distances and stairs runs reach about 8, and a cell's look depends on the levels above and below it. A whole-layer change re-uploads every raised layer too, since waterfalls depend on what's below |
| Levels | `level_index`, `block_layer`, `block_kind`, `solid_at`, `floor_here`, `floor_layer`, `water_over`, `surface_layer` | Cells, Layers and Levels. `level_index` is a lazily rebuilt per-level list of terrain layers (`lvl_first`, `lvl_next`) |
| Inclines | `slope_window`, `slope_field`, `water_slope_field` | Inclines and Stairs. Codes are `(d + 0.5) * 32` cells from the nearest incline edge: 0 just outside an edge, 32 an edge cell, 255 none in reach (also outside cells next to a cliff, so blending toward them keeps the full height). On a layer without water the water field is the ground's |
| Stairs and heights | `stairs_cell`, `stairs_geometry`, `stairs_t`, `incline_ramp`, `layer_surface`, `field_draw_height` | a stairs cell's front is found by walking each direction (south, east, west, north first) through its run to the first other cell, which must be off the layer and lower |
| Collision | `standable`, `floor_state` (inside `Tilemap_can_step`), `worse` | Collision and Walking Rules. Of two reasons a step failed, `worse` keeps the one worth reporting |
| Grid movement | `reach_alloc`, `heap_push`, `heap_pop` | Grid Movement - BFS. A state packs into `REACH_STATE_BITS` (18: 65 x 65 cells x 32 levels); a heap entry is `cost << 18 \| state`. A cheaper route pushes a state again and the old entry is skipped when it comes up (lazy deletion: no decrease-key). Each search allocates its own window and frees it on return, so searches never share state |
| Arcs | `solid_point`, `floor_under`, `arc_blocked`, `arc_k`, `arc_point`, `Tilemap_raycast` | Arcs - Jump, Throw, Fall. `z(t) = a.z + d t + k t (1 - t)` passes both ends; `k` puts its top at `max(a.z, b.z) + peak`. Obstacles don't count in the start and end cells; 1 px of grazing is allowed |
| Tilesets, terrains | `load_tileset_textures`, `image_average`, `Tilemap_add_terrain` | a terrain is compared as a normalized copy (zeroed past its strings) so `memcmp` spots real changes; a terrain with no loadable image and no color draws in a fallback color |
| Cells, undo | `put`, `keep_flags`, `Tilemap_flood`, `undo_touch`, `undo_swap`, `undo_trim` | Tilemap Editor Techniques. The flood recolors a cell as it pushes it, so the stack never exceeds the cell count |
| Save, load | `Tilemap_save`, `field_*`, `cell_value_ok`, `blocks_from_columns`, `Tilemap_load` | temp file + rename (on Windows the old file is removed first: `rename` won't replace one). A value can't be a cell when its slot or tileset is past the map's lists, or its direction past 360 |
| Shaders | `shader_files_load`, `shader_files_changed`, `Tilemap_watch_shaders` | each shader remembers its files' modification times. `g_shader_gen` bumps on every reload, and geometry built with an older ground shader rebuilds (its outline came from that shader). raylib's default shader is never unloaded. `shader_time` wraps every hour, so a float keeps sub-millisecond precision however long the app runs (animations jump once an hour) |
| Tile layers | `tile_src`, `bucket_reserve` | a counting sort of the visible cells by tileset; one texel = one world unit, so a tile bigger than a cell overhangs right and down |
| Terrain layers | `sync_ids`, `sync_fills`, `ground_uniforms`, `draw_terrain_layer`, `cut_texture` | Terrain Blending Shader; texel layout in Packing Data into Bits and Texels. All fills are the size of the first terrain image (32x32 when none); a flat-color terrain gets a lightly speckled fill from `hash3` (same output on every platform). `ground_uniforms` sets `outputMode` (`GROUND_COLOR`, `GROUND_SLOTS`, `GROUND_SHADOW`); the frame's inside view applies to colors only, not footprints or shadows |
| Raised layers | `build_chunk`, `work_*`, `span_*`, `run_flush`, `update_meshes` | Raised Terrain Geometry. Meshes are triangles **without index buffers**, so a chunk is never capped at 65,536 vertices; `meshbuf_upload` hands the arrays to raylib and drops the CPU copy. `MASK_PAD` (4) footprint pixels are kept around a chunk because blade roots reach past it; `MAX_RUNS` (16) separate stacks fit in one pixel |
| Far view | `surface_of`, `sync_overview`, `build_far`, `draw_far` | Raised Terrain Geometry, "The far view" |
| Cut map | `cut_room`, `cut_request`, `cut_sync_gpu` | Inside Views |
| Drawing raised | `mesh_material`, `draw_raised`, `cliff_color` | one material for every `DrawMesh`: material map *i* binds to the sampler at `locs[MAP_DIFFUSE + i]`, so the cells are map 0 (`texture0`), the fill strip 1, the cover 2, the cut map 3. A shader that failed to compile comes back as raylib's default; its table is left alone. Dimmed levels draw at `DIM_RGB` (110 of 255) |
| Sprites, aids, overlays | `draw_sprite_card`, `draw_arrows`, `draw_flags`, `draw_ghost`, `draw_ribbon`, `draw_overlays` | Sprites, Depth and Overlays |
| Camera | `view_camera`, `view_ray`, `ray_board`, `Tilemap_pick` | Board Camera. 3D is X = board x, Y = height, Z = board y; `push_board` makes 2D draws land on the board plane at a height |
| The frame | `Tilemap_draw` | The Tilemap Frame |

### services/tilemap/tilemap_lua.c
`gramarye.tilemap` (the API is in `docs/SCRIPTING.md`). `save` only writes
into a mounted package's `maps/` folder with a validated name (letters,
digits, `_`, `-`): authored content goes into the package being edited.
- Lua indices: cells 0-based, layers and slots 1-based; the binding
  converts layers to the C side's 0-based ones.
- The two custom nodes hold the bound map by registry ref, so a scene that
  drops its map before the UI renders doesn't free it mid-frame. Their
  sprite, arc, mark and ring arrays live in the binding and are rebuilt
  each `view()` call.
- A sprite's image is loaded once and kept for the session: sprites are
  passed every frame, so per-call loads would pile up references.
- `__gc` on a bound map only runs while the state closes, so it doesn't
  touch the registry; at shutdown the refs are already meaningless.

### services/world/world.c
- The `globe_x/y/fw/fh` and `globe_rect_valid` fields cache the screen
  rectangle the globe was last drawn into (per frame), so a world-space
  cell position can be projected back to screen space for a floating
  popup — see `world_selected_screen_pos()`.
- `input_suppressed` is reset to false on every `world_draw_in_rect` call,
  so Lua must set it fresh each frame if it still wants input suppressed.
- `project_to_globe_rect()` performs a manual camera-space projection of a
  world-space point into the screen rect the globe was last drawn into.
  It mirrors the inverse of `world_pick()`'s ray-cast basis, run forward
  instead. Returns false if the point is behind the camera, off-rect, or
  the globe hasn't been drawn this frame.
- In `world_selected_screen_pos()`, cells rotated to the far side of the
  globe are rejected because their outward normal (already unit length)
  points away from the camera.

### services/world/world_gen.c
The `clamp01`/`fbm` local aliases near the top exist so the generator
functions below read the same as they did before the hash/noise
primitives were moved into the shared `services/world_noise.h` header
(which `services/local_chunk` also depends on) — do not re-fork or
duplicate those implementations here; the shared header is the single
source of truth for the noise/hash primitives.

### services/world/world_lua.c
Thin Lua binding layer exposing the `World` singleton to scripts.
`push_tile_info` builds a Lua table from a `WorldTileInfo` struct and is
shared by both the `selection()` and `neighbor()` bindings so they expose
an identical field set for whichever cell each is inspecting. The
`neighbor()` binding takes a 1-based index from Lua script code
(`neighbor(1)` through `neighbor(neighbor_count())`), which is converted
to a 0-based slot internally.

### services/world/world.c, world_hydrology.c
- A river mouth stops at the shared border instead of running on into the
  sea or lake hex.
- River systems too short to read as rivers are dropped: a coastal hex that
  drains straight into the sea or a lake would otherwise show a stub hugging
  the shore.

### services/world/{planet,planet_render,world_biomes,world_climate,world_hydrology,world_regions,world_tectonics,world_wind}.c
No non-trivial comments in the original files.

## Systems

### systems/skinned_sprite/skinned_sprite_system.c
The skin shader (`g_skin_shader`) and its associated uniform locations are
module-level/static state rather than stored in `GlobalState`, because
it's a GPU resource shared by every skinned sprite and nothing outside
this file needs to touch it. It's lazily loaded on the first call to
`skinned_sprite_system_register()` and released in
`skinned_sprite_system_shutdown()`. In the draw callback, bailing out
when `anim.id == 0 || skinTex.id == 0` guards against drawing before the
animation/skin textures are loaded or baked. When loading the shader, a
`NULL` (0) vertex shader path is passed deliberately — raylib's default
sprite vertex shader is sufficient, since only fragment-stage texture
sampling needs to be customized for skinning.

### systems/sprite/sprite_system.c
The sprite's draw origin is set to half its width/height so that rotation
and scaling pivot around the sprite's center rather than a corner.

### systems/tilemap/render_tilemap_system.c
File is empty (0 bytes) — nothing to document.
