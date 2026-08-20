# fujinet-go-intv-desktop

A self-contained Mattel Intellivision emulator with a built-in FujiNet, based
on [jzIntv](http://spatula-city.org/~im14u2c/intv/) patched with a FujiNet
mailbox peripheral (BoIP -- Bus Over IP -- to a real `fujinet-firmware`
instance bundled with the app). Part of the FujiNet Go Desktop family
alongside `fujinet-go-adam-desktop`, `fujinet-go-apple2-desktop`,
`fujinet-go-coco-desktop`, and `fujinet-go-msx-desktop`.

**Status: feature-complete locally, not yet released.** All four native
frontends (GNOME/GTK4, KDE/Qt6, Windows, macOS), the debugger window, the
clickable keypad window, and Linux/Windows packaging (DEB/RPM/TGZ, both
flatpak manifests, NSIS installer) exist and have been built and run --
GNOME/KDE/Windows verified directly (including via Wine for Windows), macOS
written but not yet compiled on real hardware. See `TODO` for the full
milestone-by-milestone history and exactly what remains: pushing to the
GitHub remote and getting all 5 CI workflows green there (macOS and Windows
CI in particular are the first real test of those builds outside this
machine), then cutting the `v0.1.0` release tag.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

By default this configures `FRONTEND=all` (whichever of GNOME/KDE it can
find on Linux, or the native frontend on Windows/macOS), `WITH_INTV_ROMS=ON`
(embeds Mattel's EXEC/GROM/ECS firmware for a zero-setup local build -- **not**
for redistribution, see `COMPLIANCE.md`), and `WITH_WEBVIEW=ON` (embeds the
FujiNet web UI via WebKitGTK/QtWebEngine).

Useful variations:

```sh
# Headless core + tests only, no GTK4/Qt6/WebKit dependencies, no ROMs
# embedded (what every published artifact is built with):
cmake -B build -DFRONTEND=none -DWITH_INTV_ROMS=OFF

# Just one frontend:
cmake -B build -DFRONTEND=gnome   # or kde, macos, windows
```

`ctest` runs 16 suites covering the headless jzIntv core, ROM embedding, the
frame/audio publish contract, pad and keyboard input mapping (including the
ECS keyboard/second controller pair and the ECS/Intellivoice/PAL machine
options), the keypad disc's 16-way pointer geometry, the public session API,
the peripheral bus' address decode teardown (which the FujiNet cart hot-swap
depends on), the debugger's symbol table and STIC views, and (when
`WITH_INTV_ROMS=OFF`) that no Mattel firmware bytes ended up in the shipped
binary.

Set `INTV_OPEN_DEBUGGER=1` and/or `INTV_OPEN_KEYPAD=1` in the environment
before launching a built frontend to have it open those windows on startup.

## Project layout

- `core/` -- the toolkit-agnostic session library (`intvsession.h`): vendored
  jzIntv (`core/jzintv-generated/`, staged and patched by
  `cmake/StageJzIntv.cmake`), the headless SDL-free `gfx_desktop`/
  `snd_desktop` backends, frame/audio publish-and-copy buffers, pad/keyboard
  input injection, settings/paths, the debugger and STIC-view helpers
  (`core/debugger/`), and `core/tests/`.
- `frontends/{gnome,kde,macos,windows}/` -- one native UI per platform, each
  wrapping the same `core/` library: fixed 4:3 display, a debugger window
  (BACKTAB/MOB grid/GRAM-GROM sheet/palette), a clickable keypad window for
  both hand controllers, and a Settings dialog for the three machine
  options -- ECS (the Entertainment Computer System: a second controller
  pair, its own keyboard, and extra sound), Intellivoice (speech synthesis),
  and NTSC/PAL video standard. ECS and Intellivoice are tri-state
  (Auto/Off/On, matching jzIntv's own cart-metadata-driven default);
  changing any of the three restarts the emulator to apply. The ECS
  keyboard has its own input-mode toggle (in Settings, or live from a
  frontend's View menu) since the host keyboard can't drive both the ECS
  keyboard and the hand controllers at once. ECS is only selectable once an
  `ecs.bin` has been embedded or imported -- see `COMPLIANCE.md`.
- `tools/jzintv/` -- the jzIntv source patch, staging/patch scripts, and the
  Mattel EXEC/GROM/ECS images used for `WITH_INTV_ROMS=ON` builds.
- `tools/icons/` -- icon rendering (`make-icons.py`) from `data/icons/src/`
  into every installed size plus `.icns`/`.ico`.
- `build-aux/` -- flatpak manifests (GNOME + KDE) and the Windows NSIS
  installer script. Note: despite the name, this is committed source, not a
  build directory (see `.gitignore`'s `!build-aux/` exception).
- `.github/workflows/` -- per-platform CI (`linux.yml`, `macos.yml`,
  `windows.yml`), `flatpak.yml`, and `release.yml` (tag-triggered, builds and
  uploads every artifact; fails if the `vX.Y.Z` tag doesn't match
  `CMakeLists.txt`'s `project() VERSION`).

## Cutting a release

Bump `project(... VERSION x.y.z)` in `CMakeLists.txt` (and the matching
`<release version="x.y.z">` entry in both frontends'
`data/*.metainfo.xml`), commit, then push a `vx.y.z` tag. `release.yml`
verifies the tag matches before building anything.

## License

GPL-3.0-or-later. See `LICENSE` and `COMPLIANCE.md`.
