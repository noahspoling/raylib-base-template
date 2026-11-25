# dev/

Dev-only tools. Nothing here ships. CMake option `GRAMARYE_DEV_TOOLS` is ON by default for
desktop Debug builds and OFF for Release, MinSizeRel, web and Android. When it's off,
`src/dev/` isn't compiled and nothing reads this folder. Shaders are read straight from
here (an absolute path baked in at build time), never through the package mount stack.

## Shader playground

Editor menu > "Shader playground" (or `h`). The scene is in the `editor` package
(`packages/editor/scripts/scenes/shader_playground.lua`); the button only shows when the
C side is compiled in. Files on the left, code editor in the middle, live
preview on the right. Every `shaders/<name>.fs` is an entry; "+ New shader" copies
`starter.fs` to a new name.

The preview recompiles from the editor text about 0.3 s after you stop typing, so there's
no need to save to see a change. `Ctrl+S` writes the file. Switching entries or leaving
the scene saves unsaved edits for you. A broken shader keeps the last good one in the
preview. The error shows under the code with the line marked; click the error to jump
there. Editing the file in an outside editor also works: the app follows the file unless
you have unsaved edits in the app, and then it warns you instead.

Editor keys: `Ctrl+Z`/`Ctrl+Y` undo/redo, `Ctrl+/` comment lines, `Tab`/`Shift+Tab`
indent, `Ctrl+=`/`Ctrl+-` font size, `Ctrl+Enter` recompile now, double-click selects
a word. `esc` leaves the editor; a second `esc` goes back to the editor menu. In this scene `esc`
no longer quits the app. Tabs become 4 spaces. The font is DejaVu Sans Mono, in `fonts/`.

Each shader is drawn as one quad over the canvas. Uniforms you can declare (all optional):

| Name | Type | Value |
|---|---|---|
| `texture0` | sampler2D | `shaders/<name>.png` if it exists (hot-reloaded too), else 1x1 white |
| `uTime` | float | seconds |
| `uResolution` | vec2 | canvas size in pixels |
| `uMouse` | vec2 | mouse position, 0..1 over the canvas |
| `uTextureSize` | vec2 | `texture0` size in pixels |

`fragTexCoord` runs 0..1 across the canvas, origin top-left. An optional
`shaders/<name>.vs` replaces the vertex shader. `starter.fs` is just the
inputs and a gradient.

To remove it entirely, delete `dev/` (shaders and fonts), `src/dev/`, `include/dev/`,
`packages/editor/scripts/scenes/shader_playground.lua`, the `GRAMARYE_DEV_TOOLS` blocks in
`CMakeLists.txt` and `src/app.c`, and `shader_button` in `editor_main_menu.lua`.

## Learning resources

A rough path, in order:

1. **The Book of Shaders** (thebookofshaders.com). Start here. Each chapter has editable
   examples. Retype them into the playground rather than copying.
2. **Freya Holmér, "Shaders for Game Devs"** (YouTube, 3 long parts). The math intuition
   (dot products, lerp, remapping) that every effect is built from.
3. **Inigo Quilez's articles** (iquilezles.org/articles). Your reference for 2D/3D
   distance functions, noise, and procedural palettes.
4. **Shadertoy** (shadertoy.com). Read other people's code. Its `iTime`/`iResolution`
   map to `uTime`/`uResolution` here, so most 2D shaders port with a rename.
5. **raylib shader examples** (raylib.com/examples, "shaders" section). How shaders hook
   into this engine: render textures, post-processing, palette switching, models.

For your style (pixel art, tiles, palettes):

- **jess::codes, "Draw fewer tiles - by using a Dual-Grid system!"** (YouTube).
- **Oskar Stålberg's talks** on Townscaper and Bad North: corner-based tiling and
  irregular grids.
- **t3ssel8r** (YouTube): 3D rendered so it reads as pixel art, with stable pixels and
  outlines.
- **Thomas Vasseur, "Art Design Deep Dive: Using a 3D pipeline for 2D animation in Dead
  Cells"** (Game Developer, 2018): one model in place of many sprite angles.
