# Packages

All game content lives in packages: folders with a `mod.lua` manifest and
any of `scripts/`, `textures/`, `fonts/`, `shaders/`, `skins/`. At startup the
built-ins for the running app, plus the mods the user picked, are mounted as
a stack. Every lookup (`gramarye.require`, scene loads, `gramarye.resolve`,
`gramarye.textures.load`) returns the file from the highest package that has
it.

| Package | Mounted in | Contents |
|---|---|---|
| `engine/` | launcher, game, editor | shared UI libs (`lib/ui`, `lib/widgets`, `lib/camera`), splash, pause, font, skin shader, UI atlas |
| `core/` | game, editor | the game: home menu, demo hub (`main`), planet, synth, skin demo, board demo, terrain/skin art, `lib/tilesets`, `lib/terrains`, `maps/` (`example`, `cliffs`, `rivers`) |
| `editor/` | editor | editor home, tilemap editor, map viewer, shader playground (its C side is dev builds only) |
| `launcher/` | launcher | mod picker + Play/Edit |

`index.lua` lists the built-ins in base order (web and Android can't list
directories). User mods live in `../mods/` (next to the executables in
release builds). The sample mods there show both kinds:

- `example_reskin`: ships `scripts/lib/widgets.lua`. It overrides the engine
  copy and wraps it with `gramarye.require_base("lib/widgets")`.
- `example_tc`: `replaces = { "com.gramarye.core" }` plus its own
  `home = { game = "tc_menu" }`. That's a total conversion.

Manifest fields are documented in `include/services/mounts/packages.h` and
`docs/SCRIPTING.md`.

`index.lua` lists the built-in ids lowest priority first; each is a folder
next to it with a `mod.lua`. The sample mods:

- `example_reskin/scripts/lib/widgets.lua` has the same path as
  `engine/scripts/lib/widgets.lua`, so every `gramarye.require("lib/widgets")`
  gets it; it loads the engine's version with `require_base` (the next mount
  down) and changes only what it needs. Enable it in the launcher, press
  Play, open "Demo hub": the header bar is purple.
- `example_tc` drops the core game package entirely and brings its own home
  scene; the engine's libs, splash and pause stay.

The sections below describe individual scripts. Paths are relative to the
package's `scripts/` folder. The `.lua` and shader files carry no comments;
this README holds what they do and why.

## lib

### ui.lua

Back-compat shim. The actual component library now ships *inside*
gramarye-ui (versioned together with the C ABI) as the embedded module
`gramarye.ui`. This file exists only so existing call sites that do
`gramarye.require("lib/ui")` keep working. New code should require the
embedded modules directly instead:

- `gramarye.require("gramarye.ui")` — the component library
- `gramarye.require("gramarye.theme")` — skins + image bindings

To reskin a game, override `theme.skins` / `theme.bind_image` rather than
forking this file.

### widgets.lua

Game-side composite widgets built on top of the `gramarye.ui` primitives.
This file deliberately lives in the game (not in gramarye-ui) because
things like inventory slots, item cards, trade rows, settings rows, and
header bars encode game/app domain concepts rather than generic UI. The
library ships genre-agnostic primitives; the game composes them here,
registers its own skins, binds its own data, and triggers its own ECS /
event systems from `on_click` / `on_hover` callbacks.

It's loaded from `packages/engine/` at runtime (unlike the embedded, version-pinned
`gramarye.ui` library layer), so on desktop it hot-reloads with F5 while
iterating on game UI.

Registers game-specific skins on top of the library's primitive skins
(`slot`, `badge`, `panel_header`, `tooltip`).

Widgets provided (each takes one props table):

- `HeaderBar` — title on the left, action buttons on the right (screen
  chrome); uses a flexible spacer to push actions to the right edge.
- `Slot` — inventory/equipment slot with hover and selection border states.
- `Badge` — small numeric badge (item count, notification, etc.).
- `ItemCard` — a `Slot` plus a label underneath.
- `TradeRow` — icon + name/description on the left, value + action on the
  right.
- `OptionRow` — option toggle row for a settings screen.
- `Checkbox` — a toggle box + label; the whole row is clickable. `checked`
  is controlled by the caller (flip your own state in `on_click`), matching
  how the other widgets stay stateless.
- `Slider { id, label, value, min, max, log, step, fmt = "%.2f", on_change,
  on_commit }` — `on_change` fires while dragging, `on_commit(value)` once
  when the drag ends; `log` maps the track exponentially (frequencies,
  times) and needs `min > 0`. Clay's `on_click` fires on release, so press
  edges are tracked by the module: call `widgets.begin_frame()` once at the
  top of `on_draw` in any scene that uses a Slider.
- `Chip` — a compact clickable pill; `selected` highlights it.
- `Segmented { id, items = { "a", "b" }, selected, on_select = fn(i, item) }`.

### camera.lua (engine)

Camera rig for anything drawn with `gramarye.tilemap.view` (the tilemap
editor, the map viewer, a game scene): a 3D camera looking at board point
`x, y`, turned `rotation` degrees clockwise and tilted `tilt` degrees from
straight down, `zoom` screen px per world px there, in perspective when
`fov > 0` (panning shifts what you see of the cliffs) and orthographic at 0.
Its projection mirrors `view_camera` / `view_ray` in `tilemap.c`: keep the
two in step. API in `docs/SCRIPTING.md`.

```lua
local camera = gramarye.require("lib/camera")
local cam = camera.new { smoothing = 8, tilt = 40 }
cam.bounds = { 0, 0, map_w, map_h }          -- optional: keep the view inside
-- each frame:
local rect = camera.rect_of("my_viewport")   -- the custom node's screen rect (last frame's layout)
cam:follow(player.x, player.y, dt)
cam:ease_orbit(dt)                           -- turn and tilt catch up with orbit_by
cam:clamp(rect)
gramarye.tilemap.view(map, cam:view { marker = player })
```

