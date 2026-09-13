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
Two carried extra rationale:
- `set_transform` updates the existing component in place when present, so
  the per-frame Lua move path does no ECS allocation.
- `set_skinned_sprite`: `anim_tex_id` is a baked UV-animation atlas frame
  (produced by `tools/skin_baker`), while `skin_tex_id` is the detailed art
  texture — see `components/skinned_sprite.h` for more detail.

### main.c
- `WORLD_START_LEVEL` = 4 is subdivision level F16 (2562 tiles), chosen to
  balance visual detail against ECS entity-spawn cost; Lua/UI can raise it
  later via `gramarye.world`.
- `world_custom_draw` is the composition seam where a Lua `custom` UI node
  of kind `WORLD_GLOBE_KIND` gets routed to draw the globe into its rect.
  `LOCAL_CHUNK_DEBUG_KIND` belongs to the separate `services/local_chunk`
  service — this function is just the dispatch point, not the service
  itself.
- `ASSET_PREFIX` documents the Android-vs-other-platform asset path split
  (matches `script_host`'s prefix).
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
- `emscripten_set_main_loop_arg` never returns on web (the browser owns
  frame timing via `requestAnimationFrame`), so the shutdown/cleanup code
  below it never runs on that platform — only a tab close or a future
  `emscripten_cancel_main_loop` call ends it.

### scene.c
`scene_rebuild_cache` rebuilds the resolved `System*` cache via insertion
sort by priority (stable, bounded by `SCENE_MAX_SYSTEMS`), and only runs
when the system set is dirty/changed.

### script_host.c
- Asset path convention: APK-root on Android vs. an `assets/` directory
  next to the binary elsewhere.
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
- `load_into_entry` loads `scenes/<name>.lua` into an entry (refs + Scene)
  without calling `on_enter`.
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
  standard `require` with no filesystem access; game-local modules load
  from `assets/` instead.
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
  stack top during module installation. The asset prefix is exposed to
  Lua so the shared UI Lua layer can build correct texture paths while
  staying asset-model-agnostic itself (matches `l_require`'s path
  construction).
- `ScriptHost_new`: generational GC mode is used because per-frame
  workload (UI tables, closures) is almost entirely short-lived garbage
  that the minor collector reclaims cheaply.
- `ScriptHost_reload_current` drops the game-side module cache so
  `lib/*.lua` changes reload, while leaving `package.preload` (embedded
  `gramarye.*` modules) untouched.
- `draw_wrapped_text` handles tracebacks with embedded newlines as well as
  long unbroken lines (like asset paths) via character-wrapping.

## Services

### services/texture_store.c
No non-trivial comments in the original file.

### services/local_chunk/local_chunk.c
Converts a planet cell's coarse hex data into a fine square-grid "local
chunk" for close-up rendering, using a shared vector/frame-projection
toolkit (`v3add`/`v3sub`/`v3cross`/`tangent_frame`/`project_local`).

- `tangent_frame` builds an arbitrary but deterministic tangent-plane
  basis per the "Determinism rules" — orientation doesn't matter as long
  as it's self-consistent.
- `edge_midpoint` is a symmetric function of the unordered cell pair, so
  both sides agree on the same border point.
- North/south neighbor slots (`pick_north_south_slot`,
  `local_chunk_edge_role`) determine which of a hex's edges are treated as
  "staggered" (needing a half-tile column offset) vs. "clean". A border is
  only staggered if both adjacent cells agree it's their N/S slot — this
  reciprocity is checked by `local_chunk_debug_check_reciprocity` and
  logged at startup (see `local_chunk_lua.c`).
- `local_chunk_cross_border` staggering uses a symmetric tie-break (lower
  cell id = phase 0) and is explicitly **not generally invertible**.
- `stage_elevation` samples the world generator as a pure function of 3D
  position directly at each fine-grid point, so adjacent hexes sampling
  the same physical point get identical values with no blending/fading/
  seams. On top it layers a small small-scale "detail" fBm noise (fixed
  seed, no per-cell parameter) purely because the coarse generator's noise
  scale is tuned for continent-scale features and barely varies within one
  hex — without it, coastlines look like a single smooth line. A prior
  per-cell-seeded detail layer broke continuity and was intentionally
  avoided.
- `stage_rivers` connects actual inflow/outflow crossing points directly
  (a cell has at most one outflow but can have multiple inflows/
  confluences); only a genuine source/sink touches the hex center.
  `rasterize_river_path` adds minor per-path jitter/meander via noise.
- `stage_biome_context` is a deliberate stub for future extension.
- The debug-render section (`elevation_color`, `draw_tile_cells`,
  `local_chunk_draw_in_rect`) uses a fixed/absolute elevation-to-color
  ramp (not rescaled per chunk) so adjacent hexes stay visually comparable
  at shared edges.
- The 7-tile group compositor (`local_chunk_create_group`/
  `local_chunk_draw_group_in_rect`) draws a neighborhood by finding, per
  sample point, which tile's real hex polygon (projected into a shared
  frame) contains it, then converts into that tile's own local frame to
  look up its grid — this makes tiles compose edge-to-edge just like on
  the sphere.
- The single-hex square debug render (`local_chunk_draw_square_in_rect`)
  uses a different display frame (north = up on screen) than the
  elevation grid's storage frame, so it round-trips sample points through
  world space to index correctly. It also visually tints the north/south
  edge bands when they're staggered, and labels sample border crossings.
- The connected-squares render (`local_chunk_draw_connected_in_rect`)
  lays out a flat-top hex offset grid (dir index 0=N…5=NW) with a
  `PXROUND` macro that ensures shared tile edges round through the exact
  same function so adjacent tiles don't show a seam.

### services/local_chunk/local_chunk_lua.c
Exposes the local_chunk debug system to Lua as
`local_chunk.generate/clear/current_cell`, tracking a currently-active
7-tile group (`g_current_group`). `LOCAL_CHUNK_DEBUG_RES` sets the
per-tile grid resolution used specifically for these debug views. On
registration, if a world is present, it runs
`local_chunk_debug_check_reciprocity` and logs the mismatch count — the
last logged number for this check is recorded in `docs/SCRIPTING.md`'s
"Known limitations" section.

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
