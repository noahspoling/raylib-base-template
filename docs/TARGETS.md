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

The game is built as **`add_library(... SHARED)`** so it can be loaded like a typical raylib Android native library.

### Gradle client (APK)

Every project created from this template ships a ready Gradle client in **`android/`** (NativeActivity host, `applicationId com.gramarye.<name>`, splash theme on Android 12+). The package name, app label, and `System.loadLibrary` name are filled in by `tools/new_project.sh`.

```bash
cd <project>/android
./gradlew assembleDebug        # builds the C code via externalNativeBuild + packages the APK
./gradlew installDebug         # deploy to a connected device
```

Or open `<project>/android/` in Android Studio. Assets in `<project>/assets/` are packaged into the APK automatically (`assets.srcDirs` includes `../../assets`); raylib loads them through `AAssetManager` with paths relative to the assets root.

Machine-specific notes:
- `app/build.gradle.kts` passes `-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja`; adjust if ninja lives elsewhere on your machine.
- Set your JDK via `JAVA_HOME` or Android Studio, **not** by committing `org.gradle.java.home` to `gradle.properties`.

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

---

## CMakeLists.txt implementation notes

These used to be inline comments in `CMakeLists.txt`; kept here since the build script itself is now comment-free:

- **`GRAMARYE_TARGET`** drives raylib's `PLATFORM` / `OPENGL_VERSION` and Gramarye's `BUILD_WEB`. Valid values: `desktop | web | android | macos_gles_angle`. iOS has no upstream raylib CMake `PLATFORM`; use `mobile/README-ios.md` instead.
- **`GRAMARYE_GAME_THREADING`** (default `OFF`): the game loop is single-threaded, so libcore's locks default to compiled-out no-ops (`GRAMARYE_SYNC_SINGLE_THREADED` propagates to the ECS built in this tree). Flip it `ON` if the game grows real threads (e.g. async asset loading). Standalone `gramarye-ecs` builds/tests keep threading `ON` and stay fully tested independently of this flag.
- **Lua 5.4** (scripting layer) is fetched via `walterschell/Lua`, which wraps the upstream sources in plain CMake so the active toolchain (NDK, Emscripten, desktop) applies as-is.
- **`CMAKE_POLICY_VERSION_MINIMUM 3.5`**: upstream Lua declares `cmake_minimum_required(VERSION 3.1)`, which CMake 4+ refuses outright; this floor only applies to subprojects that declare an older minimum, so it's set globally before fetching.
- **`GRAMARYE_UI_LUA`** enables Lua bindings in gramarye-ui (requires `lua_static`, fetched earlier in the same file). gramarye-ui itself is Clay UI and reuses the raylib target already fetched above — drop a local checkout into `libs/gramarye-ui` to skip the network fetch, same pattern as the other vendored libs.
- **Asset staging** (`<project>/assets`) differs per target:
  - desktop/macOS: symlinked next to the binary; `main.c` pins cwd to the exe dir. A symlink (not a copy) means editing source `assets/*.lua` is picked up immediately by F5 hot-reload (`ScriptHost_reload_current`) with no rebuild — a stale copy would silently shadow every edit. On Windows, `mklink` for directories needs elevation/Developer Mode, so the build copies the directory instead of symlinking.
  - web: preloaded into MEMFS at `/assets`.
  - android: packaged into the APK by Gradle (`assets.srcDirs` includes `../../assets`).
- **`-Wl,-z,max-page-size=16384`** (Android link options): required for Google Play on Android 15+, which mandates 16 KB-aligned LOAD segments.

## Android splash theme notes

`android/app/src/main/res/values/themes.xml` defines the base `Theme.GramaryeSplash`; `values-v31/themes.xml` overrides it with splash-screen attributes on Android 12+ (the system splash shown while `NativeActivity` loads the game `.so`). Replace `windowSplashScreenAnimatedIcon` in the v31 override with a drawable to brand it.

## web/shell.html implementation notes

The custom Emscripten shell replaces the default red/white DOM Emscripten normally injects with a loading overlay that matches the rest of the page:

- The `<script>` block is the minimum glue Emscripten's runtime looks for on `Module` — no external scripts, no CDN dependency.
- `Module.setStatus` is called by the runtime during download/instantiate with either a `"Downloading...(n/m)"`-style string or `""` when startup is complete; that gets folded into the loading overlay's progress bar and hidden once startup finishes.
- The `#loading` overlay is shown until the wasm module reports it is running, then hidden by `setStatus`. It's kept as plain CSS/DOM (no framework) so it never depends on the module actually loading.
