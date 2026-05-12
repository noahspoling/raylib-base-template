# Build targets (Gramarye template)

This project uses **`GRAMARYE_TARGET`** to select raylib’s platform and graphics profile. Set it via **CMake** (`-DGRAMARYE_TARGET=...`), **`CMakePresets.json`**, or the helper scripts in the **workspace root** [`scripts/`](../../../scripts/) (from `projects/games/<game>/docs/`; if you are reading this under `projects/templates/.../docs/`, open `scripts/` from the workspace root in the file tree).

**Layout:** this tree is a **scaffold** under `projects/templates/<template_id>/`. Copy it into **`projects/games/<name>/`** (game) or **`projects/tools/<name>/`** (tool) with **`tools/new_project.sh <name> games|tools [template_id]`** (omit `games|tools` to default to **games**). Pass **`games/…`**, **`tools/…`**, or **`templates/…`** slugs to workspace **`scripts/*.sh`**. List slugs: **`./scripts/list_project_targets.sh`**; by category: **`./scripts/list_project_targets.sh --grouped`**.

| Value | Raylib `PLATFORM` | Notes |
|--------|-------------------|--------|
| `desktop` (default) | Desktop | GLFW + system OpenGL (macOS uses OpenGL 3.3; Apple deprecates GL). |
| `web` | Web | Requires **Emscripten** (`emcmake` + `EMSDK`). |
| `android` | Android | Requires **Android NDK** (`ANDROID_NDK`) and `CMAKE_TOOLCHAIN_FILE`. Produces a **shared library** (`lib<project>.so`) for NativeActivity / JNI packaging. |
| `macos_gles_angle` | Desktop | Forces **OpenGL ES 2.0** at the raylib layer; link **ANGLE** (Metal backend) yourself so GLES/EGL resolve at link time. **macOS host only.** |

Upstream raylib does **not** expose `PLATFORM=iOS` in CMake. Use **[mobile/README-ios.md](../mobile/README-ios.md)** for an Xcode-oriented path.

---

## Desktop

From the **GameWorkspace** root (the repo that contains `scripts/` and `projects/`):

```bash
./scripts/build_debug.sh <project>
./scripts/build_desktop.sh <project>
# or explicitly from projects/games/<game>:
cmake -S . -B build -DGRAMARYE_TARGET=desktop
```

Replace `<project>` with a slug **`games/<game>`**, **`templates/<id>`**, or **`tools/<id>`** relative to `projects/` (for example `games/my_game` or `templates/template`), as used by the workspace helper scripts. To print every slug currently on disk: **`./scripts/list_project_targets.sh`**.

If you previously configured another flavor, wipe the build directory or re-run with `-DGRAMARYE_TARGET=desktop` so `PLATFORM` / `OPENGL_VERSION` cache entries do not leak between runs.

---

## Web (Emscripten)

1. Install and activate [Emscripten](https://emscripten.org/) (`emsdk`, `emsdk activate`, `source emsdk_env.sh`).
2. Ensure `EMSDK` is set and `emcmake` is on `PATH`.

```bash
./scripts/build_web.sh <project>
# or: cmake --preset web-wasm && cmake --build --preset web-wasm
```

Output: `<build>/<project>.html` plus `.js` / `.wasm` alongside. Serve the build directory over HTTP (file:// often blocks wasm).

Workspace hint: [tools/scripts/install_emscripten.sh](../../../tools/scripts/install_emscripten.sh) clones emsdk into this workspace for convenience; you still must activate it.

---

## Android (NDK)

1. Install the [Android NDK](https://developer.android.com/ndk) and set **`ANDROID_NDK`** to the NDK root.
2. Optional: **`ANDROID_ABI`** (default `arm64-v8a`), **`ANDROID_PLATFORM`** (default `android-24`).

```bash
./scripts/build_android.sh <project>
# or: cmake --preset android-ndk && cmake --build --preset android-ndk
```

The game is built as **`add_library(... SHARED)`** so it can be loaded like a typical raylib Android native library. You still need an **APK / Gradle / AndroidManifest** that hosts NativeActivity and ships assets; raylib wraps `fopen` for asset paths when linked as on Android.

---

## macOS OpenGL ES + ANGLE

1. Obtain **`libEGL.dylib`** and **`libGLESv2.dylib`** for macOS (Metal-backed ANGLE). Two common approaches:
   - **Quick path:** copy the dylibs from a Chromium-based browser under `/Applications/Google Chrome.app/.../Libraries/` (paths change each Chrome version; use `find` as in the article below).
   - **Full build:** clone [ANGLE](https://github.com/google/angle), use `depot_tools` / `gn` / `ninja` as in Google’s docs or the walkthrough below.
2. Point CMake at those libraries, e.g. set **`ANGLE_ROOT`** when running **`./scripts/build_macos_gles_angle.sh <project>`** from the workspace root, or pass **`-DCMAKE_PREFIX_PATH=...`** / **`target_link_libraries`** so the linker resolves **EGL** and **GLESv2** against ANGLE, not the system.

**Step-by-step (raylib + CMake + ANGLE on macOS),** including the Chrome dylib shortcut and a full ANGLE-from-source layout: [Building and Linking Google’s ANGLE with Raylib on macOS](https://medium.com/@grplyler/building-and-linking-googles-angle-with-raylib-on-macos-67b07cd380a3) (Ryan Plyler, Medium). That post uses **`-DOPENGL_VERSION="ES 2.0"`** (and **`CUSTOMIZE_BUILD=ON`** when configuring raylib directly); this template’s **`GRAMARYE_TARGET=macos_gles_angle`** already forces **OpenGL ES 2.0** for raylib—you still need to **link the ANGLE dylibs** (see the article’s `find_library` / `g++` examples). Starter repo linked from the article: [grplyler/raylib-cmake-starter](https://github.com/grplyler/raylib-cmake-starter).

```bash
./scripts/build_macos_gles_angle.sh <project>
# or: cmake --preset macos-gles-angle && cmake --build --preset macos-gles-angle
```

Raylib stays **`PLATFORM_DESKTOP`** with GLFW; only the **graphics API** is GLES2 via **`OPENGL_VERSION`**, backed at runtime by ANGLE (often **Metal**). Validate in logs: vendor **Google Inc. (Apple)** and renderer containing **ANGLE**.

**Licensing:** redistributing Chrome’s dylibs may be restricted by Google’s terms; for shipping a game, prefer **ANGLE you built** or a **properly licensed** binary stack.

---

## CMake presets

See **[CMakePresets.json](../CMakePresets.json)**. Presets expect:

- **web-wasm**: `$env{EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`
- **android-ndk**: `$env{ANDROID_NDK}/build/cmake/android.toolchain.cmake`

Adjust paths if your emsdk or NDK layout differs.

---

## Gramarye libraries on mobile (audit)

**gramarye-libcore** is portable C99 (arena, hash, atomics). It uses `Threads::Threads` on non-Windows hosts; **Android NDK** and **Apple** toolchains provide pthreads. For Emscripten, keep **`BUILD_WEB=ON`** (the template sets this when `GRAMARYE_TARGET=web`).

**gramarye-ecs** depends only on libcore. The template forces **`BUILD_TESTS=OFF`** before fetching ecs so test executables are not built for cross targets.

If a future Gramarye release fails on a specific triple, fix it upstream or temporarily drop ecs usage in your fork; there is no separate “stub” target in this template.

---

## iOS

There is **no** `cmake --preset ios` in this repo. See **[mobile/README-ios.md](../mobile/README-ios.md)**.
