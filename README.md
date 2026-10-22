# Defense of the Dominion — desktop edition

A C++ / OpenGL 3.3 port of [Defense of the Dominion](https://github.com/nam-usth/defense-of-the-dominion), the StarCraft II–style
bunker defense game that runs in the browser. Same rules, waves, units, upgrades, cheats and HUD; the same SC2
models, textures, icons and music (`assets/`, copied from the web project).

## Build and run (Windows)

Needs a MinGW-w64 GCC (13+) and Ninja on `PATH` — or Strawberry Perl, whose bundled toolchain `build.ps1` finds
on its own.

```bash
powershell -ExecutionPolicy Bypass -File build.ps1
```

```bash
build/dotd.exe
```

The game finds `assets/` next to the executable or in a parent folder. Settings, saves and the log
(`dotd_desktop.log`) live in `%APPDATA%\DefenseOfTheDominion`. `CMakeLists.txt` is an alternative build for
CMake users; it was not tested here (no CMake on the build machine).

Controls are the web version's (click, or hold the button for full auto; R, N, T, B, 1–4, wheel, Insert/Delete,
Home, M/Esc, Q, Y, K; type `dotd` for the cheat console), plus **F3** for a performance overlay.

## Menu (M / Esc)

The web version's layout: two columns, the tabs and RESUME / RESTART / MAIN MENU on the left, the chosen tab's
page on the right, the same height on every tab. ↑ ↓ switch tabs; the last tab chosen opens again.

- **Game** — save / load (one slot; the game also autosaves after every cleared wave).
- **Settings** — quality HIGH / LOW, GPU particles, mute, music, volume, day/night cycle.
- **Display** (desktop only) — windowed or fullscreen, resolution (window presets that fit the monitor, or the
  monitor's video modes), applied at once: the HUD keeps its 1600×900 layout scaled to the height (its fonts are
  rebuilt). The graphics card on hybrid laptops, dedicated (e.g. NVIDIA) or integrated (Intel): the driver picks it
  when the game starts, so a change offers *Restart now* (default: dedicated). The frame-time overlay.
- **Controls** — the key list. **Tutorial** — hints on / off, HUD TOUR, START OVER. **Cheats** — the console.

## Tutorial

As in the web version: the first game opens with a HUD tour that spotlights the HP and ammo bars, minerals and
gas, the command card and each of its ten buttons (BACK / NEXT, OK, LET'S ROLL; Enter / Space / ← → step, Esc
ends it). After it, a hint pauses the game the first time a mechanic matters (out of ammo, wave cleared, upgrades,
roster, Roaches, nuke, hull, Stimpack); Enter / Space / Esc, the key it names, or a click elsewhere (after 0.8 s)
closes it. HOW TO PLAY on the title has the rules on one card. What has been seen is stored with the settings.

## Day / night and resupply

The day/night cycle (Y, or Menu → Settings) runs day 78 s → dusk 24 s → night 54 s → dawn 24 s, eased with a
smootherstep: the old curve jumped from full night back to day in one frame at the end of dusk. The fog radius
shrinks geometrically (90 → 9) so the night closes in evenly, the sun turns orange on the way, enemy HP bars fade
with the fog, and switching the cycle off lifts the night over 2.5 s, also behind the menu and title.

One crate per wave: the Viking's crate is promised when it launches, so a wave its bombs clear waits for it, and
a wave whose Viking crate already came in is refilled on the spot — a Marine fetches one crate, not two.

## What runs on which thread

| Thread | Work |
|---|---|
| main (GL) | simulation, draw submission, HUD (Dear ImGui), GPU uploads of finished assets (≤ 4 ms per frame) |
| 4 workers (`src/jobs.cpp`) | parsing OBJ / GLB models, decoding DDS / PNG / JPG, normalising geometry, extracting the hologram's feature edges |
| audio device init | opening the WASAPI output (≈ 1.2 s on the test laptop) without holding up the window |
| font atlas | building the glyph atlas while the game initialises |
| audio callback (miniaudio) | synthesising the effects, mixing, compressor |
| music decoder | AAC → PCM with Media Foundation, into a ring buffer |

Per-frame loops (particles, bullet hit tests) stay on the main thread on purpose: measured, a parallel-for there
made the frame slower (update 0.27 ms with 19 workers vs 0.11 ms serial in the heaviest scene below), because
the work is a few microseconds and waking threads costs more.

## GPU particles (OpenGL 4.3 compute shaders)

`src/gpu_particles.*`: the CPU queues bursts; one compute pass creates the particles in a ring buffer
(131 072 glow + 32 768 smoke), a second moves them (gravity, drag, bouncing on the ground), and the vertex
shader reads them straight from the buffer. Used for sparks (×6 the CPU count), Zerg deaths, acid splashes, and
explosions (fireball, sparks, embers, shockwave dust, smoke column — a nuke is ~57 000 particles). Menu →
Settings → GPU particles; without OpenGL 4.3 the game keeps the CPU particles. Cost (RTX 3050 Ti, 1600×900,
vsync off, averaged over the game): idle 2.99 ms on / 2.99 off; gunfire 4.10 / 3.93 ms; gunfire + 5 airstrikes
+ 3 nukes 6.1 / 3.5 ms — the extra is pixel fill of the explosions, not simulation (CPU update stays < 0.1 ms).

