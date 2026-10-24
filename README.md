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
page on the right, the same height on every tab. ↑ ↓ switch tabs; the last tab chosen opens again. On the title
screen, SETTINGS (beside HOW TO PLAY) opens it on the Settings tab, and M / Esc open it there too.

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
with the fog, and switching the cycle off lifts the night over 2.5 s, also behind the menu and title. The top bar
shows the phase and the countdown to the next one (`☀ DAY  DUSK IN 0:42`, then `NIGHT IN`, `DAWN IN`, `DAY IN`).

Shooting in the dark: tracers ignore the fog and burn white-hot in a soft halo over a faint pool of light on the
ground, so a volley shows where it goes past the lit edge; every round from the bunker and the Shrike draws the
same streak (1.8 × the tracer mesh), so both are as bright. Self-lit things (the nuke's laser, trail, reticle and
warning ring, the Viking's missiles) show through the night too.

Seeing the Zerg in the dark: the **Sensor Tower** research (roster, no unit needed, three levels) shows enemies
beyond the lit edge (~9) within 10.75 / 12.5 / 16 of the bunker as pulsing red **!** blips — not which kind —
lying flat on the ground under them (upright as the camera sees them), inside a glowing ring at its range marked
with 12 small equilateral triangles pointing at the bunker (cheat `sensor`; the icon is a placeholder glyph until the SC2 art is
exported). The ring fades in when researched, crossfades on an upgrade and follows the night out; a blip fades in
as its Zerg crosses into range and out only as the Zerg comes out of the dark — well inside the lit edge, where the
fog still leaves it 60% black (or in a Marauder grenade's light). Until then the Zerg itself is not drawn, nor its
HP bar (its size and ARM / LGT badge would tell which kind); it shows just as its blip has faded out, so there is
no gap and no Zerg seen under a blip. The Shrike turret engages blips as it does Zerg in
the light. An airstrike called at night drops a
parachute flare 0.6 s later that lights the whole spawn ring for 7.5 s of game time.

The airstrike: the Viking fires one air-to-ground missile per enemy cluster from the muzzles of its two launcher
tubes in turn (the long tubes either side of the nose); each runs ahead, then dives on its target, homing on the
lead enemy, with an exhaust flame, a light streak and a smoke trail, and strikes as the bombs did (radius 4.5).

The Marauder: its punisher grenade is drawn as the Viking's missile (a little larger), with the same exhaust flame
and light streak, burning icy blue once Concussive Shells is researched (as StarCraft II draws it). It is a small
weak light (reach 2.4, a soft falloff) that lights the ground, the Zerg and the rocks it passes and lifts the night
around it, so Zerg out in the dark show as it flies by; its burst lights the blast brighter and wider for 0.35 s
(the renderer's up to 8 small lights, which the sprites honour too). With every volley each Marauder picks its own target among the enemies in sight (at night: lit, or a
Sensor Tower blip; none in sight: the click, as everyone else): the nearest one not slowed yet and outside the blasts
already on their way (the grenades in flight and the volley's), else the nearest not slowed, else the nearest outside
them, else the nearest — so the slows spread over the swarm — and the grenade bursts on the first enemy in its path or on its target point: everyone else within
1.5 takes half the round's damage (double vs Armored, as the round), and all of them slow to 70% speed for 1.5 s.
Concussive Shells slows them to 40% for 2.5 s, and its AOE tiers widen the blast to 2.0 / 2.6 / 3.2. The burst
is a small ball of light, white-hot in a soft glow, with a few sparks (blue for a concussive shell, else warm),
over a soft glow on the ground across the blast radius that has no edge and dies away in half a second.

Stimpack (T) stims the Marines and the Marauders (SC2: 10 HP per Marine, 20 per Marauder, paid by the bunker; an
extra round or grenade every other volley). It needs six Marines in the bunker; after the Super Stimpack research
(no HP cost, repairs the bunker) its three tiers bring that down to 4, 2 and 1 Marines, so a Marine–Marauder squad
can stim. With Super Stimpack, right-clicking STIM turns on autocast, as in StarCraft II: the volley that finds it
ready stims first (quietly, the stim sound only), and two gold dashes run round the button's border while it is on
(saved with the game; `--cheat autocast:stim` in tests). The Endless armory's Magazine adds 15 rounds (half a starting magazine) and grows dearer by 15% a
purchase instead of 30%, so the magazine can keep growing.

The nuke: a mushroom cloud of fire as StarCraft II draws it — a cap rolling on a fiery stem, white-yellow inside,
orange at the edges, cooling to dark red and soot over 7 s — with a grey dust ring surging along the ground and
the shockwave; nothing is flung across the field (sparks, embers and debris were hard to read and cost ~30 000
GPU particles a blast). Its fireball lights the field: a warm point light, and at night the dark lifts (fog out
to ~40) and settles back as it cools (6 s).

A round hitting a Zerg spurts a little of its blood out the far side, instead of sparks in the round's colour
flung up and out, which read as fireworks; a Zerg dying leaves a low splash of blood and a little smoke (not 220–450
glowing drops and a burst of sparks), and one burnt by a nuke none.

One crate per wave: the Viking's crate is promised when it launches, so a wave its missiles clear waits for it,
and a wave whose Viking crate already came in is refilled on the spot — a Marine fetches one crate, not two. At
night the unit out on a crate run (Marine, Marauder or Ghost) wears a twin white LED pod on its chest armour:
two small soft hot spots in one soft glow, and a soft pool of light on the ground ahead of it with no edge anywhere
(it swells in past its feet, widens and dies away), as the Marines' flashlights in StarCraft II — and a real light:
the renderer's spot light (a 40° cone, full only within 7°, aimed 12° down, fading fast with distance) lights the
hull, the Zerg and the rocks it faces.

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
shader reads them straight from the buffer. Used for sparks (×6 the CPU count), blood on hits, Zerg deaths, acid
splashes, and Viking missile strikes (fireball, sparks, embers, shockwave dust, smoke column). The nuke uses none:
its mushroom cloud is ~250 soft sprites rebuilt on the CPU each frame, drawn with or without GPU particles.
Menu → Settings → GPU particles; without OpenGL 4.3 the game keeps the CPU particles. Cost (RTX 3050 Ti,
1600×900, vsync off, averaged over the game; measured before the nuke's cloud replaced its GPU particles and
hits spurted blood instead of sparks): idle 2.99 ms on / 2.99 off; gunfire 4.10 / 3.93 ms; gunfire + 5 airstrikes + 3 nukes 6.1 / 3.5 ms — the
extra is pixel fill of the explosions, not simulation (CPU update stays < 0.1 ms).

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
the start; also `hire:marine|marauder|ghost` and `research:KEY[:TIER]`, paid for: `--cheat mng` first) · `--key K@S` presses a key (a letter, a digit, enter, space, esc, left, right, up, down) ·
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