- **Ryan Brucks, "Octahedral Impostors"** (shaderbits.com): baking many view angles into
  one texture.
- **Joseph Huckaby's "Canvas Cycle"** demo of Mark Ferrari's art: what palette cycling
  can do with zero extra frames.

Going deeper:

- **Ben Golus** on Medium: outlines, anti-aliasing, the details that break.
- **Cyanilux** and **Alan Zucconi**: Unity-flavored tutorials; the shader logic carries over.
- **GPU Gems**: free on NVIDIA's developer site.

Habits that help: change one number at a time and watch what moves. Output intermediate
values as color (`finalColor = vec4(vec3(x), 1.0)`) to see them. Keep every experiment as
its own `.fs` file in this folder, so you build up your own reference library.

## GLSL ES 1.00 copies (`glsl100.py`)

Web and Android run GLSL ES 1.00, desktop GLSL 3.30, so every engine shader
exists twice: `packages/engine/shaders/glsl330/` and `glsl100/`. Edit the 3.30
file, then regenerate its copy:

```sh
python3 dev/glsl100.py                      # the tilemap shaders: ground.fs, wall.vs, wall.fs, sprite.fs
python3 dev/glsl100.py path/to/glsl330/x.fs # any other
```

Each `glsl330/<name>` becomes `glsl100/<name>` next to it; don't edit the
copies. Every copy is checked with `glslangValidator` when it's installed.
In dev builds the tilemap reloads a shader as soon as its file changes, so a
3.30 edit shows on the next frame. What changes:

- `#version 100` and a float precision header: `highp` where the GPU has it
  (`GL_FRAGMENT_PRECISION_HIGH`), because world positions run past
  `mediump`'s range.
- Vertex shaders: `in` -> `attribute`, `out` -> `varying`. Fragment
  shaders: `in` -> `varying`, `finalColor` -> `gl_FragColor`.
- `texture()` -> `texture2D()`. `texelFetch(tex, c, 0)` -> `texture2D` at
  the texel's center, which assumes the texture is `mapSize` texels (the
  tilemap's cell textures are) and that `ivec2 c = ivec2(...)` holds whole
  numbers.
- A `float x = ...dFdx/dFdy/fwidth...;` line gets the derivatives extension
  when the GPU has it, else `x = 1.0`.

GLSL ES 1.00 also reserves some words that 3.30 doesn't (`active`, `input`,
`output`, `filter`...): the validator catches them, so rename and re-run.

## Procedural pixel art (`make_textures.py`)

```sh
python3 dev/make_textures.py   # -> packages/core/textures/terrain/{riverbed,kelp}.png, textures/fx/splash.png
```

The riverbed and kelp fills are seamless 64x64 tiles: every pattern repeats
exactly every 64 px both ways (waves use whole numbers of periods across the
tile), and what's drawn near an edge wraps to the other side. Fills are
sampled one texel per world pixel, and all fills are one size
(`lib/terrains`). The riverbed is wet sand under shallow water (ripple
crests across the current, bent a little, grain speckle, pebbles with a lit
corner and a shadow); kelp is fronds along y (the shader waves them side to
side), each with leaves and a glint, on a dark bed.

The splash is a 6-frame sheet (24x20 per frame, side by side) drawn frame by
frame from a little physics: a spout in the first frames, droplets thrown up
and out on parabolas falling back past the ring, a ring on the water (an
ellipse, seen from above at a slant) growing, everything fading as it goes,
around where the feet meet the water. `lib/water` plays it, and its `SPLASH`
table must match `FRAMES` / `FW` / `FH`. Edit the script and re-run it
rather than editing the PNGs.

## Pictures of the app for tests

Dev builds have `gramarye.app.screenshot(path)`: it saves the frame, once
it's drawn and before it's shown, to a PNG. A temporary scene can set up a
view, call it a few frames in, then `gramarye.app.quit()`; the picture is
the app's own frame, never whatever else is on screen.
