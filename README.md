# __PROJECT_NAME__

Minimal Gramarye starter template.

## Quick start (desktop)

From the **GameWorkspace** repository root (the directory that contains `scripts/` and `projects/`):

```bash
./scripts/build_debug.sh <project>
./scripts/run_desktop.sh <project>
```

Use **`games/<name>`**, **`tools/<name>`**, or **`templates/<id>`** for `<project>`. List slugs: `./scripts/list_project_targets.sh`; grouped by folder: `./scripts/list_project_targets.sh --grouped`. New projects: `./tools/new_project.sh <name> games|tools [template_id]` (legacy `./tools/new_project.sh <name> [template_id]` creates under **games/**). This README lives under `projects/templates/template/` as a scaffold; after `tools/new_project.sh` copies it to `projects/games/<your_game>/`, pass **`games/<your_game>`** to the scripts. Template prefabs use **`templates/<id>`** (e.g. `templates/template`).

## Other targets (Web, Android, macOS + ANGLE, iOS notes)

See **[docs/TARGETS.md](docs/TARGETS.md)** for `GRAMARYE_TARGET`, CMake presets, and helper scripts:

- `./scripts/build_web.sh <project>` — Emscripten / WebAssembly
- `./scripts/build_android.sh <project>` — Android NDK (shared library only); for an installable APK use the bundled Gradle client: `cd <project>/android && ./gradlew assembleDebug`
- `./scripts/build_macos_gles_angle.sh <project>` — macOS OpenGL ES 2 + ANGLE (host must be Darwin)
- **[mobile/README-ios.md](mobile/README-ios.md)** — Xcode-first iOS (no CMake preset upstream)
