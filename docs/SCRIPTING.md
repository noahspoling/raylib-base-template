# Lua scripting (scenes)

Game orchestration is authored in **Lua 5.4** (vendored via FetchContent,
statically linked). Lua decides what a screen looks like, wires input/UI,
fires scene transitions, and chooses which C systems run per frame. Heavy
per-frame math belongs in **C systems** registered through the ECS.

## Timing model

The main loop runs a **fixed-timestep simulation** over a variable render
rate (frame dt is clamped to 0.25 s so pauses/window drags don't spiral):

- **C systems** of the active scene run 0..N times per render frame at a
  constant `dt = 1/60` (deterministic simulation).
- **`on_update(dt)`** runs **once per render frame** with the real (clamped)
  frame dt — so `gramarye.input.key_pressed` edges fire exactly once, as
  expected for orchestration/input code.
- **`on_draw`** runs once per render frame, inside the frame's draw pass.

Draw order per frame: **C sprite pass → `on_draw` (`gramarye.draw.*`) → UI**.

`gramarye.draw.*` is intentionally immediate-mode: `on_draw` already executes
inside `BeginDrawing` and raylib batches via rlgl, so a Lua→C command buffer
would add copying with no batching gain. Bulk drawing belongs in C systems
(see the sprite system) — `gramarye.draw` is for prototyping.

## Layout

Content is split into **packages**, each a folder with a `mod.lua` manifest:

```
packages/
├── index.lua            # built-in package folders, lowest priority first
├── engine/              # shared by every app
│   ├── mod.lua
│   ├── scripts/lib/     # ui.lua, ui/*.lua, widgets.lua
│   ├── scripts/scenes/  # splash.lua, pause.lua
│   └── fonts/ shaders/ textures/
├── core/                # the game (game + editor)
├── editor/              # editor-only scenes (tilemap editor, shader playground)
└── launcher/            # mod picker
mods/                    # user mods, same shape as a package
```

The running app mounts its built-ins, then the mods the user picked, as a
**stack**. Every lookup returns the file from the highest package that has it:
scene loads (`scripts/scenes/<name>.lua`), `gramarye.require("lib/x")`
(`scripts/lib/x.lua`), `gramarye.resolve("textures/x.png")` and
`gramarye.textures.load`. A mod overrides any file by shipping the same
relative path. The startup log prints the stack (`MOUNT [n] id -> path`), and
a missing file logs every path it tried.

On every target the files are read through raylib file IO, so one code path
covers desktop, web (MEMFS) and Android (APK).

### Three apps

| Executable | Mounts | Starts at |
|---|---|---|
| `<name>` (launcher) | engine, launcher | splash, then the launcher |
| `<name>_game` | engine, core, chosen mods | `home.game` (`game_main_menu`) |
| `<name>_editor` | engine, core, editor, chosen mods | `home.editor` (`editor_main_menu`) |

The launcher saves the mod choice to `user/last_launch.lua` and spawns
`game`/`editor`. Started without `--mods`, those read the same file, so
running `game` straight from a debugger reproduces the launcher's last
choice. Web and Android build one binary that switches mode in-process.
Command line for all of them: `--mods a,b`, `--launch-file PATH`,
`--scene NAME`.

### Manifests and mods

```lua
-- mods/my_mod/mod.lua  (plain data: runs with an empty environment)
return {
    id       = "com.you.my_mod",
    name     = "My Mod",
    version  = "1.0.0",
    depends  = { "com.gramarye.engine" },  -- must be mounted below this one
    apps     = { "game", "editor" },       -- default for mods
    native   = { "gramarye.tactics" },     -- C modules the build must provide
    replaces = { "com.gramarye.core" },    -- total conversion: unmount these
    home     = { game = "my_menu" },       -- where splash / "Quit to Menu" go
}
```

To **wrap** a base module instead of copying it, require the copy one mount
below the current file:

```lua
-- mods/my_mod/scripts/lib/widgets.lua
local base = gramarye.require_base("lib/widgets")
local M = setmetatable({}, { __index = base })
function M.HeaderBar(props) props.bg = { 90, 40, 110, 255 }; return base.HeaderBar(props) end
return M
```

See `mods/example_reskin` (wraps a module) and `mods/example_tc` (total
conversion).

## Scene contract

A scene file returns a table of hooks (all optional):

```lua
local t = 0
return {
    on_enter  = function() gramarye.systems.add("global") end,
    on_update = function(dt)
        t = t + dt
        if t > 2.0 then gramarye.scene.change("main") end
    end,
    on_draw   = function() gramarye.draw.text("GRAMARYE", 270, 250, 48) end,
    on_exit   = function() end,
    on_pause  = function() end,   -- another scene pushed on top
    on_resume = function() end,   -- the scene above was popped
}
```

`on_update` and `on_draw` are cached as direct references at scene load (no
per-frame table lookup); the other hooks are looked up by name when fired.

## Scene stack

- `gramarye.scene.change(name)` — replaces the **whole stack**: every live
  scene gets `on_exit` (top-down), the scene arena is rewound, the new scene
  becomes the only entry.
- `gramarye.scene.push(name)` — `on_pause` on the current top, then the new
  scene is entered on top. Suspended scenes keep all their state (Lua table,
  C system list, entities) but get no `on_update`/`on_draw`.
- `gramarye.scene.pop()` — `on_exit` on the top, `on_resume` on the scene
  below. Popping the last scene is refused (logged warning). Popping an
  errored scene clears the error state.
- `gramarye.scene.depth()` — current stack depth.

All transitions are **deferred**: they apply after `on_update` returns, never
mid-hook. See `scenes/pause.lua` for a working push/pop example.

Notes:
- If a scene hook errors, that scene's remaining hooks (including `on_exit`)
  are skipped — its Lua state is unknown, so cleanup calls would risk
  cascading errors. The error + traceback shows on screen; F5 reloads.
- Scene-arena C allocations made by pushed scenes are reclaimed at the next
  full `change`, not on `pop`.

## API (`gramarye` global)

| Function | Notes |
|---|---|
| `gramarye.log(msg)` | raylib TraceLog INFO |
| `gramarye.require(path)` | load+cache `scripts/<path>.lua` from the top-most package that has it (e.g. `"lib/util"`) |
| `gramarye.require_base(path)` | same path from the next package *below* the calling file (wrap instead of copy) |
| `gramarye.resolve(rel)` | full path of a resource through the mount stack, or nil (logs every path tried) |
| `gramarye.app.mode()` / `home()` / `is_dev()` / `is_single()` | which app is running and its home scene |
| `gramarye.app.launch(app, { mods = {...} })` | launcher: start `"game"`/`"editor"`; returns `true` or `nil, err` |
| `gramarye.app.child()` / `quit()` | spawned child status `{ app, running, exit_code }` / close (single builds return to the launcher) |
| `gramarye.app.screenshot(path)` | dev builds: saves this frame, once it's drawn, to a PNG (for tests that need a picture of the app and nothing else on screen) |
| `gramarye.mods.available([app])` / `active()` / `selected()` / `problem()` | installed packages with `ok`/`reason`, the mounted stack, the saved choice, first stack error |
| `gramarye.scene.change(name)` / `push(name)` / `pop()` | deferred transitions (see Scene stack) |
| `gramarye.scene.current()` / `depth()` / `exists(name)` | active scene name / stack depth / is a scene mounted |
| `gramarye.systems.add(name)` | attach a C system (registered in app.c) to this scene |
| `gramarye.systems.enable(name, bool)` | toggle a system globally |
| `gramarye.input.key(name)` | resolve a key name to an id **once** (do it at scene top) |
| `gramarye.input.key_pressed(k)` / `key_down(k)` | `k` is an id from `input.key` or a name string |
| `gramarye.input.mouse_pressed([button])` / `mouse_down` / `mouse_released` | button 0 left (default), 1 right, 2 middle |
| `gramarye.input.wheel()` | mouse wheel movement this frame (positive = up) |
| `gramarye.input.mouse_pos()` / `touch_pos()` | returns `x, y` |
| `gramarye.draw.text(s, x, y, size[, r, g, b[, a]])` | prototyping helper |
| `gramarye.draw.rect(x, y, w, h[, r, g, b[, a]])` | prototyping helper |
| `gramarye.draw.clear(r, g, b)` | |
| `gramarye.time.total()` / `frames()` | accumulated sim time / sim tick count |
| `gramarye.entities.*`, `gramarye.textures.load` | sprite entities, see below |
| `gramarye.audio.*` | procedural sound synth, see below |
| `gramarye.tilemap.*` | layered tile maps: edit, save/load, draw through a camera, see below |

## Sprite entities (C-rendered)

The scalable render path: Lua **spawns and configures** entities; the C
`sprite_render` system iterates them densely (`ECS_storage_iterate`) and draws
under the `GlobalState` camera, below the UI. Lua touches an entity when
something changes — never per object per frame for static content.

```lua
local tex = gramarye.textures.load("textures/ship.png")  -- id, 0 on failure
local e = gramarye.entities.spawn()
gramarye.entities.set_transform(e, x, y [, rot_deg [, scale]])
gramarye.entities.set_sprite(e, tex, w, h [, r,g,b,a [, src_x,src_y,src_w,src_h]])
-- tex 0 = untextured: draws a w×h tinted quad (no assets needed)
gramarye.entities.despawn(e)
```

`set_transform`/`set_sprite` update in place when the component exists, so
per-frame moves from Lua do no ECS allocation. Entity handles are userdata,
comparable with `==`. See `scenes/splash.lua` for the demo.

## Procedural audio (`gramarye.audio`)

A voice is described by one flat table; the C synth renders it on the audio
thread. Any field left out takes its default (`gramarye.audio.defaults()`).
See [src/services/audio/README.md](../src/services/audio/README.md) for the
signal flow and every field.

```lua
local h = gramarye.audio.play {
    wave = "saw", freq = 110,                       -- sine|triangle|square|pulse|saw
    decay = 0.4, filter = "lp", cutoff = 600, filter_env = 3,
}
gramarye.audio.update(h, { ... })   -- retune a live voice (no retrigger)
gramarye.audio.release(h)           -- start its release stage
```

| Function | Notes |
|---|---|
| `audio.play(desc)` | returns a handle, or `nil` if the command queue is full |
| `audio.update(h, desc)` / `release(h)` / `stop_all()` | |
| `audio.set_master(g)` / `master()` | 0..1.5 |
| `audio.voices()` / `max_voices()` / `ready()` | `ready()` is false when no device opened |
| `audio.defaults()` | default desc table, a starting point to edit |
| `audio.WAVES` / `FILTERS` / `MAX_MODES` | name lists for UIs |
| `audio.SCOPE_KIND` / `SPECTRUM_KIND` | `custom` UI node kinds that draw the output |

Sound design lives in Lua: `lib/sfx.lua` holds materials (modal recipes),
presets and the footstep generator (`sfx.play_footstep(foot, surface,
{ weight, speed, pan })`). The `synth` scene (`a` from main) exposes every
parameter; **Log patch** prints the current desc as Lua to paste into presets.

Scenes using `widgets.Slider` must call `widgets.begin_frame()` at the top of
`on_draw` (Clay's `on_click` fires on release, so press edges are tracked there).

## Tilemaps (`gramarye.tilemap`)

Fixed-size, layered tile maps. The data, edits, file IO and drawing are C
(`src/services/tilemap`); Lua holds a map handle and calls a handful of
batched functions, never per-cell draws. The editor package uses it for the
tilemap editor and map viewer; a game scene uses the same calls to show a
map.

Conventions: cell `x, y` are 0-based positions (0, 0 is the top-left cell);
layers, tileset slots and terrain slots are 1-based list indices; a tile is
its 0-based index in the tileset image (`column + row * columns`). A layer is
either a tile layer (a cell is `tileset, tile`) or a terrain layer (a cell is
`terrain [, dir]`: a terrain slot and an optional compass direction, 0 =
north, 90 = east, that a flowing terrain follows); `value...` below means
whichever the layer takes, and 0 clears.

| Call | Notes |
|---|---|
| `tilemap.new(w, h, { tile = 16, levels = 8, layers = { { name = "level 0", kind = "terrain" }, { name = "level 1", kind = "terrain", elevation = 1 }, "detail" } })` | empty map, up to 1024 cells a side, `levels` (its z size) 1..32, up to 40 layers; a plain name is a tile layer at level 0 |
| `tilemap.load(name)` | `maps/<name>.lua` through the mount stack (a mod's copy wins): `map, package_id` or `nil, err` |
| `tilemap.list()` | `{ { name, package }, ... }` of saved maps, top-most package wins (desktop/web; Android can't list APK folders) |
| `map:add_tileset{ id, image, skin?, tile? \| tile_w?, tile_h? }` | returns the slot (`nil, err` on failure); same id returns the existing slot |
| `map:tilesets()` | `{ { id, image, skin, tile_w, tile_h, cols, rows, loaded }, ... }` |
| `map:add_terrain{ id, image?, color?, noise?, blades?, foam?, foam_speed?, foam_color?, waves?, flow?, sway?, sway_speed?, cliff?, stairs?, water?, solid? }` | returns the slot (`nil, err` on failure); same id updates that terrain in place |
| `map:terrains()` | `{ { id, image, color, swatch, noise, blades, foam, foam_speed, foam_color, waves, flow, sway, sway_speed, cliff, stairs, water, solid }, ... }` |
| `map:layer_kind(layer)` | `"tiles"` or `"terrain"` |
| `map:levels()` / `set_levels(n)` | the map's z size, 1..32: layers sit on levels 0 .. n - 1 |
| `map:layer_elevation(layer)` / `set_layer_elevation(layer, level)` | 0 .. levels - 1 (clamped); 0 is the ground |
| `map:add_layer(name, kind, level)` / `remove_layer(layer)` / `layer_empty(layer)` | append an empty `"tiles"` or `"terrain"` layer (returns it, or nil when there are 40) / remove one (the later ones move down; clears undo) / is every cell 0 |
| `map:elevation()` / `set_elevation(level_height, tilt [, slope_run])` | world px of height per level (1..32), the default camera tilt in degrees from straight down (0..90), and how far a one-level incline runs (world px, 1..6 tiles); clamped, saved with the map |
| `map:layer_height(layer)` | world px the layer stands above the board (`level * level_height`); pick on that plane to find its cells |
| `map:level_at(x, y [, z])` | `level, terrain`: the top floor at that cell (`-1, 0` when none), i.e. what something there stands on; with `z`, the highest floor at or below level `z` (a cave under a mountain) |
| `map:height_at(wx, wy [, z])` | `height, level`: the floor's height (world px) at a board point, lowered along inclines; put a sprite's `z` there. `z` as in `level_at` |
| `map:surface(x, y [, z])` | `level, terrain, solid, water, layer, depth`: the top floor at a cell (or the one at or below `z`), with its collision flags; `water` when there's water on it (`depth` levels of it) or it's open water |
| `map:water_at(x, y [, z])` | `depth, surface, deep`: water standing on floor `z` (default the top): levels of it (0 = dry), its surface's height (world px), and whether it's too deep to wade whatever the count (open water on level 0 has no bed) |
| `map:covered(x, y, z)` | is there a block above floor `z` of the cell: a character there is inside (a cave, under a bridge) |
| `map:flags(layer, x, y)` / `paint_flags(layer, x, y, w, h, "solid" \| "water", on)` | a terrain cell's collision flags (painted + its terrain's) / set or clear one on a rect's non-empty cells |
| `map:can_step(x0, y0, x1, y1 [, walker])` | `true, nil, level, cost`, or `false, why` (`"solid"`, `"water"`, `"cliff"`, `"void"`, `"low"`): can a walker step to that neighbor cell, the floor it lands on (a river's bed) and what the step costs. `walker = { solid, water, level, height, wade, wade_cost, swim_cost }`: what it can pass (`water`: it swims), the floor it's on (default the top), the levels of room it needs (default 1), the levels of water it wades (default 1) and what a step into wading / swimming water costs (default 2 each) |
| `map:reach(x, y, max [, walker])` | `{ { x, y, z, steps, cost, from_x, from_y, from_z }, ... }`: every floor a walker reaches for at most `max` (4-way, by `can_step`; up to 32), start first, cheapest first |
| `map:path(x0, y0, x1, y1 [, max = 32, walker])` | `{ { x, y, z }, ... }` start to end: the cheapest walk costing at most `max`, or nil; `walker.to_level` picks the floor at the end (default: the cheapest) |
| `map:arc(x0, y0, z0, x1, y1, z1 [, { peak = 12, obstacle = 0, samples = 32 }])` | `points` (flat `{ x, y, z, ... }`), plus `hx, hy, hz` where it dips into the ground (or an obstruction `obstacle` px tall); a parabola whose top is `peak` px above the higher end |
| `map:trace(x, y, z, vx, vy, vz [, { gravity = 600, dt = 1/60, max = 240 }])` | `points, landed`: something thrown or falling, until it comes down on the ground (the last point) or runs out of points |
| `map:raycast(ox, oy, oz, dx, dy, dz [, max [, max_level \| room]])` | `x, y, z` where a ray first meets the ground or a solid block (through water, to the bed), or nil (`cam:board_ray` gives the mouse's ray); blocks above `max_level` are skipped, or with `room = { x, y, level }` what an inside view of that room cuts away |
| `map:mesh_stats()` | `{ chunks, built, vertices, built_last_frame, build_ms, pending, far }`: what the levels' geometry costs; a frame builds stale chunks in view, nearest first, for about 8 ms (`pending`: what's left); `far`: the last frame was zoomed out into the far view (an overview texture on coarse meshes, no chunks built) |
| `map:get(layer, x, y)` / `set(layer, x, y, value...)` | `get` returns `tileset, tile`, or `terrain, dir` (`dir` nil when unset) |
| `map:fill(layer, x, y, w, h, value...)` / `fill_empty(...)` | rect fill (`fill_empty`: only into empty cells); returns cells changed |
| `map:stamp(layer, x, y, tileset, tx, ty, w, h)` | tile layers: copy a w x h block of the tileset (col `tx`, row `ty`); returns cells changed |
| `map:erase(layer, x, y [, w, h])` / `flood(layer, x, y, value...)` | rect erase (default one cell) / 4-way flood fill (0 erases the region); both return cells changed |
| `map:snapshot(layer [, layer...])` / `drop_snapshot()` / `undo()` / `redo()` | snapshot before each stroke (several layers make one step); undo/redo return the (first) layer or nil |
| `map:size()` / `tile()` / `layers()` / `set_layer_visible(i, b)` / `dirty()` / `name()` | `name()` is `name, package`, nil for a map never saved or loaded |
| `map:save(package_id, name)` | writes `<package>/maps/<name>.lua`; `true, path` or `nil, err` |
| `tilemap.view(map, opts)` | what the next `{ _type = "custom", kind = tilemap.VIEW_KIND }` node draws (`nil` clears) |
| `tilemap.pick(sx, sy [, height])` | board point at `height` under a screen point in the last drawn view (`lib/camera` does the same math without a frame) |
| `tilemap.palette(map, slot, { scale = 2, hover = { x, y }, select = { x, y, w, h } })` | what the next `PALETTE_KIND` node draws (`nil` clears): one tileset with grid, hover and selection. Size the node to `cols * tile_w * scale` by `rows * tile_h * scale` (from `tilesets()`) |

`view` options: `x, y, zoom` (the board point at the node's center, and
screen px per world px there), `rotation` (degrees clockwise), `tilt`
(degrees from straight down; default the map's), `fov` (perspective field of
view, default 30; 0 = orthographic), `supersample` (`true` or 2: render at
twice the size and filter down, which keeps pixel art smooth while the board
turns freely; the editor and map viewer use it), `grid`, `bounds`, `layer` + `dim` (fade
the other layers), `bg`, `cursor = { x, y, w, h, erase }`,
`ghost = { x, y, tileset, tx, ty, w, h }` (translucent stamp preview) or
`ghost = { x, y, w, h, terrain }` (terrain brush preview), `arrows` (flow
arrows on the active terrain layer), `flags` (collision overlay on it),
`marker = { x, y, level | z }`, `arcs = { { points, color, width } }` (3D
lines through the board, drawn as ribbons with their shadow on the ground,
faint where terrain hides them), `marks = { { x, y, level?, color } }`
(highlighted cells on their floor: `level`, default the top), `rings = { { x, y, z, r, color } }` (where things
land), the inside views `room = { x, y, level }`, `section`,
`see_through = { x, y, z, r }` and `cut_level` (see **Inside views** below) and
`sprites = { { image, x, y, level? | z?, w?, h?, src = { x, y, w, h }?, tint? }, ... }`
(upright sprites whose foot stands at board point `x, y` on `level`, or at
height `z`, e.g. `map:height_at(x, y)` to follow inclines; `image` is a
package path, loaded once). With `layer` set, the grid, cursor, ghost
and arrows sit on that layer's plane. Call it every frame before
`gramarye.ui.render`.

**The board.** The map is drawn as a board under a 3D camera (X = board x,
Y = height, Z = board y): it looks at `x, y`, turned and tilted, in
perspective by default, so the ground foreshortens as it tilts and panning
shifts what you see of the cliffs. Sprites are upright cards stretched by
1 / sin(tilt) so they show at full size, and the depth buffer sorts them
against cliffs: a plateau between a sprite and the camera hides it, a sprite
in front of a cliff covers it.

**Tilesets.** An image sliced into `tile`-sized pieces, drawn at one texel
per world unit (a 32 px tree on a 16 px grid covers 2x2 cells). With `skin`,
the image is an index map (red channel = palette index) drawn through
`shaders/<glsl>/skin.fs` with the skin as its palette, which is how the
foliage and cliff art is stored. The editor offers what `lib/tilesets`
returns (core's list; a mod wraps it with `require_base` to add its own).

**Terrain layers.** The ground without a tileset: each cell is one number
and a shader (`shaders/<glsl>/ground.fs`) draws the whole layer as one quad.
Each pixel blends the cells around it (Catmull-Rom over the nearest 4×4);
terrains stack in list order (water, sand, dirt, grass), and each one painted
on the four nearest cells shows where it holds more than half the blend (a
cell holds every terrain at or below its own). Lone cells come out round,
diagonals straight, and a one-cell diagonal path stays joined; a middle
terrain squeezed to a sliver (dirt between grass and water) gives way to the
one above it. Per terrain: `image` is a seamless fill
(all fills one size; one texel = one world pixel) or `color` a flat one,
`noise` (0..0.45 of a cell) makes edges wander, `blades` (px) hangs its lower
edges down a random length per column (toward the camera), and `foam` (px)
draws a `foam_color` band on it where a higher terrain meets it, washing in
and out at `foam_speed` by `waves` (0..1, default 1: the sea; a lake keeps it
low). `flow` (px/s) makes the surface drift along each cell's direction,
blended between cells (a two-phase flow map, so bends never smear), with
glints riding the current: a river. `sway` (px) leans its blades in a
travelling gust and ripples its fill half as far, with gusts sweeping across
as lighter bands, at `sway_speed`: grass in the wind (1.5), kelp in the
current (3, slow). In the editor, drag a river brush
downstream and each cell takes the drag's direction. The editor offers what
`lib/terrains` returns and refreshes a map's copy from it when it opens the
map.

**Levels.** A map is `width x height x levels` (its z size, up to 32), and
terrain is blocks: each level has a terrain layer, and a cell on level k is a
block one level tall standing from `(k - 1) * level_height` to
`k * level_height` world px; level 0 is the ground sheet. A block with no
block on it is a **floor**: what a character stands on. A block above an
empty level is a ceiling, so a cell can have several floors: a cave room is
the empty levels inside a mountain, with its floor below them; a bridge is a
row of blocks with empty levels under it. Where a floor's edge meets a floor
exactly one level lower, it's an incline: the ground ramps down a level over
`slope_run` px inside the edge (`map:height_at` gives the same heights, so
walkers can follow it). Under water too: a bank one level above the bed
beside it slopes down into the water, and the water's surface covers the
slope up to a waterline. Two or more levels lower, it's a cliff. The blocks
are real geometry: each level's tops are a mesh at the ground's height
everywhere (drawn by the same ground shader, so they blend and hang grass
blades over the lip, with a pale rim along the outline and inclines lit from
the north-west), and walls stand wherever the surface drops, following the
outline pixel for pixel (`shaders/<glsl>/wall.fs`), with a shadow at the
foot. A wall covers only its own level's band, and a block's top under
another block is never drawn, so a terrace flush with a plateau's edge makes
one cliff, not two walls in the same place. It looks right from any angle,
low ones included. Per terrain: `cliff` is the face color under it
(default: rock tinted toward the terrain), and a `stairs` terrain has square
cells: paint it on a raised edge and the stairs are cut into it, the ground
stepping down through the run of stairs cells toward the lower side (treads
with lit noses, stone risers, the level's own ground standing as walls either
side), for any drop; walkers climb them, and `map:height_at` ramps along
them. Water is a block too, but a see-through one: see **Water**. Cells keep
their logical position, which is what gameplay should use: a level-1 cell at
(x, y) is at (x, y), it just stands higher. Raised tile layers lie on their
level's plane. Version 1 maps (a raised layer was a column from the ground
up) load as blocks: each level takes the cell of the lowest old layer that
reached it.

**Water.** A `water` terrain is a block you see through and stand under. On
a raised level it's water over the block below it, the **bed** (paint
`riverbed` or `kelp` there): the floor is the bed, and the water's depth is
the levels of water on it (`map:water_at`). A river one level below its banks
is a riverbed on level k - 1 with water on level k (its banks slope down
into it, the water reaching up them to a foamy waterline). Water is drawn after
everything solid, blended by depth (a riverbed shows through 1 level, 3 levels
are nearly opaque; the lower the camera, the more it reflects), so a
character wading in it is under the surface from the waist down. Where it
drops off a ledge, its face is an animated waterfall. Water on level 0 has no
bed: it's open water, deep whatever happens. A ray (`map:raycast`), an arc or
a fall goes through water to the bed. `lib/water` (engine) says how a
character stands in it (`water.state`: dry, wading on the bed, or swimming
at the surface, and `water.stand_z`) and adds ripples and a splash
animation while it moves (`water.effects`).

**Inside views.** A camera above can't see a character in a cave, a
gallery or under a bridge: blocks over its floor hide it. `lib/inside`
(engine) opens the view up when `map:covered` says the character is inside.
The view's `room = { x, y, level }` (the character's cell and floor) cuts
away what's above the room it's in: the air around it under the roof is
flooded, each column of the room is cut one level above its own floor (so a
ledge three levels up inside the cave shows, and so does the floor below
it), and every other cell takes the cut of the nearest room column. Rock the
cut slices gets a dark cap, and where the cut steps between two solid cells a
dark section face closes the gap. Three ways to show it (the map viewer's
Inside button and V cycle them, the board demo too):

- **Cutaway** (`room`): the cut everywhere.
- **Section** (`room` and `section = true`): the cut, with what's under each
  cell's floor dimmed: a floor plan. On a ground floor it's the same.
- **See-through** (`room` and `see_through = { x, y, z, r }`): the cut only
  in a dithered circle `r` screen px around the character, in front of it;
  the rest of the map stays whole.

`cut_level = n` is the plain version: one level everywhere (the editor's Cut
above). `inside.apply(v, map, method, x, y, z, level)` sets the view fields
and returns what it did; `inside.raycast(map, cam, rect, mx, my, shown)`
picks against what's shown (`map:raycast` with the room), so a click lands
on the cave floor, not the mountain cut away above it.

**Collision.** Each terrain cell can carry two flags: obstruction (`solid`)
and `water`, painted with the editor's Flags tool or set by its terrain
(`solid = true`, `water = true` in `lib/terrains`; water, lake and river are
water). Repainting a cell's terrain keeps its flags; erasing it clears them.
`map:can_step(x0, y0, x1, y1, walker)` is the walking rule: a walker stands
on a floor of each cell (`walker.level`, default the top) and steps to the
neighbor's floor nearest its own: an obstruction stops it unless `walker`
has `solid = true` (a ghost); water up to `walker.wade` levels deep (default
1) it wades on the bed, a step in costing `wade_cost` (default 2); deeper
water (and open water, and a floor painted as water) needs `water = true` (a
swimmer; `swim_cost`), and a swimmer counts as being at the surface, so it
can push off a bank level with the water; one level up or down is an incline
and fine; two or more is a cliff unless the higher floor is stairs; and the
floor needs `walker.height` levels of room over it, water counting as room
(`"low"`: a tunnel too low for a tall body).

**Arcs.** Points in the air are board space too: `x, y` on the board, `z` =
height (what `map:height_at` gives). `map:arc` samples a jump or lob between
two points and reports where it hits terrain (a cave's ceiling included);
`map:trace` simulates something with a velocity under gravity (falls);
`map:raycast` finds what the mouse is over. `lib/arcs` (engine) builds the
tactics rules on them: a body `{ range, up, down, peak, lob, swim, phase,
obstacle, height, wade }` and `arcs.jump / throw / fall / reach` (landing in
water deeper than `wade` needs `swim`), each taking the
floor the character stands on and the one aimed at (by default the highest
it could jump up to); `arcs.level_of(map, x, y, z)` turns a board point (the
mouse's) into its floor level. The map viewer's Jump, Throw and Drop modes
show them, and draw the results with the view's `arcs`, `marks` and
`rings`.

**Grid movement.** Characters walk cell to cell, 4-way, each step by
`can_step`: 1, more into water. `map:reach` / `map:path` do the search (a
cheapest-first search, Dijkstra, over floors, `(x, y, level)`, in the window
the budget can reach, so map size doesn't matter); `lib/moves` (engine)
wraps them for a body (`move` = what it can spend in a turn, `height`,
`wade`, plus `swim` / `phase`; `moves.cost(map, path, body)` prices a walk):
`moves.range(map, cx, cy, body, left, z)` (the floors it can walk to, and a
lookup by `moves.key(x, y, z)`), `moves.path(map, cx, cy, tx, ty, body,
left, z, tz)` and `moves.points(map, path)` (board points along the walk
that follow inclines and stairs, for drawing and animating). The map viewer's Walk mode
is the model of a turn: a move budget, the reachable cells marked, the path
to the hovered cell, click to walk it, New turn to refill.

**Stress test.** `lib/stress_map` (editor) makes a big map (128 to 1024
cells a side, 16 levels) with every feature repeated across it (caves with
ledges and pools, shallow rivers, kelp lakes, plateaus, waterfall terraces, a
deep river with bridges) and its points of interest. `stress.job(size)`
builds it a slice at a time: call `job:step(seconds)` once a frame until it
returns true, then read `job.map` and `job.pois` (`job.progress` is 0..1
meanwhile). A 1024 map takes about 6 s, too long for one call (the Lua
watchdog stops a call after 3 s); `stress.build(size)` does it in one call
for small sizes. The map viewer's third bar generates one, jumps between the
points (God mode, G: a click puts the marker anywhere, walking is free) and
shows the frame rate and what the geometry costs (`map:mesh_stats`). The
tilemap editor's New > Generated makes the same maps to edit.

**Files.** A saved map is a Lua data file, run with an empty environment like
a manifest:

```lua
return {
    version = 3, width = 40, height = 24, tile = 16, levels = 8,
    level_height = 12, tilt = 40, slope_run = 24,
    tilesets = { { id = "trees", image = "textures/trees.png", tile_w = 16, tile_h = 16 }, ... },
    terrains = { { id = "water", color = { 52, 101, 164, 255 }, noise = 0.3, foam = 3, ... }, ... },
    layers   = { { name = "level 0", kind = "terrain", rows = { "1*30 2*10", ... } },
                 { name = "level 1", kind = "terrain", elevation = 1, rows = { "0 4*12 0*27", ... } },
                 { name = "detail", rows = { "0*5 10012 0*34", ... } }, ... },  -- a string per row
}
```

Each row string is runs separated by spaces: `"v*n"` is n cells of value
v, a lone `"v"` one cell. Runs keep big maps small (a 1024x1024x16 stress
map is 1.7 MB). Version 2 files have `cells = { ... }` (every cell a
number) instead of `rows`, and still load.

On a tile layer a cell is `0` (empty) or `tileset * 10000 + tile`, so
`20064` is tile 64 of tileset 2. On a terrain layer it's the terrain slot in
the low 8 bits plus, in the next 9, the direction + 1 (0 = none), plus
131072 for an obstruction flag and 262144 for a water flag: a river cell of
slot 3 flowing east (90) is `3 + 91 * 256 = 23299`.
Each map carries its own tileset and terrain lists, so changing
`lib/tilesets` or `lib/terrains` never breaks saved maps. The editor writes
into the package being edited (core, or the top mod); in dev builds that's
the source tree, so maps can be committed. Samples in `packages/core/maps/`:
`example.lua` (tile layers), `cliffs.lua` and `rivers.lua` (version 1
terrain maps with elevation, inclines, stairs, water and waterfalls) and
`caves.lua` (blocks: a mountain with a cave room, a tunnel and a gallery
above the cave, a bridge with a path under it, a shallow stream across the
plateau that falls off its edge).

**Camera** (`lib/camera` in the engine package): `camera.new { smoothing,
tilt, rotation, fov }`, `cam:follow(x, y, dt)` (frame-rate independent
easing), `cam.bounds` + `cam:clamp(rect)`, `cam:zoom_by(steps, rect, sx, sy)`
(toward a screen point; the steps are `camera.ZOOMS`, 1/16x to 8x, so a 1024
map fits on screen), `cam:set_zoom_index(i)`, `cam:fit(rect, w, h)`, `cam:rotate_by(deg)` / `cam:tilt_by(deg)`,
`cam:pan_screen(dx, dy, rect, sx, sy)` (the grabbed point stays under the
pointer), `cam:orbit_by(dturn, dtilt)` / `cam:orbit_to(turn, tilt)` /
`cam:snap_turn(step)` with `cam:ease_orbit(dt)` once a frame (turn and tilt
ease toward a target: smooth mouse orbiting; the scenes orbit on a held middle
button), `cam:board_ray(rect, sx, sy)` (for `map:raycast`),
`cam:screen_to_world(rect, sx, sy [, height])` and
`world_to_screen` for picking, `cam:unrotate(dx, dy)` (screen-relative
movement), `camera.rect_of(node_id)`, and `cam:view(opts)` to fill in the
camera for `tilemap.view`. It mirrors the C camera's math, so picking works
before the first frame. `board_demo.lua` (core) is a small game scene built
on all of it (grid moves, jumps, throws, inside views on the caves map); the
editor's `tilemap_view.lua` is the full test bed.

## Performance notes for scripts

- **Hoist stable closures and tables out of `on_draw`** — everything built
  there is garbage the collector must reclaim every frame. The Lua VM runs
  generational GC (set by the host), which makes this cheap but not free.
  See `main.lua` and `splash.lua` in `packages/README.md`.
- Resolve key names once with `gramarye.input.key("space")` at scene top.
- Big datasets stay in C; UI pulls visible rows by index (see `gramarye.demo`
  and the List widget in `main.lua`).

## Exposing C to Lua

Register systems by name in `src/app.c`:

```c
ScriptHost_register_system(host, "physics", physics_system_register(ecs, state));
```

then `gramarye.systems.add("physics")` from any scene's `on_enter`.

Add custom functions (e.g. entity bindings live in `entities_lua.c`):

```c
static int l_spawn_wave(lua_State *L) { /* ... */ return 0; }
ScriptHost_register_function(host, "game", "spawn_wave", l_spawn_wave);
-- lua: gramarye.game.spawn_wave()
```

C systems get the arena tiers through `GlobalState`: `frame_arena` (rewound
every render frame — per-frame scratch) and `scene_arena` (rewound on every
full scene change).

## Errors and hot reload

Script errors never crash the game: the failing hook is disabled, the error +
traceback goes to the log, and the screen shows the wrapped traceback. On
desktop, **F5** reloads the current scene file in place — the game-side module
cache is cleared too, so edits to `lib/*.lua` are picked up (embedded
`gramarye.*` modules are not reloaded; they ship in the binary). Dev builds
also **reload automatically**. The watcher polls the exact files that were
loaded (resolved paths, so it's always the source file and the mod override
that's actually in use), and reloads when one changes, is deleted, or
another package starts providing it (e.g. you add an override while
running). Each reload logs which package the scene came from. A call
from C into Lua that runs longer than 3 s (an infinite loop, runaway
recursion) is stopped by a watchdog and reported like any other error, so
long work has to be spread over frames (a coroutine stepped from
`on_update`, as `lib/stress_map`'s `job` does). Dev caveat:
scenes that call `gramarye.ui.load_texture` on enter (like splash's skin) will
register a fresh texture per reload — harmless during development.

## Genre C libraries

Heavy genre logic (platformer physics, tactics grids and pathfinding, turn
resolution) belongs in C, with a small Lua API on top. Follow the gramarye-ui
pattern:

```
libs/gramarye-tactics/
├── CMakeLists.txt                 # add_library(gramarye-tactics STATIC ...)
├── include/gramarye_tactics/
│   ├── tactics.h                  # the C API: grids, units, path queries
│   └── tactics_lua.h              # int luaopen_gramarye_tactics(lua_State *L);
├── src/
│   ├── tactics.c
│   └── tactics_lua.c              # builds and returns the module table
└── lua/gramarye/tactics.lua       # optional Lua-side helpers
```

Wire it into a game:

1. Add it to `project.json` / `libs/` like the other libs, `add_subdirectory`
   or `FetchContent` it in `CMakeLists.txt`, and append it to
   `GRAMARYE_NATIVE_LIBS` (the runtime links that list).
2. Register it in `apps/native_modules.h`:
   `{ "gramarye.tactics", luaopen_gramarye_tactics },`. Every executable
   shares the list, so the launcher knows which mods the game build accepts.
3. Lua gets it with `gramarye.require("gramarye.tactics")`.
4. Mods that need it declare `native = { "gramarye.tactics" }`. Builds
   without it skip the mod, and the launcher shows why.

API shape: Lua should hold **handles** and make **batched calls**
(`tactics.step(world)`, `tactics.path(world, from, to)`), never call per
entity per frame. Keep the data in C; Lua reads what the UI needs.
