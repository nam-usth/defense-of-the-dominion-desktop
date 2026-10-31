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
(`dotd_desktop.log`) live in `%APPDATA%\DefenseOfTheDominion`. `CMakeLists.txt` also builds on Windows, but
that half was not tested (no CMake on the build machine); it is the Linux build below.

## Build and run (Linux)

X11 and Wayland, GCC 11+ or Clang, CMake 3.16+ and Ninja. The packages on Debian / Ubuntu:

```bash
sudo apt install g++ cmake ninja-build pkg-config libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libwayland-dev libxkbcommon-dev libavformat-dev libavcodec-dev libavutil-dev fonts-dejavu
```

```bash
cmake -S . -B build/linux -G Ninja
```

```bash
cmake --build build/linux
```

```bash
build/linux/dotd
```

- **Windowing.** GLFW's X11 and Wayland backends are both built in. The game runs on X11 whenever `$DISPLAY`
  is set (XWayland on a Wayland desktop), because GLFW's Wayland backend can neither place the window nor change
  video modes, and the Display menu does both. `--platform wayland` runs it on Wayland natively.
  `-DDOTD_WAYLAND=OFF` or `-DDOTD_X11=OFF` leaves one backend out. GLFW opens libX11, libwayland-client, libGL
  and libEGL at run time, so the binary links none of them.
- **Music.** The tracks are AAC, the same files as the web and Windows versions, and FFmpeg's libavformat and
  libavcodec decode them. Linux has no system AAC decoder that miniaudio can use, and converting the tracks to
  Ogg or MP3 would add a second lossy 13 MB copy that has to be kept in step. FFmpeg comes with nearly every
  desktop Linux. The binary needs the FFmpeg major version it was built against, so build it on the machine that
  runs it. `-DDOTD_MUSIC=OFF` builds without FFmpeg: sound effects only, and the log says the music is off.
- **Fonts.** Bahnschrift, Consolas and Segoe UI Symbol are Windows fonts. The game uses them if they are
  installed (for example in `~/.local/share/fonts`). Otherwise it uses DejaVu Sans Condensed, DejaVu Sans Mono and
  DejaVu Sans, then Ubuntu, Liberation or Noto. The log names the fonts it picked.
- **Saves.** Settings, saves and the log are in `$XDG_DATA_HOME/DefenseOfTheDominion`, or
  `~/.local/share/DefenseOfTheDominion` if `XDG_DATA_HOME` is not set.
- **Graphics card.** The menu does not list the cards on Linux. To pick the dedicated one, start the game with
  `DRI_PRIME=1 build/linux/dotd` (Mesa: AMD, Intel, nouveau) or `prime-run build/linux/dotd` (NVIDIA's driver).

Tested on Ubuntu 22.04 under WSLg: GCC 11.4, FFmpeg 4.4, X11 through XWayland and native Wayland, sound
through PulseAudio, the HDR post-processing and the new nuke included. WSLg's Mesa D3D12 driver stops at OpenGL 4.1, so the game falls back to 3.3 with CPU
particles there (60–100 FPS at 1280×720). With Mesa's llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`, OpenGL 4.5), the GPU
particles' compute shaders compile and run. Not tested: a native Linux desktop, AMD / Intel / NVIDIA drivers on
Linux, PipeWire, and FFmpeg 5.1+. The code handles 5.1's channel-layout API but was compiled only against 4.4.

Controls are the web version's (click, or hold the button for full auto; R, N, T, B, 1–4, wheel, Insert/Delete,
Home, M/Esc, Q, Y, K; type `dotd` for the cheat console), plus **F3** for a performance overlay.

## Menu (M / Esc)

The web version's layout: two columns, the tabs and RESUME / RESTART / MAIN MENU on the left, the chosen tab's
page on the right, the same height on every tab. ↑ ↓ switch tabs; the last tab chosen opens again. On the title
screen, SETTINGS (beside HOW TO PLAY) opens it on the Settings tab, and M / Esc open it there too.

