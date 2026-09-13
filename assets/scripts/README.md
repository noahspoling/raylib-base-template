# Scripts

This directory holds the Lua side of the Gramarye demo game: shared UI
plumbing under `lib/`, and per-scene screens under `scenes/`. The `.lua`
files themselves are kept comment-free by convention; this README captures
the context that used to live inline.

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

It's loaded from `assets/` at runtime (unlike the embedded, version-pinned
`gramarye.ui` library layer), so on desktop it hot-reloads with F5 while
iterating on game UI.

Registers game-specific skins on top of the library's primitive skins
(`slot`, `badge`, `panel_header`, `tooltip`).

Widgets provided:

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

## scenes

### main.lua

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
  hex's actual local-map representation: the selected square plus its
  connected neighbor squares (via
  `local_chunk_draw_connected_in_rect`), laid out in the standard
  flat-top-hex offset arrangement (N above, S below, 4 diagonals to either
  side), with the 2 staggered N/S neighbors outlined differently from the 4
  clean diagonal ones. It's independent of `tile_popup` so it survives the
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
- `assets/shaders/*/skin.fs` — the sampling shader

Details:

- `frames` holds two poses of the *same* animation, baked once by
  `tools/skin_baker/bake_skins.py` from `assets/skins/example`'s map +
  frames.
- `skins` holds the two skin textures; swapping between these live is the
  entire point of the demo — both share the same `frames` untouched.
- The sprite is drawn at 128x160 even though the source art is 16x20, i.e.
  8x scale, so it reads clearly on screen.
- `FRAME_TIME = 0.35` seconds per pose gives a slow idle wave.

### splash.lua

Full-screen Clay UI demo layered over C-rendered sprite entities. Shows the
engine title, and waits for a click (or previously, a timer) before going
to `main`.

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