- `ZOOMS` = 1/16 to 8. Whole-number steps keep pixel art crisp; the ones
  in between and below 1 are for overviews (the smallest fit a 1024 map).
  `FOV` 30, `MAX_TILT` 85, `ORBIT_RATE` 18 per second.
- `rotate_by` / `tilt_by` (or assigning `rotation` / `tilt`) jump there now;
  `orbit_by` / `orbit_to` move a target that `ease_orbit` eases toward once
  a frame, frame-rate independent, the short way round, landing exactly.
  A rotation or tilt set directly since the last frame becomes the new
  target. `snap_turn(step)` rounds the turn target to a multiple of `step`
  (90: quarter turns).
- `zoom_by(steps, rect, sx, sy)` keeps the board point under the screen
  point put; `fit` picks the biggest step at which an area fits;
  `pan_screen` keeps the grabbed board point under the pointer.
- `follow` eases frame-rate independently: the same smoothing feels the
  same at 30 and 144 fps (0 snaps). `clamp` keeps the center inside
  `bounds` and centers a map that fits, measured flat and unturned (a
  tilted or turned view can show a little past the edges).
- `screen_to_world(rect, sx, sy, height)` picks on the plane at `height`
  (a raised layer: `map:layer_height(layer)`); `board_ray` gives the mouse
  ray in board space for `map:raycast`; `unrotate` turns a screen vector
  into a board direction by the turn alone (screen-relative controls).

### moves.lua (engine)

Grid movement for a tactics game: `range` (floors a body can walk to with the
moves it has left), `path` (the shortest walk to one) and `points` (the walk
as board points that follow inclines and stairs). A character stands on one
floor of a cell (`x, y, z`: a cave under a mountain is a lower floor of the
same cells). Built on `map:reach` / `map:path`, so it walks by
`map:can_step`: inclines and stairs fine, cliffs not, obstructions only with
`phase`, and `body.height` levels of room overhead. A step costs 1; into
water up to `body.wade` levels deep it wades on the bed for `wade_cost`;
deeper water needs `swim` (`swim_cost` a step). The move budget is spent in
cost, so a river slows a walker down. A body is `lib/arcs`'s.

```lua
local moves = gramarye.require("lib/moves")
local area = moves.range(map, hero.cx, hero.cy, body, moves_left, hero.z)
local way  = moves.path(map, hero.cx, hero.cy, cx, cy, body, moves_left, hero.z, z)
if way then animate along moves.points(map, way) end
```

- `range(map, cx, cy, body, left, z)`: `{ { x, y, z, steps, cost }, ... }`
  without the start, plus a lookup by `moves.key(x, y, z)`. `left`
  defaults to `body.move`, `z` to the top floor.
- `path(map, cx, cy, tx, ty, body, left, z, tz)`: `{ { x, y, z }, ... }`
  start to end, or nil; `tz` nil = the cheapest floor there.
- `cost(map, path, body)`: what a walk costs, step by step.
- `points(map, path)`: each cell's center on its floor and the edge between
  cells (just past it), so the walk follows inclines and stairs.
- `walker(body, z, to_z)`: the walker table `map:reach` / `map:path` /
  `map:can_step` take (pass flags, the floor it starts on, the floor it
  wants, nil = the cheapest, its height in levels, how it does in water).

### inside.lua (engine)

Seeing a character who's inside (a cave, a gallery, under a bridge): when
`map:covered` says there's a block over its floor, `apply` opens the view
by one of three methods (cutaway, section, see-through) and `raycast` picks
against what's shown. The map viewer and the board demo switch between them.
The technique is in `docs/Notes/Rendering/Inside Views.md`.

```lua
local inside = gramarye.require("lib/inside")
local v = cam:view { ... }
local shown = inside.apply(v, map, "cutaway", x, y, z, level)
local hx, hy, hz = inside.raycast(map, cam, rect, mx, my, shown)
```

- `METHODS` = `off`, `cutaway`, `section`, `see` (`LABELS` names them);
  `next(method)` cycles them for a button or key. `RADIUS` = 140 screen px
  is the see-through circle.
- `is_inside(map, x, y, level)`: is a character on that floor at board
  point `x, y` under a block.
- `apply(v, map, method, x, y, z, level)` sets the view's `room` (and
  `section` or `see_through`) and returns what it did,
  `{ method, level, room, x, y, z }`, or nil when the character isn't inside.
- `raycast(map, cam, rect, mx, my, shown)`: the board point under the mouse
  against what the view shows; blocks the cut takes away aren't there to
  hit (with see-through, only inside its circle).

### water.lua (engine)

Characters in water, the look of it: `state` says whether a character on a
floor is dry, wading on the bed (water up to `body.wade` levels, up to its
waist, the water drawing over it) or swimming at the surface (head and
shoulders out); `stand_z` where to draw it; `effects` adds ripples
around it and, while it moves, the splash animation
(`textures/fx/splash.png`, drawn by `dev/make_textures.py`). The rules (who
wades, what a step costs) are `map:can_step`'s, through `lib/moves`.

```lua
local water = gramarye.require("lib/water")
local st = water.state(map, x, y, level, body)
v.marker = { x = x, y = y, z = water.stand_z(st, z, map) }
water.effects(v, st, x, y, clock, moving)
```

- `state(...)`: `{ mode = "dry" | "wade" | "swim", depth (levels),
  surface (px height of the water), deep }`.