- **Game** — save / load (one slot; the game also autosaves after every cleared wave).
- **Settings** — quality HIGH / LOW, GPU particles, mute, music, volume, damage numbers (stored, on by default),
  day/night cycle, weather (stored, on by default).
- **Display** (desktop only) — windowed or fullscreen, resolution (window presets that fit the monitor, or the
  monitor's video modes), applied at once: the HUD keeps its 1600×900 layout scaled to the height (its fonts are
  rebuilt). The graphics card on hybrid laptops, dedicated (e.g. NVIDIA) or integrated (Intel): the driver picks it
  when the game starts, so a change offers *Restart now* (default: dedicated; on Linux the menu says how to pick
  the card instead). The frame-time overlay.
- **Controls** — the key list. **Tutorial** — hints on / off, HUD TOUR, START OVER. **Achievements** — the totals and
  every achievement as an icon; the one under the cursor is shown as its full row. **Cheats** — the console.

## Tutorial

As in the web version: the first game opens with a HUD tour that spotlights the HP and ammo bars, minerals and
gas, the command card and each of its ten buttons (BACK / NEXT, OK, LET'S ROLL; Enter / Space / ← → step, Esc
ends it). After it, a hint pauses the game the first time a mechanic matters (out of ammo, wave cleared, upgrades,
roster, Roaches, nuke, hull, Stimpack, and with Super Stimpack its right-click autocast); Enter / Space / Esc, the
key it names, or a click elsewhere (after 0.8 s) closes it (the autocast hint: a right-click on STIM too). HOW TO PLAY on the title has the rules on one card. What has been seen is stored with the settings.

## Achievements

