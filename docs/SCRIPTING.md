# Lua scripting (scenes)

Game orchestration is authored in **Lua 5.4** (vendored via FetchContent,
statically linked). Lua decides what a screen looks like, wires input/UI,
fires scene transitions, and chooses which C systems run per frame. Heavy
per-frame math belongs in **C systems** registered through the ECS.

## Layout

```
assets/scripts/
├── scenes/    # one file per scene; splash.lua runs first
│   ├── splash.lua
│   └── main.lua
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
}
```

Per frame the host runs: C systems of the active scene (`Scene_run`), then
`on_update(dt)`, then applies any pending transition, then `on_draw` inside
the frame. Transitions are deferred — `gramarye.scene.change` never tears down
the scene mid-call.

## API (`gramarye` global)

| Function | Notes |
|---|---|
| `gramarye.log(msg)` | raylib TraceLog INFO |
| `gramarye.require(path)` | load+cache `assets/scripts/<path>.lua` (e.g. `"lib/util"`) |
| `gramarye.scene.change(name)` | deferred transition to `scenes/<name>.lua` |
| `gramarye.scene.current()` | active scene name |
| `gramarye.systems.add(name)` | attach a C system (registered in main.c) to this scene |
| `gramarye.systems.enable(name, bool)` | toggle a system globally |
| `gramarye.input.key_pressed(name)` / `key_down(name)` | `"a"`, `"3"`, `"space"`, `"enter"`, `"escape"`, arrows, shift/ctrl |
| `gramarye.input.mouse_pressed([button])` | default left |
| `gramarye.input.mouse_pos()` / `touch_pos()` | returns `x, y` |
| `gramarye.draw.text(s, x, y, size[, r, g, b[, a]])` | prototyping helper |
| `gramarye.draw.rect(x, y, w, h[, r, g, b[, a]])` | prototyping helper |
| `gramarye.draw.clear(r, g, b)` | |
| `gramarye.time.total()` / `frames()` | from GlobalState (global system) |

## Exposing C to Lua

Register systems by name in `main.c`:

```c
ScriptHost_register_system(host, "physics", physics_system_register(ecs, state));
```

then `gramarye.systems.add("physics")` from any scene's `on_enter`.

Add custom functions (e.g. future gramarye-ui bindings):

```c
static int l_spawn_wave(lua_State *L) { /* ... */ return 0; }
ScriptHost_register_function(host, "game", "spawn_wave", l_spawn_wave);
-- lua: gramarye.game.spawn_wave()
```

## Errors and hot reload

Script errors never crash the game: the failing hook is disabled, the error +
traceback goes to the log, and the screen shows the traceback. On desktop,
**F5** reloads the current scene file in place (edit `main.lua`, hit F5).