- `stand_z(st, z, map)`: where to stand a character whose floor is at
  height `z`: on the bed when dry or wading, at the surface with the feet
  `SWIM_SINK` (0.7) of a level under when swimming (`map` gives the level
  height; 12 without one).
- `effects(v, st, x, y, clock, moving)`: ripples (`v.rings`) every
  `RIPPLE_PERIOD` (1.4 s), spreading `RIPPLE_REACH` (9 px); the splash
  (`v.sprites`) at the feet, a touch behind the character so it doesn't
  hide it, its droplets reaching about the knees. `clock` is a running time
  in seconds. `SPLASH` describes the sheet (6 frames of 24x20 at 14 fps)
  and must match what `make_textures.py` draws.

### arcs.lua (engine)

Jumps, throws and falls for a tactics game, on top of `map:arc`,
`map:trace` and `map:surface`. Cells are 0-based `(x, y)`; points are board
space (`x, y` in world px, `z` the height above the board, as
`map:height_at` gives it). A cell can have several floors, so each function
takes the floor the character stands on (`z`, nil = the top) and the one
aimed at (`tz`, nil = the highest the body could jump up to, else the top);
`level_of(map, x, y, z)` gives the floor of a board point (the highest
floor of its cell at or below the level its height falls in).

A **body** is what a character can do (`arcs.body { ... }` fills in
`DEFAULT`):

| Field | Default | Meaning |
|---|---|---|
| `move` | 4 | cells it can walk in a turn (`lib/moves`) |
| `range` | 3 | cells it can jump or throw, center to center |
| `up` / `down` | 1 / 2 | levels it can jump up / drop safely |
| `peak` / `lob` | 10 / 14 | px its jump rises above the higher end; a throw adds `lob` |
| `swim` / `phase` | false | it may land in water / on obstructions |
| `obstacle` | 12 | px an obstruction stands in an arc's way (0: they don't) |
| `height` | 2 | levels of room it needs overhead (`lib/moves`) |
| `wade`, `wade_cost`, `swim_cost` | 1, 2, 2 | levels of water it walks through on the bed; step costs |

```lua
local arcs = gramarye.require("lib/arcs")
local body = arcs.body { range = 4, up = 2 }
local j = arcs.jump(map, hero.x, hero.y, cx, cy, body)
if j.ok then -- animate along j.points
else print(j.why) end
```

- `jump(map, ax, ay, cx, cy, body, z, tz)` from the feet at board point
  `ax, ay`: `{ ok, why, points, from, to, level, hit, up, down, distance }`;
  `level` is the floor it lands on, `why` is `"out of range"`, `"too high"`,
  `"too far down"`, `"nothing to land on"`, `"obstruction"`, `"water"`
  (deeper than `wade` without `swim`) or `"blocked"` (the arc hits terrain).
- `throw(...)` leaves from the hand, `HAND` (10) px above the feet, and
  stops where the arc hits: `{ ok (reaches the target, in range), why,
  points, land = { x, y, z }, distance }`.
- `fall(map, ax, ay, vx, vy, body, z)` walks the way it's moving to the
  nearest edge within a cell (where the ground drops by more than a step)
  and steps off, under `GRAVITY` (600 px/s²): `{ points, land, level, drop
  (levels fallen), ok (within body.down), why }`.
- `reach(map, ax, ay, body, z)`: every cell a jump could land on within
  range, `{ { x, y, z, ok, why }, ... }`.
- `cell_point(map, cx, cy, level)`: a cell's center on its floor (nil: the
  top) as `x, y, z`; `too_deep(map, cx, cy, level, body)`: water on that
  floor the body can't wade (open water, or more than `body.wade` levels).

### tilesets.lua (core)

The tilesets the tilemap editor offers. Each is an image sliced into
`tile x tile` pieces (one texel = one world unit when drawn). A `skin`
marks an index map: its red channel picks a color from the skin palette, so
the same art can be recolored (see the `*.skin.*.png` variants and
`textures/terrain/terrain_atlas.json` for how the sheets are laid out).
Saved maps keep their own copy of each tileset entry, so editing this list
never breaks a map. A mod adds tilesets without copying the file:

```lua
local base = gramarye.require_base("lib/tilesets")
local list = { table.unpack(base) }
list[#list + 1] = { id = "castle", name = "Castle", image = "textures/castle.png", tile = 16 }
return list
```

### terrains.lua (core)

The ground terrains for terrain layers, in stacking order (later draws over
earlier): water, lake and river (see-through blocks: water over a bed; calm,
or flowing along each cell's direction), sand, riverbed and kelp (fills
from `textures/terrain/`, drawn by `dev/make_textures.py`; kelp's fronds sway
in the current), dirt, rock, grass (its blades sway in the wind), stairs. Each is a fill image or flat color plus edge
settings, a `cliff` color for its face on a raised level, `stairs` for the
terrain that cuts steps into a cliff, and `water` for the ones that fall as
waterfalls onto water below. Saved maps keep a copy, refreshed by the
editor when it opens a map, so tweaks here show up after the map is saved
again. A cell is one number; there are no transition tiles to draw: each
terrain draws over the ones before it wherever it holds more than half of
the cells around a pixel (`docs/Notes/Rendering/Terrain Blending Shader.md`).