27 achievements worth 600 points (StarCraft II's 10 / 20 / 30 / 50), in five categories:

| Category | Achievements |
|---|---|
| Campaign | Boot Camp (finish the HUD tour) · First Blood · Hold the Line (wave 5) · Dig In (wave 10) · The Dominion Stands (win the campaign) |
| Endless | No End in Sight (wave 20) · The Swarm Grows (25) · Legend of Korhal (30) |
| Combat | Exterminator (100 kills in a game) · Four Stars (GENERAL in a game) · Veteran (1,000 kills in all) · Hive Breaker (5,000 in all) · Nuclear Option (10 with one nuke) · Air Superiority (10 with one airstrike) · Shrapnel (5 with one Marauder grenade) · One Shot, One Kill (50 snipes in all) |
| Command | Fully Upgraded (damage, armor, hull, ammo maxed) · Shrike Online · Overdrive · Combined Arms (a Marine, a Marauder and a Ghost at once) · Super Soldiers (every Super Stimpack tier) · Eyes in the Dark (Sensor Tower 3) |
| Feats | Untouchable (a wave from 5 on without the Zerg damaging the bunker) · Last Stand (hidden) · Night Watch (a wave at night) · Band of Brothers (win without a Marauder or a Ghost) · No Air Support (win without an airstrike or a nuke) |

A pop-up below the top bar announces each one as it is earned (with a chime, over the roster and the menu too):
one box: the icon, the name, what it was for, and the points large with the date. The title's ACHIEVEMENTS button opens
the full list, a tab per category, each achievement drawn as StarCraft II does: the icon, a blue panel with the
name and what to do, and a box with the points and the date it was earned (`24 OCT 2026`) — or LOCKED, or the
progress (`412/1,000`) for those that count across games. The results screen shows the (up to three) best the
game earned.

**A cheat code turns achievements off for the rest of that game** (any code but `help`, before it takes effect):
the top bar shows a red CHEATS chip, the results screen says so, and the game counts toward nothing — not even
the lifetime kills (BOOT CAMP too: a tour finished in that game does not count). What was earned before the
cheat stays earned. The flag is saved with the game, so loading does not wash it out; a save made before
achievements existed has no flag, so its game earns none either (the chip reads OLD SAVE). The test hooks
`--bench-fire` and `grant:` count as cheats unless the run uses `--data`, so a test never earns on the real profile.
The rules live in `src/achievements.cpp`; what has been earned and the lifetime stats (kills, snipes) are stored
in `achievements.txt` beside the settings. `powershell -ExecutionPolicy Bypass -File build.ps1 -Test` builds and runs
their unit test (35 checks: every rule and its cheat gate, the toasts, storage round trips), which needs no window;
on Linux: `cmake --build build/linux --target ach_test && build/linux/ach_test`.

## Day / night and resupply

The day/night cycle (Y, or Menu → Settings) runs day 78 s → dusk 24 s → night 54 s → dawn 24 s, eased with a
smootherstep: the old curve jumped from full night back to day in one frame at the end of dusk. The fog radius
shrinks geometrically (90 → 9) so the night closes in evenly, the sun turns orange on the way, enemy HP bars fade
with the fog, and switching the cycle off lifts the night over 2.5 s, also behind the menu and title. The top bar
shows the phase and the countdown to the next one (`☀ DAY  DUSK IN 0:42`, then `NIGHT IN`, `DAWN IN`, `DAY IN`).

The field is drawn by the shader from three tiling 512² maps built at startup: a gradient noise with its broad
octaves domain-warped (no stripes, no straight edges), read at rotated scales of 150, 36 and 6 units, and two
detail maps, 12 000 short grass blades and 1 100 pebbles on crumbly earth, read at rotations and scales of 2.7 to
7.3 units that share no period. Two grass tones in soft patches with lighter and darker swathes, dirt patches
frayed by the blades with a darker seam, a worn ring round the bunker with shade at its foot. Every fine term
averages out, so a softer read keeps the tone. GFX HIGH draws it in HD: by each pixel's footprint, single blades
and pebbles with their own slope for the light close in, the blades reading larger as clumps from afar (the
overview stays calm, with no hatching). GFX LOW draws it in SD: the broad layers alone, a little softer.

On HIGH, 7 500 tufts of four blades stand on the grass out to r 30 (one static mesh, 90 000 triangles). Each takes
the colour of the ground at its root, dark at the foot and pale at the tip, and sways in slow gusts, harder in the
rain. There are none on bare earth, in water or where a nuke has scorched the field; they grow back as the scar
fades.

The sun is lower than it was (55° up, not 65°: longer shadows, the shapes stand out), and on HIGH soft cloud shadows
drift over the field with the wind: two reads of the noise at rotated scales, so no lattice shows as bands. The
ambient light depends on which way a surface faces, the sky above and the field's dim green bounce below, and polished
surfaces mirror the sky on a fresnel edge, so the bunker's metal follows the weather and the dusk.

Clusters of boulders lie out past the Sensor ring: three shapes, each a sphere chiselled by 16 random planes into
faces that meet in ridges (a corner's normal averages the faces within 40°, so the ridges stay creased). On HIGH they
are shaded as stone, from the ground's maps projected on three axes: mottled, grainy, faintly banded, lichen on the
faces turned up, darker at the foot. Every boulder, Zerg, corpse and unit out of the bunker sits on a soft contact
shadow. At night the field does not go black: the fog fades it to its own
grey, moon-blue value under a cool moon from behind left (no shadows), a Zerg in the dark fades to the ground
under it, the bunker's light is warm white, and the sky turns violet at dusk. Boulders stay paler than the
field in the dark, so none reads as a crouching Zerg.

Gunfire lights the ground: each shot from a fire port (or the Shrike on the roof) throws a warm pool of light
on the grass in front of it, out to about 7, each shot a little stronger or weaker and off to one side, so a
volley flickers rather than strobes (clear at night and dusk, a light touch by day). A burning bunker (below 50%
HP) lights the hull, the ground and the Zerg round it out to about 9 in a soft orange flicker on three octaves of
noise, stronger with the damage. Neither lifts the night: a Zerg in the dark stays hidden.

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

The Marauder fires on its own, not with the volleys: a punisher grenade every 0.7 s each (stimmed: 50% faster) at
enemies within 14, and every 3 grenades (the Marauders' together) cost 1 round of ammo — out of ammo, they stop
too. The grenade is drawn as the Viking's missile (a little larger), with the same exhaust flame and light streak,
burning icy blue once Concussive Shells is researched (as StarCraft II draws it). It is a small weak light (reach
2.4, a soft falloff) that lights the ground, the Zerg and the rocks it passes and lifts the night around it, so
Zerg out in the dark show as it flies by; its burst lights the blast brighter and wider for 0.35 s (the renderer's
up to 8 small lights, which the sprites honour too). Each Marauder picks its target among the enemies in sight (at
night: lit, or a Sensor Tower blip; none in sight: it holds its fire): the nearest one not slowed yet and outside
the blasts already on their way (the grenades in flight), else the nearest not slowed, else the nearest outside
them, else the nearest — so the slows spread over the swarm — and the grenade bursts on the first enemy in its
path or on its target point: everyone else within 1.5 takes half the round's damage (double vs Armored, as the
round), and all of them slow to 70% speed for 1.5 s. Concussive Shells slows them to 40% for 2.5 s, and its AOE
tiers widen the blast to 2.0 / 2.6 / 3.2. The burst is a small ball of light, white-hot in a soft glow, with a few
sparks (blue for a concussive shell, else warm), over a soft glow on the ground across the blast radius that has
no edge and dies away in half a second. Marines and Ghosts fire where you click; a hint introduces each unit the
first time it fights, and the roster hint sums up all three.

Stimpack (T) stims the Marines and the Marauders (SC2: 10 HP per Marine, 20 per Marauder, paid by the bunker;
Marines fire an extra round every other volley, Marauders their grenades 50% sooner). It needs six Marines in the
bunker; after the Super Stimpack research (no HP cost, repairs the bunker) its three tiers bring that down to 4, 2
and 1 — at the last a Marine or a Marauder, so even three Marauders can stim. With Super Stimpack, right-clicking
STIM turns on autocast, as in StarCraft II: the attack that finds it ready (a volley, or a Marauder's grenade)
stims first (quietly, the stim sound only), and two gold dashes run round the button's border while it is on
(saved with the game; `--cheat autocast:stim` in tests). The Endless armory's Magazine adds 15 rounds (half a
starting magazine) and grows dearer by 15% a purchase instead of 30%, so the magazine can keep growing.

The crate runner: click a unit in the roster to send it out for the supply crates (a gold border and a crate in
its corner; click it again for any unit); unelected, a Marine goes, else any free unit (`--cheat runner:N` in
tests). The research and armory cards keep one height whatever their text (three lines; longer is cut off).

The nuke: StarCraft II's, rebuilt on one closed-form timeline (src/nuke_fx.h) and timed from SC2's own (Nuke.m3).
The flash blows the field out to white through the HDR exposure (the HUD stays clear) while a six-rayed star opens
flat over the blast; then SC2's glow cards, a soft ball of light, swell round a soft fireball of overlapping fire
that rises, flattens and rolls into the mushroom cloud. As in SC2 the fire never turns to a grey cloud: it dims and
is gone by 5.4 s, and small embers keep rising off the crater until 9 s. Under the sprites, SC2's three rings of
fire roll round the cap (1.1-4.7 s), streaks of flame are pulled in toward ground zero (1.4-3 s), and the air
bends: a shock bubble swells out to the kill radius, a ripple runs over the ground behind it, heat shimmers round
the cap and over the crater until about 12 s (a distortion pass in the HDR post; none with `--post off`). A ragged
shock front races over the ground under a fading shock dome, and a slow ring of burning ground spreads behind it
(0.5-2.9 s); a ring of dust with a burning leading edge rolls out past the kill radius; the
Zerg catch fire in the flash and run on, slowing and staggering, until the front flings them out, burning and
charring, to smoulder out where they land; the shock booms as it
passes the bunker (no camera shake). It leaves a small pit of scorched earth whose cracks glow deep red, cool and
fade by 16 s. Its light (SC2's curve: full by 0.5 s, held to 1.8 s, out by 3.9 s) lights the field, and at night
the dark lifts (fog out to ~40) and settles back.


The scene renders into an HDR target (R11G11B10F, RGBA16F if refused; 4x MSAA on GFX HIGH): a bloom chain and a
hue-preserving tone curve composite it before the HUD (src/post.h). Lit surfaces stay as before; only emitters
(fire, tracers, lamps) go past 1 and bloom. `--post off` draws straight to the window as before.


A round hitting a Zerg spurts a little of its blood out the far side, instead of sparks in the round's colour
flung up and out, which read as fireworks; a Zerg dying leaves a low splash of blood and a little smoke (not 220–450
glowing drops and a burst of sparks), and one burnt by a nuke none.

One crate per wave: the Viking's crate is promised when it launches, so a wave its missiles clear waits for it,
and a wave whose Viking crate already came in is refilled on the spot — a Marine fetches one crate, not two. At
night the unit out on a crate run (Marine, Marauder or Ghost) wears a twin white LED pod on its chest armour:
two small soft hot spots in one soft glow, and a soft pool of light on the ground ahead of it with no edge anywhere
(it swells in past its feet, widens and dies away), as the Marines' flashlights in StarCraft II — and a real light:
the renderer's spot light (a 40° cone, full only within 7°, aimed 12° down, fading fast with distance) lights the
hull, the Zerg and the rocks it faces. The bunker blocks it (a cylinder fitted to its walls): a runner coming home lights the hull it faces, not the ground, Zerg or rocks behind it, and the pool on the ground is cut by the same shadow.

## Weather

The sky wanders at random on game time (`src/weather.*`): clear for 80–170 s, then clouds roll in over ~18 s and
stay 25–45 s, then a shower about half the time (40–90 s), else clear again; after a shower it mostly clears. A
new game (and the title) starts clear and dry, the first change 45–100 s in. A shower is a **drizzle** (strength
0.15–0.35: few fine, short, slow drops, a light wind) or a **downpour** (0.7–1: dense long streaks falling nearly
twice as fast, a stronger wind, the day darker and the distance hazed), even odds, and every 15–30 s it may turn from one
to the other; the strength also wavers a little as it falls. The rain has its own sound (band-limited noise and
the patter of near drops, as loud as it is heavy).

- **The rain** is particles without state: each drop is a quad the vertex shader builds from its index (a hash
  gives its place; the distance fallen and the wind's drift are accumulated on the CPU, so a change of speed never
  makes them jump). Up to 24 000 streaks in a box over the field (fixed in the world, and only 12 m high: drops
  passing just under the overview camera smear across the whole view) and 4 000 splash rings on the ground. No buffers, no compute
  shader: it works on OpenGL 3.3 and with GPU particles off. At night it shows only in the bunker's light.
- **Clouds** dim the sun (the shadows fade), flatten the day's light toward grey and hide part of the moon.
- **The ground** wets with the rain (soaked after ~12 s of a downpour), darkening (the dirt more than the grass)
  and taking a dull sheen. **Puddles** fill in the low ground, in the dirt patches and the worn ring round the
  bunker, never on the lush grass, with the rain that has fallen: full after about a minute of a downpour, hardly
  any from a drizzle. Once it stops, the ground dries in about a minute in the sun (two under clouds) and the
  puddles seep away more slowly. The water lies flat but for the rain's rings on it (fewer in a drizzle), mirrors
  the sky (blue, or grey under clouds, with the cloud deck drifting over it) and gives sharp glints of every light:
  the sun after a shower, the bunker's lamp and grenades at night. The reflection is one function in the terrain
  shader (`skyMirror`): a traced reflection of the scene would return what the reflected ray hits there, and the
  sky where it escapes. The night keeps its readable moonlit value: the dark fades to the dry ground's.
- **The aim**: in the rain 5% of the rounds from the bunker and the Shrike miss in a drizzle, 10% in a downpour
  (in between as the rain grows): a missed round flies on past its Zerg (it may hit one behind) and a pale blue
  MISS rises. Marauder grenades still burst; Ghost snipes, Viking missiles and nukes still strike. The status line
  names the shower and its miss chance when it starts.

Menu → Settings → Weather switches it off (the sky clears, the ground dries four times as fast). Cheat `rain`, in
turn: a downpour now, a drizzle, clear. Cost (heavy scene below, 1600×900, HIGH, vsync off, a downpour over full
puddles against a clear sky; the laptop was warm, the runs noisy): about +0.3–0.45 ms a frame on the RTX 3050 Ti
and +1 ms on the Iris Xe, all of it on the GPU (the CPU's render submit stays the same).

## What runs on which thread

| Thread | Work |
|---|---|
| main (GL) | simulation, draw submission, HUD (Dear ImGui), GPU uploads of finished assets (≤ 4 ms per frame) |
| 4 workers (`src/jobs.cpp`) | parsing OBJ / GLB models, decoding DDS / PNG / JPG, normalising geometry, extracting the hologram's feature edges |
| audio device init | opening the WASAPI output (≈ 1.2 s on the test laptop; PulseAudio / ALSA on Linux) without holding up the window |
| font atlas | building the glyph atlas while the game initialises |
| audio callback (miniaudio) | synthesising the effects, mixing, compressor |
| music decoder | AAC → PCM with Media Foundation (Linux: FFmpeg), into a ring buffer |

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
| 1600×900 | 191 FPS (5.2 ms) | 137–220 FPS (4.5–7.3 ms) |
| 2800×1575 | 74 FPS (13.5 ms) | 53–91 FPS (11–19 ms) |

The Iris Xe's range is the same build run repeatedly: its speed swings with the chip's heat. The overview; close up
(`--zoom-in 8`) the RTX draws 161 and 70 FPS. Before the detailed ground, the 3D grass and the stone (commit eda26bf, the
same runs): 280 and 113 FPS on the RTX, 193–210 and 76–79 FPS on the Iris Xe. Most of the new cost is the ground's
detail at 2800×1575, which takes 6 ms on the RTX; the grass takes about 1 ms.

Starting on the NVIDIA card is slower (driver start-up, and a first texture upload that took 80–200 ms once
per run); on the Intel card the first frame came after ~0.2 s.

## Measurements (Intel Iris Xe laptop, 1600×900, GFX HIGH)

| | |
|---|---|
| launch → first frame | 0.45 s (1.65 s before the audio device and font atlas moved off the main thread) |
| launch → every asset loaded | 0.6 s |
| longest frame while loading | ≈ 50 ms (the first frames: GPU uploads) |
| heavy scene: wave 15, 53 enemies, 6 Marines + Shrike overdrive, ~1000 particles, vsync off | 137–220 FPS, 4.5–7.3 ms per frame; CPU: update 0.04 ms, render submit 0.16 ms, UI 0.9 ms — the GPU is the limit |

With vsync on (the default) the game runs at the monitor's refresh rate. These numbers come from the test
options below; the web version was not benchmarked side by side.

## Test options

`build/dotd.exe --size 1600x900 --ui-scale 1 --no-tutorial --start campaign --cheat zergrush --cheat fu --bench-fire --no-vsync --perf --after 8 --shot out.png`

`--start campaign|endless` starts once assets are loaded · `--cheat CODE[@S]` (repeatable; `@S`: S seconds after
the start; also `hire:marine|marauder|ghost` and `research:KEY[:TIER]`, paid for: `--cheat mng` first, or
`grant:M,G`, test money that is not a cheat under `--data`) · `--key K@S` presses a key (a letter, a digit, enter, space, esc, left, right, up, down) ·
`--open roster|menu|cheat|menu:TAB|tour|tour:N|howto|achievements[:N]` (TAB: game, settings, display, controls, tutorial,
achievements, cheats; N: the achievement list's category tab, 0-4) ·
`--zoom-in N` · `--shot FILE --after S` saves a screenshot and exits · `--perf` overlay + log line ·
`--no-vsync` · `--threads N` (0 = no workers) · `--bench-fire` fires every volley automatically (without `--data`: no achievements) · `--ui-scale S` ·
`--gpu dedicated|integrated` (Windows) · `--platform x11|wayland` (Linux) · `--resize WxH` (after --start) ·
`--data DIR` settings and saves in DIR (a new player; the real ones untouched) · `--no-tutorial` this run without
the tour and hints · `--dn S` day/night on,
S seconds into the cycle · `--weather clear|overcast|rain|drizzle|after` that weather at once, held for the run
(rain: a downpour over full puddles; after: a shower just over, full puddles under the sun) ·
`--dump-ground DIR` writes the ground's three maps as PNGs and exits (the web version's `assets/textures`) ·
`--hold X,Y,S` holds the fire button at (X, Y) (fractions of the window) for S seconds
and logs the volleys fired · `--post on|off` the HDR post-processing · `--hp F` holds the bunker at F (0–1) of its
max HP for the run (below 0.5 it burns). Each cleared wave logs how it was resupplied.

## Differences from the web version

- Desktop window, native OpenGL: three.js materials re-implemented in one shader (Standard / Lambert / Phong /
  Basic, SC2 team colour, DXT5nm normal maps), PCF sun shadows, radial fog, instanced enemies and particles.
- The graphics are the same on both: the field (from the same three maps, `--dump-ground`), grass, boulders, sun, cloud
  shadows, moonlit night, wet ground and puddles, HDR bloom and tone curve. GFX LOW turns off shadows, 4× MSAA, the grass
  and the cloud shadows and draws the field in SD on both, and the web LOW also drops HiDPI rendering. The web HIGH
  caps the pixel ratio at 1.5 and gives up 4× MSAA for the session under 45 FPS; lit surfaces on Lambert materials
  there take no cloud shadow (they are lit per vertex).
- Damage numbers ride above their Zerg's HP bar as a two-row queue: a new one comes in just over the bar and
  pushes the older up, and the one pushed out fades; the web draws each where the round hit, so a burst on one
  Zergling runs together. Menu → Settings → Damage numbers hides them (and MISS); the HP bars stay.
- `help` in the cheat console lists the codes in the console itself (the web printed them to the browser console).
- Achievements (desktop only so far; a cheat code turns them off for that game).
- Saves are `key=value` files in `%APPDATA%` (Linux: `~/.local/share`) instead of `localStorage`, so web saves
  do not carry over.
- Windows and Linux. Music decoding uses Media Foundation on Windows and FFmpeg on Linux (see Build and run
  (Linux)). macOS is not supported: no Cocoa build and no AAC decoder there.

## Layout

```
src/main.cpp        window, GL context, ImGui, frame loop, test options
src/game.*          simulation and 3D scene (port of js/game.js)
src/game_ui.cpp     HUD, command card, roster, menu, tutorial, title / HOW TO PLAY / results, achievements list and pop-ups (index.html + css/hud.css)
src/game_save.cpp   save / load, cheat codes
src/render.*        geometry, materials, shadows, instancing, particles
src/weather.*       the weather's state, the rain (the wet ground and puddles: render.cpp's terrain shader)
src/assets.*        DDS / PNG / JPG, OBJ / GLB loading on workers, feature edges
src/audio.*         synthesised effects, AAC music (Media Foundation / FFmpeg)
src/jobs.*          worker pool
src/display.*       graphics cards (DXGI), resolutions, fullscreen, HUD scale, restart
src/ui.*            fonts, text, cut-corner panels, SVG icons
src/storage.*       %APPDATA% / XDG persistence
src/achievements.*  achievements: the list, the rules, lifetime stats, achievements.txt
tests/ach_test.cpp  their unit test (build.ps1 -Test)
third_party/        GLFW 3.4 (+ its glad loader and Wayland protocol XML), Dear ImGui 1.91.9, cgltf 1.14, stb,
                    miniaudio 0.11.21, glm 1.0.1
assets/             SC2 models, textures, icons and music
```

## Credits

Non-commercial fan project — not affiliated with or endorsed by Blizzard Entertainment. StarCraft® and the models,
textures, icons and music in `assets/` are © Blizzard Entertainment, Inc. The music is from the StarCraft
soundtrack by Glenn Stafford. The libraries in `third_party/` keep their own licenses (zlib, MIT, public domain).
