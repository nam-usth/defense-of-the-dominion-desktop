# Handoff: gameplay session → graphics session (2026-10-24)

What the gameplay session changed while the `gfx` branch (HDR post pipeline, rebuilt nuke)
was in progress, what now touches the rendering, and what is left open. Both editions:
desktop `F:\dotd_desktop` (C++/OpenGL) and web `F:\Code\Applications\defense-of-the-dominion`
(three.js r128). Delete this file once it has been read and acted on.

## Where things stand

| Repo | `main` | vs `origin/main` |
|---|---|---|
| desktop | `7ed6cf1` merge of the Linux port (`611ea6c`) | **4 ahead, not pushed**: your `d6dfc3d`, `d2b326d` (nuke on SC2 timing) + `611ea6c` + `7ed6cf1` |
| web | `81d8c5c` | in sync (pushed) |

- Pushing desktop `main` now pushes your two nuke commits too — your call (the user has not
  asked for a push yet).
- The gameplay work was committed as `055cbcd` (desktop) / `f39c457` (web) just before your
  `gfx` merges; the web follow-up is `81d8c5c`.
- The Linux port is merged: built and run on Windows after the merge, **not rebuilt on Linux
  since** — `post.cpp`, `nuke_fx.cpp` and the HDR / compute code have never been compiled
  there (WSL `Ubuntu-22.04`, CMake + ninja, see README "Build and run (Linux)").

## Build rule you must follow now

A new desktop source file goes into **both** build files:
- `build.ninja` — the Windows build (`build.ps1`);
- `CMakeLists.txt` — the CMake build the Linux port uses: one `add_executable(dotd ...)` around
  line 93, shared by both platforms (the `if(WIN32)` blocks around it pick GLFW's backend and
  the libraries); it already lists `src/post.cpp src/nuke_fx.cpp`.
Windows-only code goes behind `#ifdef _WIN32` (see `main.cpp`, `display.cpp`, `audio.cpp`).

## Rendering changes made on the gameplay side

### Small lights: Marauder grenades and their bursts (desktop `render.h` / `render.cpp`)
- `GlowLight {pos, color, radius, reveal}`, `SceneParams::glows[MAX_GLOWS = 8]`, `glowCount`.
- `LIT_FS`: uniforms `uGlPos[8]` (xyz + radius), `uGlColor[8]` (rgb + reveal), `uGlCount`.
  Each light adds soft half-wrapped diffuse (`(1 - d/r)^2`, no specular) to `direct`, **and lifts
  the radial night fog** around it: `fogF *= 1 - max(reveal * (1 - smoothstep(0.35r, r, d)))`.
- `PART_FS` (CPU sprites) honours the same lift, so the grenade burst sprites show in the dark.
- Filled each frame by `Game::updateGlows()` (game.cpp): bursts of the last 0.35 s first
  (`2.2 * f²`, radius = blast + 1), then grenades in flight (`MG_LIGHT_I = 0.9`,
  `MG_LIGHT_R = 2.4`, reveal 0.9); intensity × `(0.35 + 0.65 * dnFactor_)`.
- **Not honoured yet**: `gpu_particles.cpp` `DRAW_FS` (blood near a lit grenade still fades in the
  fog) and `nuke_fx.cpp` (own fog, fine — the nuke has its own light). If you touch the GPU
  particle shader, adding the same lift is a small, welcome change.
- In the HDR pipeline these lights, the additive blast glow and the blue flames are not tuned
  for bloom — worth a look.

Web: the same lights live in `js/radial-fog.js` — the fog chunk gets `glowPos[8]`,
`glowCol[8]`, `glowCount` (horizontal distance only) and adds `diffuseColor.rgb * glowLight`
before the fog mix. The uniforms reach **every** material through a wrapper installed on
`THREE.Material.prototype.onBeforeCompile` (a material's own hook, e.g. the SC2 materials in
`model-loader.js`, still runs first). Do not overwrite `onBeforeCompile` on the prototype; set it
per material as before. The particle `ShaderMaterial` (`PARTICLE_FS`) honours the lift too.
`syncGlows()` in game.js fills `GLOW_LIGHTS`.

### Night visibility follows the shader's fog
`Game::nightDark(x, z)` (web `_nightDark`) = the shader's `smoothstep(fogNear, fogFar, |xz|)`
minus the glow lift. It decides:
- enemy visibility at night within Sensor Tower range: shown only once `nightDark < 0.6`
  (`SENSOR_SHOW_DARK`), otherwise only its blip (no model, no HP bar — they would tell the
  kind);
- the blip's fade (`SENSOR_SHOW_DARK` → `+ SENSOR_BLIP_FADE 0.15`).
**If you change the fog curve** (`fogNear_`/`fogFar_`, the smoothstep, or tone mapping that
changes how dark "60% fogged" looks), update `nightDark` / `_nightDark` to match.

### Marauder grenade visuals (game.cpp render block, web `syncMissiles` / `syncBlasts`)
- Drawn as the Viking missile (`vmMesh_`, `MG_LEN 0.16`) with flame + light streak; icy blue
  (`mgBlueGlowMat_` / `mgBlueCoreMat_`, web `_mgFlame*IM` / `_mgStreak*IM`) once Concussive
  Shells is researched.
- Burst: a ball of light + a few sparks (CPU `glow_` sprites, `marauderBlast`) over a soft,
  edgeless ground glow (`MBlast`, `blastWashMat_` with `blastTex_`, 0.55 s). No ring.
- The old `marauderMissile` GLB is no longer loaded (still in `assets/`).

### HUD
- Sensor ring triangles: small equilateral (side 0.5), centred on the ring.
- STIM autocast mark: `autocastBorder()` (game_ui.cpp) — two gold dashes running round the
  button's cut-panel outline, 1.6 s a lap. Web: `.cmd-btn.autocast::after`, a conic gradient
  turned with `@property --ac-a`, masked to a 2 px ring.
- Research and armory cards have a fixed height (three description lines, clipped).

## Gameplay changes you may see while testing
- Marauders fire on their own (`marauderFireTick`): every 0.7 s each (stimmed ×1.5), reach 14,
  at the nearest enemy not slowed / not under a grenade already on its way; every 3 grenades
  cost 1 round. Click volleys are Marines and Ghosts only.
- Super Stimpack has 3 tiers: Marines needed 6 → 4 → 2 → 1 (the last: a Marine or a Marauder);
  right-click STIM toggles autocast.
- Elected crate runner: click a roster tile (gold border + crate glyph); `runnerIdx_`.
- Endless Magazine: +15 rounds, +15% price per purchase.

## Test hooks (desktop CLI)
`--cheat hire:marine|marauder|ghost`, `--cheat research:KEY[:TIER]` (after `--cheat mng`),
`--cheat autocast:stim`, `--cheat runner:N`, plus `--dn S` for night,
`--data DIR` for a scratch save folder. A scratch `DIR\prefs.txt` with `endless_unlocked=1`
lets `--start endless` run.

## Open, for the graphics side
1. Decide on pushing desktop `main` (4 commits, two of them yours).
2. Rebuild and run on Linux (WSL) now that HDR + the new nuke are in — the merge was only
   checked on Windows.
3. Optional: grenade lights in `gpu_particles.cpp`; HDR tuning of grenade light / blasts.
