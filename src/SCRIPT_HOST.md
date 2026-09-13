# Script Host

[script_host.h](../include/script_host.h) / [script_host.c](script_host.c) is
the bridge between the C engine and Lua. It owns the Lua VM, the scene stack,
and the `gramarye` binding table; it is the thing `main.c` drives every frame
and the thing scenes (Lua) talk to via `gramarye.*`.

This document explains the C-side implementation. For the Lua-author-facing
scene contract and API table, see [docs/SCRIPTING.md](../docs/SCRIPTING.md) —
that is the reference to hand to someone writing `assets/scripts/`; this one
is for someone changing `script_host.c` itself.

## Contents

- [Responsibilities](#responsibilities)
- [Data structures](#data-structures)
- [Lifecycle](#lifecycle)
- [The scene stack](#the-scene-stack)
- [Hook dispatch and errors](#hook-dispatch-and-errors)
- [Chunk loading](#chunk-loading)
- [The `gramarye` table](#the-gramarye-table)
- [Extending from C](#extending-from-c)
- [Gotchas](#gotchas)

## Responsibilities

- Own the single `lua_State` for the game's lifetime.
- Load scene files (`assets/scripts/scenes/<name>.lua`) and run their
  `on_enter/on_update/on_draw/on_exit/on_pause/on_resume` hooks.
- Maintain the **scene stack** (`change`/`push`/`pop`), deferring transitions
  requested from Lua until a safe point.
- Register C systems by name so Lua can attach them to a scene
  (`gramarye.systems.add`), and register extra C functions under
  `gramarye.<module>.<name>`.
- Install the `gramarye` binding table (scene control, input, immediate-mode
  draw helpers, time, a demo data source) — the orchestration-level API only;
  bulk/hot-path work stays in C systems.
- Catch every Lua error via `lua_pcall` + traceback so a script bug degrades
  to an on-screen error screen instead of crashing the game, and support
  hot-reloading the current scene (F5 on desktop).

## Data structures

```c
struct ScriptHost {
    Arena_T arena;        // persistent (engine lifetime)
    Arena_T scene_arena;  // rewound on every full scene change
    ECS *ecs;
    GlobalState *global_state;
    lua_State *L;

    SceneEntry stack[SCRIPT_HOST_MAX_STACK];   // SCRIPT_HOST_MAX_STACK = 8
    size_t depth;

    PendingOp pending;                          // deferred change/push/pop
    char pending_scene[SCRIPT_HOST_NAME_MAX];

    RegisteredSystem systems[SCRIPT_HOST_MAX_SYSTEMS]; // SCRIPT_HOST_MAX_SYSTEMS = 32
    size_t system_count;

    bool script_error;
    char error_message[1024];
};
```

- **`SceneEntry`** — one stack slot: the scene's name, a `Scene` (the C
  system list, see [scene.h](../include/scene.h)), and three Lua registry
  refs: `scene_ref` (the table the scene file returned), plus `update_ref`
  and `draw_ref` cached at load time. Caching those two means the per-frame
  hot path is a `lua_rawgeti`, not a string field lookup; the rarer hooks
  (`on_enter/on_exit/on_pause/on_resume`) are looked up by name each time
  they fire since they only fire on transitions.
- **`RegisteredSystem`** — a name → `SystemId` mapping, populated by
  `ScriptHost_register_system` (usually called from `main.c` at startup) so
  `gramarye.systems.add("name")` can resolve to a real system.
- Two arena tiers live here: `arena` is the engine-lifetime allocation the
  `ScriptHost` itself was allocated from; `scene_arena` is exposed via
  `ScriptHost_scene_arena()` for scene-lifetime C allocations and is rewound
  on every full `change()` (not on `push`/`pop` — a suspended scene's
  allocations must survive being paused).
- The Lua registry holds two well-known keys: `HOST_REGISTRY_KEY` maps to the
  `ScriptHost*` itself (as light userdata, so any bound C function can call
  `host_from(L)` and get back to the struct), and `LOADED_REGISTRY_KEY` is
  the module cache used by `gramarye.require`.

## Lifecycle

```
ScriptHost_new(arena, ecs, global_state)
    -> luaL_newstate + luaL_openlibs
    -> generational GC (lua_gc(L, LUA_GCGEN, 0, 0))
    -> stash the host pointer + an empty module cache in the registry
    -> install_bindings()                      // the gramarye table

ScriptHost_register_system(host, name, id)     // once per system, from main.c
ScriptHost_register_function(host, module, name, fn)  // optional extra bindings

ScriptHost_load_scene(host, "splash")          // == do_change, the first scene

// per render frame:
ScriptHost_update_fixed(host, dt)   // 0..N times, fixed dt — runs C systems
ScriptHost_update(host, dt)         // once, real dt — Lua on_update, then
                                     // applies any pending scene transition
ScriptHost_draw(host)               // once — Lua on_draw, or the error screen

ScriptHost_dispose(host)            // lua_close + free the scene arena
```

The fixed/variable split mirrors the engine's main loop: C systems
(`Scene_run`, via `ScriptHost_update_fixed`) run at a deterministic `1/60`
tick, while `on_update`/`on_draw` run once per render frame so raylib's
per-frame input edges (`key_pressed`) behave the way a scripter expects. See
[docs/SCRIPTING.md](../docs/SCRIPTING.md#timing-model) for the player-facing
version of this.

Generational GC is set once at VM creation: the per-frame Lua garbage (UI
tables, closures built in `on_draw`) is almost entirely short-lived, which
the minor collector reclaims cheaply.

## The scene stack

Three internal operations, all funneled through the public request functions
so a scene can never mutate the stack mid-hook (see
[Hook dispatch](#hook-dispatch-and-errors)):

| Internal | Public request | Effect |
|---|---|---|
| `do_change(host, name)` | `ScriptHost_request_scene` | Exits and unrefs every live entry top-down, rewinds `scene_arena`, loads `name` as the sole entry (`depth = 1`), fires its `on_enter`. |
| `do_push(host, name)` | `ScriptHost_request_push` | `on_pause` on the current top, loads `name` into the next slot, fires its `on_enter`. On load failure, resumes the paused scene instead. Refuses (logs a warning) past `SCRIPT_HOST_MAX_STACK`. |
| `do_pop(host)` | `ScriptHost_request_pop` | `on_exit` + unref the top, `on_resume` on the scene below, clears any error state. Refuses to pop the last entry. |

`ScriptHost_update` applies at most one pending op per frame, after
`on_update` returns — never from inside a hook, so Lua code never has to
reason about the stack changing under it mid-call. `top(host)` (the only
entry that updates/draws) is just `depth > 0 ? &stack[depth-1] : NULL`.

`load_into_entry` is the shared loader behind both `do_change` and `do_push`:
run `scripts/scenes/<name>.lua`, require it to return a table, `Scene_init`
the embedded `Scene`, cache `on_update`/`on_draw` refs, and `luaL_ref` the
table itself. It does **not** call `on_enter` — callers do that after
committing the entry to the stack, so a failed load never left a half-entered
scene behind.

## Hook dispatch and errors

Two call paths, both wrapping `lua_pcall` with a `luaL_traceback` message
handler so any error (including from deep inside a scene's own `require`d
modules) comes back as a formatted traceback string:

- **`pcall_hook`** — function + args already pushed; used for the cached
  `on_update`/`on_draw` refs (the hot path).
- **`call_hook0`** — looks a hook up by name on the scene's table; used for
  the rare `on_enter/on_exit/on_pause/on_resume`. Skipped entirely while
  `host->script_error` is set, since the failing scene's Lua-side state is
  unknown at that point and calling into it risks a cascading error.

On failure, `set_error` records the message, logs it, and sets
`script_error`. `ScriptHost_draw` checks that flag first and, if set, draws a
full-screen error panel with the wrapped traceback instead of calling
`on_draw` — see `draw_wrapped_text` for the line-wrapping (newline-aware,
falls back to character wrapping for long unbroken lines like asset paths).
The error clears on the next successful transition, `do_pop`, or a reload.

## Chunk loading

`run_file` is the one place Lua source gets read off disk (via raylib's
`LoadFileText`/`UnloadFileText`, which works unmodified across desktop, web
MEMFS, and the Android APK) and compiled+run with `luaL_loadbuffer` +
`lua_pcall`. Both scene loading (`load_into_entry`) and module loading
(`l_require`) go through it, so there is exactly one error-handling path for
"couldn't read/compile/run a script file."

`ASSET_PREFIX` is `""` on Android (APK asset paths are already relative to
the assets root) and `"assets/"` everywhere else (assets sit next to the
binary). It's also exposed to Lua as `gramarye.asset_prefix` so the shared
`gramarye-ui` Lua layer can build matching texture paths without knowing the
platform.

## The `gramarye` table

`install_bindings` builds the table installed as the Lua global `gramarye`.
It is intentionally **orchestration-level only** — scene control, input
polling, a handful of immediate-mode draw helpers for prototyping, time, and
a demo data source. Heavy per-frame work (bulk sprite drawing, simulation)
belongs in C systems reached via `gramarye.systems.add`, not in this table.
The full function-by-function reference lives in
[docs/SCRIPTING.md](../docs/SCRIPTING.md#api-gramarye-global); notable
implementation details:

- `gramarye.require(mod)` (`l_require`) special-cases the `gramarye.*`
  namespace: those modules ship embedded in the `gramarye-ui` binary via
  `package.preload`, so they're resolved through Lua's own `require` with no
  filesystem access. Everything else is loaded from
  `assets/scripts/<mod>.lua` and cached in the registry's
  `LOADED_REGISTRY_KEY` table (cleared on reload, see below).
- `gramarye.input.*` accepts either a key **name** string (resolved once via
  `key_from_name`, a small name→raylib-`KEY_*` table plus the obvious
  letter/digit shortcuts) or a numeric id returned by `gramarye.input.key`,
  so hot paths can skip the string compare.
- `gramarye.draw.*` stays immediate-mode on purpose: `on_draw` already runs
  inside `BeginDrawing`, and raylib batches via rlgl, so a Lua→C command
  buffer would only add a copy with no batching win.
- `gramarye.demo.*` stands in for a game's own C/ECS-owned dataset — the
  full 500-row set never lives in Lua, only the two strings `l_demo_row`
  formats per call. A real game backs a List/Grid with its own components
  and does its sorting/filtering in C, following this pattern.

## Extending from C

```c
// Make a C system addressable by name (call once, e.g. from main.c setup):
ScriptHost_register_system(host, "physics", physics_system_register(ecs, state));
// -> gramarye.systems.add("physics") from any scene's on_enter

// Add a custom function under gramarye.<module>.<name>:
static int l_spawn_wave(lua_State *L) { /* ... */ return 0; }
ScriptHost_register_function(host, "game", "spawn_wave", l_spawn_wave);
// -> gramarye.game.spawn_wave() from Lua
```

`ScriptHost_register_function` creates the module subtable on demand if it
doesn't exist yet (pass `module = NULL` to install straight on `gramarye`
itself, as `l_log`/`l_require` are). Bound functions call `host_from(L)` to
reach the `ScriptHost*` and, from there, `top(host)` for the active
`SceneEntry`, `host->ecs`, or `host->global_state` as needed — see
`entities_lua.c` for a real example (entity spawn/transform/sprite
bindings).

## Gotchas

- **Never mutate the stack from inside a hook.** All three transitions are
  deferred (`ScriptHost_request_*`) and applied once per frame from
  `ScriptHost_update`, after `on_update` returns. This is why `l_scene_change`
  et al. just forward to the request functions instead of calling
  `do_change`/`do_push`/`do_pop` directly.
- **A scene in error state gets no hooks**, including `on_exit` — its Lua
  side may be in an inconsistent state, so calling into it risks a cascading
  failure. This means C-side cleanup a scene was relying on `on_exit` for
  will not run if that scene errored; don't put must-run cleanup only in
  Lua.
- **`scene_arena` is rewound on `change`, not `push`/`pop`.** A pushed
  scene's C allocations from `scene_arena` stay alive while it's suspended
  below the stack top; they're only reclaimed at the next full `change`.
- **Reload (`ScriptHost_reload_current`) clears the module cache** but not
  `package.preload` — embedded `gramarye.*` modules are baked into the
  binary and are never hot-reloaded, only game-local `scripts/**.lua`.
- **`SCRIPT_HOST_MAX_STACK` (8) and `SCRIPT_HOST_MAX_SYSTEMS` (32)** are
  fixed-size arrays in the struct, not dynamic; both failure paths just log
  a warning and no-op rather than allocate.