## Graphics card

The executable exports `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance`, which hybrid-graphics
drivers read when the OpenGL context is created; the menu's choice sets them before that. Heavy scene below,
vsync off, same laptop:

| | RTX 3050 Ti Laptop | Iris Xe |
|---|---|---|
| 1600×900 | 359 FPS (2.8 ms) | 226 FPS (4.4 ms) |
| 2800×1575 | 182 FPS (5.5 ms) | 128 FPS (7.8 ms) |

Starting on the NVIDIA card is slower (driver start-up, and a first texture upload that took 80–200 ms once
per run); on the Intel card the first frame came after ~0.2 s.

## Measurements (Intel Iris Xe laptop, 1600×900, GFX HIGH)

| | |
|---|---|
| launch → first frame | 0.45 s (1.65 s before the audio device and font atlas moved off the main thread) |
| launch → every asset loaded | 0.6 s |
| longest frame while loading | ≈ 50 ms (the first frames: GPU uploads) |
| heavy scene: wave 15, 53 enemies, 6 Marines + Shrike overdrive, ~1000 particles, vsync off | 263 FPS, 3.8 ms per frame; CPU: update 0.11 ms, render submit 0.16 ms, UI 1.2 ms — the GPU is the limit |

With vsync on (the default) the game runs at the monitor's refresh rate. These numbers come from the test
options below; the web version was not benchmarked side by side.

## Test options

`build/dotd.exe --size 1600x900 --ui-scale 1 --no-tutorial --start campaign --cheat zergrush --cheat fu --bench-fire --no-vsync --perf --after 8 --shot out.png`

`--start campaign|endless` starts once assets are loaded · `--cheat CODE[@S]` (repeatable; `@S`: S seconds after
the start) · `--key K@S` presses a key (a letter, a digit, enter, space, esc, left, right, up, down) ·
`--open roster|menu|cheat|menu:TAB|tour|tour:N|howto` (TAB: game, settings, display, controls, tutorial, cheats) ·
`--zoom-in N` · `--shot FILE --after S` saves a screenshot and exits · `--perf` overlay + log line ·
`--no-vsync` · `--threads N` (0 = no workers) · `--bench-fire` fires every volley automatically · `--ui-scale S` ·
`--gpu dedicated|integrated` · `--resize WxH` (after --start) · `--data DIR` settings and saves in DIR (a new
player; the real ones untouched) · `--no-tutorial` this run without the tour and hints · `--dn S` day/night on,
S seconds into the cycle · `--hold X,Y,S` holds the fire button at (X, Y) (fractions of the window) for S seconds
and logs the volleys fired. Each cleared wave logs how it was resupplied.

## Differences from the web version

- Desktop window, native OpenGL: three.js materials re-implemented in one shader (Standard / Lambert / Phong /
  Basic, SC2 team colour, DXT5nm normal maps), PCF sun shadows, radial fog, instanced enemies and particles.
- GFX LOW turns off shadows and 4× MSAA (the web LOW turned off shadows and HiDPI rendering).
- `help` in the cheat console lists the codes in the console itself (the web printed them to the browser console).
- Saves are `key=value` files in `%APPDATA%` instead of `localStorage`, so web saves do not carry over.
- Music decoding uses Windows Media Foundation: the game is Windows-only as written.

## Layout

```
src/main.cpp        window, GL context, ImGui, frame loop, test options
src/game.*          simulation and 3D scene (port of js/game.js)
src/game_ui.cpp     HUD, command card, roster, menu, tutorial, title / HOW TO PLAY / results (index.html + css/hud.css)
src/game_save.cpp   save / load, cheat codes
src/render.*        geometry, materials, shadows, instancing, particles
src/assets.*        DDS / PNG / JPG, OBJ / GLB loading on workers, feature edges
src/audio.*         synthesised effects, AAC music
src/jobs.*          worker pool
src/display.*       graphics cards (DXGI), resolutions, fullscreen, HUD scale, restart
src/ui.*            fonts, text, cut-corner panels, SVG icons
src/storage.*       %APPDATA% persistence
third_party/        GLFW 3.4 (+ its glad loader), Dear ImGui 1.91.9, cgltf 1.14, stb, miniaudio 0.11.21, glm 1.0.1
assets/             SC2 models, textures, icons and music
```

## Credits

Non-commercial fan project — not affiliated with or endorsed by Blizzard Entertainment. StarCraft® and the models,
textures, icons and music in `assets/` are © Blizzard Entertainment, Inc. The music is from the StarCraft
soundtrack by Glenn Stafford. The libraries in `third_party/` keep their own licenses (zlib, MIT, public domain).