| Field | Meaning |
|---|---|
| `id` | saved in maps: keep it stable |
| `name` | shown in the editor |
| `image` | seamless fill texture; make all fills one size (the first one found sets it); one texel = one world pixel |
| `color` | `{ r, g, b }` flat fill when there's no image |
| `noise` | how far edges wander, 0..0.45 of a cell (default 0.2) |
| `blades` | px its lower edges hang down, random per column |
| `foam`, `foam_speed`, `foam_color` | px band drawn on it where a higher terrain meets it; how fast it washes in and out (default 1.5, 0 = still); `{ r, g, b, a }` |
| `waves` | how much the foam band washes, 0..1 (default 1; lakes and rivers keep it low) |
| `flow` | px/s the surface drifts along each cell's direction (rivers); the editor sets a cell's direction from the brush stroke |
| `water` | a see-through block: water over the bed under it; walkers stand on the bed, wading where it's shallow; drawn after everything else, the bed showing through by depth; open water on level 0 has no bed (swim); where it drops off a ledge its face is a waterfall |
| `sway`, `sway_speed` | px its blade tips lean in a travelling gust (its fill drifts half as far): grass in the wind, kelp in the current; how fast (1: a breeze) |
| `solid` | every cell is an obstruction (single cells can be flagged with the editor's Flags tool) |
| `cliff` | `{ r, g, b }` face color under it on a raised level (default: earthy rock tinted toward its color); for water, the bank where it doesn't fall |
| `stairs` | square cells (no blend): painted on a raised edge, one or more cells deep, it cuts a staircase down to the ground in front, and walkers climb the cliff there |

### stress_map.lua (editor)

Builds a big map for stress tests (128 to 1024 cells a side, 16 levels):
rolling land with every feature repeated across it in sites (caves with a
ledge, stairs and a wading pool; shallow rivers with kelp; kelp lakes deep in
the middle; plateaus with stairs; waterfall terraces) and a deep river with
bridges across the middle. Gives the map and its points of interest, which
the map viewer's God mode teleports between and the editor's Go to steps
through. `stress.job(size)` builds it a slice a frame (a coroutine that
yields per row; a 1024 map is ~6 s, past the Lua watchdog for one call);
`stress.build(size)` in one call, for small sizes (fine up to 512 a side).

```lua
local stress = gramarye.require("lib/stress_map")
local map, pois = stress.build(256)
-- pois: { { name = "Cave 1: ledge", x = 40, y = 52, level = 7 }, ... }

local job = stress.job(1024, { levels = 32 })
-- each frame: local done, progress = job:step(0.03)   -- about 30 ms of work
-- when done: job.map, job.pois
```

The map is split into `SITE x SITE` (48) cell sites, the examples taking
turns: a **cave** (a mountain with a room inside: a floor, a ledge 3 levels
up with stairs, a sunken wading pool, a tunnel out of the south face), a
**river** (1 level deep over a riverbed, kelp in it), a **lake** (shallow at
the shore, 3 deep in the middle, kelp), a **plateau** (stairs and a
terrace) and **falls** (a stream stepping down three terraces). The middle
row is the deep river: 5 wide, 3 levels deep with its surface on level
`RIVER_LEVEL` (5), banks level with the surface, kelp on its bed, and a
bridge every 3 sites, its deck a level above the water with ramps up from
the banks. The land rolls on levels 4..7 from 2-octave value noise
(deterministic). `opts`: `levels` (default 16, 16..32), `tile` (16).
Everything is written straight into the map: the land a row at a time, one
`map:fill` per run of equal cells per level; rectangles as one fill per
level; the odd shapes cell by cell. Nothing map-sized is kept in Lua, so
1024 x 1024 fits.

### editor_ui.lua, tilemap_session.lua (editor)

`editor_ui`: compact flat button skins shared by the editor scenes (`_on`
marks the active tool, layer or tileset; they have their own skin names, so
they stay flat even after splash binds an image to `button`), a short
package name (`"com.gramarye.core"` -> `"core"`), and a back action (pop a
pushed scene, or go home when this scene was the first). `tilemap_session`:
the map open in the editor (the handle, its saved name and package), kept
across leaving and re-entering the editor and shown live by the map viewer,
unsaved edits included. It's module state, so it resets when scripts reload
(F5 or the dev file watcher): save before editing scripts.

### sfx.lua (core)

The sound design layer over `gramarye.audio`. C only understands a flat
voice desc (`include/services/audio/synth.h`); everything here is plain Lua
data that turns game context into descs. Edit it, then F5 to hear changes.
The technique is in `docs/Notes/Engine/Procedural Audio Synth.md`.

- `patch(overrides)`: the engine defaults with overrides applied; every desc
  in the file goes through it. `serialize(desc)` turns a desc into Lua
  source, for pasting a patch tuned in the synth lab into `presets`.
- `materials`: modal recipes, partial frequency ratios over a base pitch
  (a free-free bar, a bell's hum, prime, tierce..., a circular membrane).
  The ratios make a material recognizable; inharmonic (non-integer) ratios
  sound metallic. `modes(name, pitch, decay)` scales every partial by
  `pitch` and the ring time by `decay`.
- `presets`: single-voice patches showing what each building block does.
- Footsteps are an exciter (the foot) times a resonator (the surface), then
  weight and speed transform both. A foot's `attack` is contact softness,
  `burst` the impact length, `color` 0 bright .. 1 dull, `hardness` the high
  end and resonator drive, `damp` how much it lets the surface ring.
  `footstep(foot, surface, opts)` returns the layer descs for one step
  (`opts`: `weight`, `speed` 0..1, `pan` -1..1): heavier is lower, fast or
  heavy is louder, and a slow walk plays heel then toe where a run strikes
  once. `feet` and `surfaces` are the tables it combines; `play_footstep`
  plays a step and `play_layers` any list of descs. `copy` deep-copies a
  table.

## shaders

`engine/shaders/glsl330/` (desktop) and `glsl100/` (web, Android; generated
by `dev/glsl100.py`, never edited by hand). The tilemap loads `ground`,
`wall` and `sprite`; the skinned sprite system loads `skin`. The techniques
are in `docs/Notes/Rendering/` (Terrain Blending Shader, Raised Terrain
Geometry, Water, Inside Views, Sprites, UV Remap Skinning); this is the
interface.

### ground.fs

Draws a terrain layer with no transition tiles: the ground level as one flat
quad, a raised level's top as a mesh at its heights. Either way
`fragTexCoord` is the board position over the map, so each pixel works out
its world position and everything else from it. Positions snap to whole
world pixels (one texel = one world unit), so edges stay crisp at any zoom.

| Uniform | Meaning |
|---|---|
| `texture0` | one texel per cell: r = terrain slot (0 = empty, 1..N in stacking order), g = direction or stairs geometry, b = flags, a = incline distance code (layout: Packing Data into Bits and Texels) |
| `terrainFills` | every terrain's fill texture side by side, in slot order; `fillSize` px each |
| `mapSize`, `tileSize` | in cells; world px per cell |
| `terrainCount`, `terrainParams[]` | per terrain: noise, blades px, foam px, foam speed |
| `terrainFoam[]`, `terrainSwatch[]` | foam color; average color (what fine detail fades to far away) |
| `terrainExtra[]` | x = 1: square cells (stairs), y = waves, z = flow px/s |
| `terrainSway[]` | x = px blade tips sway (the fill half as far), y = speed |
| `time` | seconds, wrapped hourly by the C side |
| `outputMode` | 0 color; 1 the winning slot in red (+64 where water falls off this layer's edge onto water below): the footprint the tilemap reads back; 2 a drop shadow (white splash where water falls) |
| `viewDown` | screen-down in world units: the way blades hang |
| `raisedInfo` | a raised top: top height, level height, incline run, 1 (w = 1 turns on slope light, stair noses and the rim) |
| `coverMask`, `coverRect` | per pixel, what's on the level above (128 a solid block, 64 water, low 6 bits a water depth 1..31 or 32 + a bank's depth under the surface), over that world rect (w = 0: none) |
| `waterPass` | 0: solid terrain only; 1: water surfaces only (the blended pass) |
| `viewCos` | cos of the camera's tilt (1 = straight down): water reflects more at a slant |
| `cutMap`, `cutInfo`, `seeThrough` | the inside view: per cell the highest level shown (255 all); x 0 none, 1 everywhere, 2 in the circle, y 1 = section; the circle's window x, y, radius px and depth |

Tuning constants: `SADDLE` (0.2, how far past one half a diagonal crossing
is pushed), `THIN_PX` (2, a middle terrain thinner than this is dropped),
`EDGE_SLOPE` (1.25, weight change per cell across a straight edge),
`FLOW_PERIOD` (3 s before a flow phase snaps back), `STEP_PX` (4, must match
`tilemap.c`), `SHADOW_ALPHA` (0.3), `SHORE_FOAM` (1.6 px of water over a
bank still in the waterline's foam). The hashes are Dave Hoskins' "hash
without sine" (stable across GPUs) and dithering uses interleaved gradient
noise.

### wall.vs, wall.fs

The vertical faces of the levels: cliffs and banks along the outline, stair
risers, the walls beside a staircase, the low step at an incline's foot,
waterfalls, an inside view's section faces. Everything a pixel needs comes
with the geometry:

| Vertex input | Meaning |
|---|---|
| `vertexPosition` | 3D: x = board x, y = height, z = board y |
| `vertexTexCoord` | the whole wall's top and foot height at this end (a quad may be one level's band of a taller wall, shaded as one face) |
| `vertexNormal` | the way the wall faces, on the board (y = 0) |
| `vertexColor` | r = terrain slot; g = style flags: 1 water falls here, 2 a stair riser, 4 an incline's step, 8 a section face |

Colors are k/255 exactly, so the fragment shader gets the integers back with
`floor(x * 255 + 0.5)`. Uniforms: `cliffColor[]` (face color per slot; a = 1:
stairs), `swatchColor[]` (each terrain's own color, for waterfalls and
incline steps), `time`, `mapSize`, `tileSize`, `levelHeight`, `waterPass`
(1: these are water's faces, drawn see-through) and the inside view's
`cutMap`, `cutInfo`, `seeThrough` as in `ground.fs`. A wall pixel looks the
cut up for the cell behind it; section faces only show where the cut
applies. Detail is keyed to the board pixel half a pixel behind the face, so
it stays put while the camera turns; light comes from the board's
north-west, like inclines.

### sprite.fs

Sprites standing on the board: alpha test instead of blending, so a
sprite's transparent pixels write no depth and don't hide what's behind
them.

### skin.fs

UV-remap skinning. `texture0` (the animation frame, bound by
`DrawTexturePro`) holds baked coordinates instead of color: RG = skin-space
pixel position / 255, exact pixel coordinates; A = the silhouette mask.
`texture1` is the skin (the full-detail art), `skinTexelSize` = 1 / its size
in pixels, `colDiffuse` the sprite tint. See `tools/skin_baker` for how
frames are baked and `include/components/skinned_sprite.h` for the
component.

## scenes

### tilemap_editor.lua (editor)

Paint a fixed-size map, saved into a package as `maps/<name>.lua`. Tile layers paint from the tileset palette (bottom
panel, resizable; drag in it to select a block such as a 2x2 tree). Terrain
layers paint one terrain number per cell from `lib/terrains`, and the ground
shader draws the blended edges; a river brush takes its flow direction from
the drag. Tools: brush, erase, fill, pick, and Flags (obstruction / water for
collision). A map is width x height x levels (New sets all three, up to
1024 x 1024 and 32 levels, or makes a Generated stress map behind a progress
popup, with Go to > stepping through its examples; the bottom panel adds or
drops levels): terrain is blocks, a terrain
layer per level, and you pick the level you paint with the level picker
(PageUp / PageDown). The Column brush fills the empty levels under a block
(a click makes a hill) and erases everything above; the Block brush paints
one level, for hollowing caves and laying bridges; Cut above hides the
levels over the one you're painting. With water selected, the Column brush
carves a river: a water depth (1 wades, 2-3 needs a bridge) down from the
active level, a bed under it (riverbed, kelp or keep) and the levels above
cleared, so the water is open to the sky; "As painted" stacks a column of
water instead. A new map has a terrain layer per level ("level 0" is the
ground), then the tile layers `detail` and `objects` on top. The brush goes from 1 to 32 cells. The status line shows the hovered
cell's floor and water (wade / deep), and the chunks built or "overview"
when zoomed out. The bottom panel also sets the height
per level, the incline run and the default tilt. Layers hide and "dim
others". Undo/redo per stroke, a column stroke being one step. Holding the middle mouse button turns and tilts the
view smoothly (Shift + middle-drag pans); `,` `.` turn it too and `/` turns it back. New (size presets), Open (saved maps from every mounted package),
Save / Save as into a game package: game content (core or a mod), never
the shared engine package (the launcher mounts it too) or the editor-only
one; the default is the top mod, else core. The open map lives in
`lib/tilemap_session`, so leaving the editor keeps it and View > shows it
with unsaved edits.

| Input | Does |
|---|---|
| Left / right mouse | use the tool / erase (right-drag clears with the Flags tool) |
| Middle drag | turn and tilt the view, smoothly; Shift + middle drag or Space + left drag pans |
| Wheel | zoom toward the cursor |
| Alt + click | pick |
| `B` `E` `F` `I` `C` | brush, erase, fill, pick, collision flags |
| `G` | grid |
| `1`, `2`-`9` | the terrain layer, tile layers |
| PageUp / PageDown | terrain level |
| `[` `]` | previous / next tileset or terrain |
| `-` `=` | zoom (on a terrain layer's palette, `-` / `+` set the brush size) |
| WASD / arrows | pan |
| `,` `.` (hold) / `/` | rotate the view / turn it back |
| Ctrl+Z / Ctrl+Y, Ctrl+S, Esc | undo / redo, save, close |

Palette: on a tile layer, click a tile or drag to select a block (a 2x2
tree). During a stroke, stamps land on a grid anchored where the stroke
began, so dragging a 2x2 block tiles it instead of overlapping itself; a
terrain brush stays centered on the cursor. Fast drags fill the cells
between frames, and a river brush takes its direction once the drag has
moved 4 world px (its first stamp is redone then). Pick takes the block on
the active level (water included), else the floor at or below it. A tile
layer has a level of its own, set in the bottom panel; terrain layers are
their level's. Older maps can have extra terrain layers on a level; they
show as their own layer buttons. The bottom panel's height is a fraction of
the screen (drag its top edge, `v` / `^` minimizes it). The techniques are in
`docs/Notes/Editor/Tilemap Editor Techniques.md`.

### tilemap_view.lua (editor)

Loads a saved map, or the editor's map live, and shows it through
`lib/camera`. Follow mode has a marker that's a character on the grid, with
an adjustable body and a turn's worth of moves: Walk marks the cells it can
reach, draws the path to the hovered one and walks it on click (WASD steps
one cell); cliffs, water, obstructions and tunnels too low for its height
are in the way (Swim / Phase lift water and obstructions). Jump / Throw /
Drop draw the arc to the cell under the mouse (with the cells a jump can
reach) and do it on click. New turn refills the moves; water slows it (a
wading step costs 2). Inside (V) picks how the view opens up when the marker
is under a block: Cutaway, Section, See-through or Off. The third bar is the
stress test: generate a big map (`lib/stress_map`), teleport between its
points of interest (God mode, G: a click puts the marker anywhere), and read
the frame rate, chunks built and pending, vertices and build time. Free mode
pans. `board_demo.lua` (core) is the game-side version.

| Input | Does |
|---|---|
| `1`-`4` | Walk, Jump, Throw, Drop |
| `N` | new turn |
| `V` | inside view method |
| `G` | God mode |
| `F` | Follow / Free camera |
| Wheel or `-` `=` | zoom |
| `[` `]` | tilt by 5 degrees |
| Middle drag | turn (sideways) and tilt (up and down); the drag moves the camera's orbit targets and `ease_orbit` catches up |
| `R` / `P` | reset turn and tilt / perspective or flat |
| WASD | Follow: one cell in the screen direction pressed (nearest board axis); Free: pan |
| Esc | back |

Walk marks every cell reachable with the moves left and draws the path to
the one under the mouse; Jump shows the arc (green when it can land) and
every cell it can reach; Throw the lob toward the mouse, stopping where it
hits; Drop walks off the nearest edge toward the mouse and falls. The cell
under the mouse comes from the mouse ray against what the view shows (with
an inside view, the cut blocks aren't there). Reach marks are cached and
recomputed when the marker's cell, the moves left or the body change. A
generated stress map is built a slice a frame (`stress.job`), since all at
once would freeze the app and trip the Lua watchdog.

### board_demo.lua (core)

A small game scene on `maps/caves.lua`: one character with a turn's worth of
moves on the grid (`lib/moves`), plus jumps and throws in arcs (`lib/arcs`), under
the tilting perspective camera (`lib/camera`). It starts outside a tunnel
into a mountain; walking into the cave room (or the gallery above it, from
the bridge) cuts the mountain away (`lib/inside`; V switches the method). The board turns in animated
quarter turns (a middle-drag turns and tilts it, then settles on the nearest quarter), the
way pixel-art tactics games keep their art square to the screen; the editor
and map viewer turn freely and render supersampled instead. Reached from the game
menu and the demo hub (it's pushed, so Esc pops back). It shows how a game
scene uses the tilemap features; the editor's map viewer is the full test bed.

The map is blocks on levels: a plateau with stairs and a shallow stream
across it (a step in costs 2; it falls off the plateau's edge), a mountain
with a cave room and a tunnel inside, a gallery above the cave that a
bridge leads into, and a path under the bridge. Wading, the character
ripples and splashes (`lib/water`).

| Input | Does |
|---|---|
| `1`-`3` | Walk (reachable floors marked; click one), Jump (up 1 level, down 2), Throw (the lob toward the mouse) |
| `N` | new turn |
| `V` | Cutaway, Section, See-through, Off |
| `[` `]`, wheel | tilt, zoom |
| Middle drag | turn and tilt; on release the board settles on the nearest quarter turn |
| WASD | one cell in the screen direction pressed, mapped to the nearest board axis |
| Esc | back |

Pixel art looks its best square to the screen, so the board only shows
in-between angles while it moves.

### synth.lua (core)

The synth lab: every `gramarye.audio` feature behind a slider. The Synth tab
edits one patch (oscillator, noise, envelope, filter, output), the
Resonator tab the modal bank the patch excites, and the Footsteps tab runs
`lib/sfx`'s footstep generator (any layer can be opened in Synth). Space
plays, `L` toggles live mode (a held voice follows the sliders, kept
audible even for one-shot patches; otherwise an edit retriggers the note),
Esc goes back. Labels use `•`, which is in the baked font, not `·`.

### game_main_menu.lua (core), editor_main_menu.lua (editor), launcher.lua (launcher)

The home scenes of the three apps, named by each package's manifest
(`home = { game = ... }`); splash and pause's "Quit to Menu" come back
here. The game menu's subtitle lists the non-built-in packages mounted.
The editor menu only shows the shader playground when its C side is
compiled in (dev builds). The launcher picks mods (on/off and load order,
keeping previously selected mods that are still installed) and starts the
game or the editor: on desktop it spawns the sibling executable, in
single-binary builds (web, Android) it switches this process over. The
choice is saved to `user/last_launch.lua`, which game and editor also read
when started on their own (e.g. from a debugger).

### shader_playground.lua (editor)

The shader playground's scene (see `dev/README.md`). Its C side (`src/dev/`)
is only compiled into dev builds, so `gramarye.shader_playground` is nil in
release builds: the editor menu hides the button, and this scene just shows
a notice. Files on the left, the code editor in the middle, the live
preview on the right (custom-node kinds 7 and 8, `SHADER_PLAYGROUND_KIND`
and `SHADER_EDITOR_KIND`). The editor and the new-shader name box both take
typing, so only one is live at a time.

### main.lua

The demo hub. Buttons for scenes that only exist in some apps (the tilemap
editor lives in the editor package) are appended only when the scene
exists, not written inline as `cond and x or nil`, because a nil ends
`ipairs` early. `on_click` is called with no arguments, so the scene buttons
are closures that remember the scene name.

Demo scene showing off `gramarye.ui` primitives plus the game-side
composite widgets from `lib/widgets`. Intended as a template: replace the
body of `on_draw` with your game's actual screens.

Details worth knowing:

- Key ids (`KEY_SPACE`, `KEY_ESCAPE`, `KEY_K`) are resolved once at scene
  load rather than looked up by string every frame.
- Callbacks like `goto_splash`, `goto_planet`, `push_pause`, `log_reload`
  are hoisted to scene level rather than created as closures inside
  `on_draw`, since closures created per-frame are garbage; anything that
  doesn't capture per-frame state belongs at scene level. The same applies
  to `tab_clicks`, which is built once instead of every frame.
- Has four tabs: Overview, Inventory, Settings, Widgets. Note the
  `inventory_panel` function and `settings_volume` variable it references
  are not currently defined in this file (previously present only as
  commented-out code) — this is pre-existing and out of scope for the
  comment-stripping pass.
- The Widgets tab demonstrates the M2 primitives: `List`/`Grid` pull rows
  from `gramarye.demo`, a C-owned data source — only the currently visible
  rows ever cross into Lua (virtualization).
- Scene-stack hooks `on_pause` / `on_resume` are optional and fire around
  `gramarye.scene.push`/`pop`.

### pause.lua

Demonstrates the scene stack. `main.lua` pushes this scene with
`gramarye.scene.push("pause")`; the scene underneath keeps all of its state
(Lua table, C systems, entities) but stops updating/drawing until this one
is popped.

### planet.lua

Renders a rotating icosahedral hex world into a UI `custom` node, with
controls that drive the C-side World service (`gramarye.world.*`).

Key architecture notes:

- Terrain is derived on the C side from per-tile elevation/temperature/
  humidity (ECS `TileComp`). The generation-parameter sliders (sea level,
  global temp, mountain, detail, resolution) are drawn by C into a second
  custom node — dragging only moves the handle; the world regenerates once,
  on release, since regeneration is expensive on large worlds.
- `GLOBE_KIND`, `CONTROLS_KIND`, `CHUNK_DEBUG_KIND` are custom-node kind ids
  that must stay in sync with `WORLD_GLOBE_KIND`/`WORLD_CONTROLS_KIND` in
  `world.h` and `LOCAL_CHUNK_DEBUG_KIND` in `local_chunk_lua.h`.
- `VIEW_NAMES` indices match `WORLD_VIEW_*` in `world.h` (0-based in C).
- `FAULT_NAMES` indices match `WORLD_FAULT_*` in `world_pipeline.h` (0-based
  in C).
- The seed text box is a controlled input: typed text may not parse as a
  number mid-edit, so it's staged into the world via `W.set_seed()` as it
  changes, filtering to digits only. It starts `nil` until first drawn, so
  it picks up the world's actual seed on the first frame.
- "Generate new seed" stages a random seed via `W.reroll()` — it does NOT
  regenerate the world (same staging contract as every other control here;
  only Apply/Regenerate actually rebuilds the globe) — and syncs the text
  box display to match.
- The local map's view buttons are one per `LocalChunkView`: a view added in
  C shows up there automatically.
- Local-chunk debug preview generates `services/local_chunk`'s border-
  consistent elevation/river fields for the selected cell and shows them as
  a crude heightmap in the corner. This is a debug preview, not final tile
  rendering — see `local_chunk.h` for what this pass does and doesn't
  cover.
- `control_panel()`: staged edits only take effect when Regenerate is
  clicked (tracked via `W.dirty()`/`W.generating()`). The Regenerate button
  label reflects generating/dirty/clean state. The panel is wrapped in a
  `ScrollFrame` because Clay only scrolls a clip element with a *bounded*
  height (a grow height sizes to content, so nothing overflows) — this
  mirrors the working `List` widget pattern: the scroll node is the panel
  itself (a plain `Panel` wrapper around it would not scroll), sized to the
  screen height minus the header bar (52px).
- Tile inspection lives in a floating popup anchored on the globe
  (`tile_popup()`) rather than in the control panel, so it isn't shown
  twice.
- `tile_popup()` is anchored on the selected hex via
  `world_selected_screen_pos` (`world.c`), listing that tile's info plus a
  compact row per neighbor. Neighbor slot numbers are the icosphere's raw
  neighbor order, not compass directions — the geometry has no notion of
  N/S/E/W to label them with. The popup id is keyed on the selected cell
  (`"tile_popup_" .. sel.cell`) because `gramarye.ui`'s Popup persists x/y
  across frames so it can be dragged — keying on the cell means a new
  selection always starts anchored at its own screen position instead of
  wherever the previous cell's popup was left.
- `chunk_preview_panel()` is a fixed corner panel showing the selected
  hex's local guide terrain and its ring of neighbors as one seamless,
  north-up composite (via `local_chunk_draw_connected_in_rect`), each
  pixel read from the cell that owns it, with ownership borders drawn
  faintly. It's independent of `tile_popup` so it survives the
  popup being closed.
- `mouse_over_overlays()` exists because the globe's own click/drag/zoom
  handling (`world.c`) runs against the raw mouse position with no idea a
  Lua popup is drawn over part of it — a click meant for the popup would
  also reselect/rotate the globe underneath ("click-fighting"). This tells
  the globe to ignore mouse input for any frame the pointer is over one of
  the Lua overlays. There's a one-frame lag (same caveat as
  `gramarye.ui.element_rect`/`hovered` elsewhere) because it uses last
  frame's laid-out rect, since this frame's popup hasn't been rendered yet
  when it runs. It must run before the globe's custom node draws this frame
  (i.e. before `GramaryeUI_end_and_render`, which happens after `on_draw`
  returns).
- In `on_draw`, Row/Column take `w`/`h` at the *top level* of their props
  table, not inside a `layout=` table (only `Panel` accepts `layout=`).
  Every ancestor of the globe must grow, or the custom node collapses to
  zero width. The tile popup and chunk preview panel are drawn after the
  main column so they layer on top of the globe/control panel.

### skin_demo.lua

UV-remap skinning demo: one baked walk-in-place animation, driven by
`gramarye.entities.set_skinned_sprite`, rendered against two different skin
textures. Press SPACE to swap skins live — the animation frames never
change, only which detail texture they sample.

Related references:
- `projects/tools/skin_baker/README.md` — author-time workflow + bake step
- `include/components/skinned_sprite.h` — the C component
- `packages/engine/shaders/*/skin.fs` — the sampling shader

Details:

- `frames` holds two poses of the *same* animation, baked once by
  `tools/skin_baker/bake_skins.py` from `skins/example`'s map +
  frames.
- `skins` holds the two skin textures; swapping between these live is the
  entire point of the demo — both share the same `frames` untouched.
- The sprite is drawn at 128x160 even though the source art is 16x20, i.e.
  8x scale, so it reads clearly on screen.
- `FRAME_TIME = 0.35` seconds per pose gives a slow idle wave.

### splash.lua

Full-screen Clay UI demo layered over C-rendered sprite entities. Shows the
engine title, and waits for a click (or previously, a timer) before going
to the app's home, which comes from the mounted packages' manifests
(`home = {...}`): launcher -> `launcher`, game -> `game_main_menu`,
editor -> `editor_main_menu`.

Details:

- Opt-in image skinning (`apply_skin`): if the UI atlas texture is present,
  buttons get skinned with a nine-patch frame. If the atlas is missing,
  `load_texture` returns `nil` and buttons keep their plain color skin — no
  crash, pure graceful fallback. This runs once per scene-enter; theme
  bindings persist across scenes since the theme module is cached.
- The `continue` skin is intentionally darker on hover (the default
  `button_hover` skin is lighter than its base).
- Sprite entities are simulated/moved via `gramarye.entities` and drawn by
  the C `sprite_render` system (under the camera, below the UI). The UI
  tree has no opaque background, so the sprites show through it. World
  `(0,0)` is screen center (camera offset); sprites are spawned in a row
  below the title.
- `build_tree()` is rebuilt every frame (like `main.lua`) so hover-reactive
  elements (e.g. Button's fade) re-evaluate each frame. A one-shot cached
  tree would bake in whatever hover state existed at scene-enter and never
  update again.
- In `on_update`, sprites bob and spin via `math.sin`; that motion state
  lives in the ECS transform, not in Lua.
