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

```
assets/scripts/
├── scenes/    # one file per scene; splash.lua runs first
│   ├── splash.lua
│   ├── main.lua
│   └── pause.lua
└── lib/       # shared modules, loaded with gramarye.require("lib/<name>")
```

Scripts ship with the game on every target: copied next to the desktop binary,
preloaded into MEMFS on web, packaged into the APK on Android. They are always
loaded through raylib file IO, so one code path covers all platforms.

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
| `gramarye.require(path)` | load+cache `assets/scripts/<path>.lua` (e.g. `"lib/util"`) |
| `gramarye.scene.change(name)` / `push(name)` / `pop()` | deferred transitions (see Scene stack) |
| `gramarye.scene.current()` / `depth()` | active scene name / stack depth |
| `gramarye.systems.add(name)` | attach a C system (registered in main.c) to this scene |
| `gramarye.systems.enable(name, bool)` | toggle a system globally |
| `gramarye.input.key(name)` | resolve a key name to an id **once** (do it at scene top) |
| `gramarye.input.key_pressed(k)` / `key_down(k)` | `k` is an id from `input.key` or a name string |
| `gramarye.input.mouse_pressed([button])` | default left |
| `gramarye.input.mouse_pos()` / `touch_pos()` | returns `x, y` |
| `gramarye.draw.text(s, x, y, size[, r, g, b[, a]])` | prototyping helper |
| `gramarye.draw.rect(x, y, w, h[, r, g, b[, a]])` | prototyping helper |
| `gramarye.draw.clear(r, g, b)` | |
| `gramarye.time.total()` / `frames()` | accumulated sim time / sim tick count |
| `gramarye.entities.*`, `gramarye.textures.load` | sprite entities, see below |

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

## Performance notes for scripts

- **Hoist stable closures and tables out of `on_draw`** — everything built
  there is garbage the collector must reclaim every frame. The Lua VM runs
  generational GC (set by the host), which makes this cheap but not free.
  See the comments in `scenes/main.lua` and `scenes/splash.lua`.
- Resolve key names once with `gramarye.input.key("space")` at scene top.
- Big datasets stay in C; UI pulls visible rows by index (see `gramarye.demo`
  and the List widget in `main.lua`).

## Exposing C to Lua

Register systems by name in `main.c`:

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
`gramarye.*` modules are not reloaded; they ship in the binary). Dev caveat:
scenes that call `gramarye.ui.load_texture` on enter (like splash's skin) will
register a fresh texture per reload — harmless during development.
