// Simulation and 3D scene — a port of js/game.js (the HUD lives in game_ui.cpp,
// saving and cheats in game_save.cpp).
#include "game.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "audio.h"
#include "config.h"
#include "log.h"

static const float PI = PI_F;

// ─── Units and research ──────────────────────────────────────────────────────
// SC2 unit attributes drive damage bonuses: Zergling is Light, Roach Armored.
static const UnitDef UNIT_DEFS[3] = {
  {1, {50, 0}, 1.0f, 0xffee44, 0.65f, false, false, 1.0f, "M", "MARINE", "marine"},
  {2, {150, 25}, 1.2f, 0xff8800, 0.5f, true, false, 2.0f, "Ma", "MARAUDER", "marauder"},   // +100% vs Armored
  {2, {200, 50}, 1.4f, 0xcc44ff, 0.75f, false, true, 2.5f, "Gh", "GHOST", "ghost"},        // +150% vs Light
};
const UnitDef& unitDef(Unit u) { return UNIT_DEFS[(int)u]; }

static const float SNIPE_CD[4] = {8, 6, 4, 2.5f};            // s per snipe level
static const float CONC_RADIUS[4] = {0, 1.5f, 2.2f, 3.0f};   // concussive AOE radius per level
static const uint32_t STIM_COLOR = 0xff6a3a;
static const uint32_t SHRIKE_GOLD = 0xffb81f;
static const float AIM_Y = 0.45f, PORT_Y = 0.62f;
namespace shrike {
const float interval = 0.3f, range = 14, overdriveInterval = 0.12f, omega = 20, maxTurn = 14, sink = -0.05f;
const uint32_t overdriveColor = 0xff2a1a;
}
// One cycle (3 min): day 78 s → dusk 24 s → night 54 s → dawn 24 s
static const float DN_DAY = 78, DN_FADE = 24, DN_NIGHT = 54, DN_CYCLE = DN_DAY + DN_FADE + DN_NIGHT + DN_FADE;
static const float DN_SLEW = 2.5f;   // turning the cycle on / off blends over at most this many seconds
static const float DN_NIGHT_FOG_NEAR = 2.5f, DN_NIGHT_FOG_FAR = 9.0f, DN_DAY_FOG_NEAR = 45, DN_DAY_FOG_FAR = 90;
// Viking flare: burns FLARE_DUR s of game time and lifts the night to FLARE_F at full
// strength (fog far ≈ 21.6: the whole spawn ring shows)
static const float FLARE_DUR = 7.5f, FLARE_F = 0.62f;
static const float FIRE_HP = 0.5f;
static const float FIRE_SPOTS_XZ[6][2] = {{0.55f, 0.35f}, {-0.6f, -0.3f}, {0.1f, -0.8f}, {-0.4f, 0.75f}, {0.95f, -0.35f}, {-1.0f, 0.15f}};
static const float NUKE_RADIUS = 16, WARN_DUR = 2.0f;
// The warhead leaves the sky (above the overview camera, y 22) at NUKE_FALL_AT s
// and lands at WARN_DUR; its trail fades over NUKE_TRAIL_FADE s after the blast
static const float NUKE_SKY_Y = 30, NUKE_FALL_AT = 0.4f, NUKE_TRAIL_FADE = 0.5f;
static const float NUKE_SLANT = 0.55f;   // the dive path's lean: horizontal per vertical (29° off vertical)
// The fireball's light: flashes up, holds NUKE_LIGHT_HOLD s, then cools (gone after NUKE_LIGHT_DUR s);
// at full strength it lifts a night to NUKE_LIGHT_F (fog far ≈ 40: past the spawn ring)
static const float NUKE_LIGHT_DUR = 6, NUKE_LIGHT_HOLD = 0.5f, NUKE_LIGHT_F = 0.35f;
static const float MUSH_DUR = 7;   // the mushroom cloud rises, spreads, cools and thins out over this many seconds
// Viking missiles: VM_FLIGHT s from the wing to the target, running VM_RUN ahead before the dive
static const float VM_FLIGHT = 0.6f, VM_RUN = 4;
// its length (a tenth of the punisher grenade model it first borrowed, scaled 1.7) and the centre → nozzle distance
static const float VM_LEN = 0.13f, VM_TAIL = 0.5f * VM_LEN;
static const float VM_STREAK = 0.25f;   // its light streak covers the last this-many seconds of the path

static float angDiff(float a, float b) { return std::atan2(std::sin(a - b), std::cos(a - b)); }
static glm::vec3 lerp3(const glm::vec3& a, const glm::vec3& b, float t) { return a + (b - a) * t; }

// three.js Object3D matrix: T · Rx · Ry · Rz · S (Euler order XYZ)
static glm::mat4 trs(const glm::vec3& p, const glm::vec3& rot = glm::vec3(0), const glm::vec3& s = glm::vec3(1)) {
  glm::mat4 m = glm::translate(glm::mat4(1), p);
  if (rot.x) m = glm::rotate(m, rot.x, {1, 0, 0});
  if (rot.y) m = glm::rotate(m, rot.y, {0, 1, 0});
  if (rot.z) m = glm::rotate(m, rot.z, {0, 0, 1});
  return glm::scale(m, s);
}

// Shared Lambert / Basic materials by colour (procedural shapes)
static std::map<uint32_t, std::unique_ptr<Material>> g_lam, g_bas;
static const Material* lamMat(uint32_t col) {
  auto& m = g_lam[col];
  if (!m) { m = std::make_unique<Material>(); m->type = MatType::Lambert; m->color = hexColor(col); }
  return m.get();
}
static const Material* basMat(uint32_t col) {
  auto& m = g_bas[col];
  if (!m) { m = std::make_unique<Material>(); m->type = MatType::Basic; m->color = hexColor(col); }
  return m.get();
}

// Procedural template builder: parts placed by a matrix, merged into one mesh per material
struct ProcBuilder {
  struct Item { const Material* mat; Geometry g; bool cast; };
  std::vector<Item> items;
  void add(Geometry g, const Material* mat, const glm::mat4& m, bool cast) {
    g = g.nonIndexed();
    g.transform(m);
    for (Item& it : items)
      if (it.mat == mat) { it.g.append(g); it.cast = it.cast || cast; return; }
    items.push_back({mat, std::move(g), cast});
  }
  Baked bake() {
    Baked b;
    for (Item& it : items) b.push_back({Mesh::create(it.g), it.mat, it.cast});
    return b;
  }
};

// ─── Init / scene ────────────────────────────────────────────────────────────
bool Game::init(GLFWwindow* win) {
  win_ = win;
  if (!R.init()) return false;
  glow_.init(3000);    // sparks, debris, flames, embers (additive)
  smoke_.init(800);    // smoke (normal blending)
  audio::music("title");
  buildScene();
  gfxHigh_ = storage::get("gfx", "HIGH") != "LOW";
  gpuFx_.init(131072, 32768);   // false without OpenGL 4.3: the CPU particles stay
  gpuFxOn_ = storage::get("gpuParticles", "1") != "0";
  R.setGpuParticles(gpuFx() ? &gpuFx_ : nullptr);
  applyDNLights(0);
  tutLoad();
  roster_ = {Unit::Marine, Unit::Marine};
  syncMarines();
  showTitle();
  return true;
}

void Game::buildScene() {
  // Ground: large enough that its edge is lost in the fog from the low close-up angle
  Geometry g = geo::plane(120, 120);
  g.transform(trs({0, 0, 0}, {-PI / 2, 0, 0}));
  groundMesh_ = Mesh::create(g);
  groundMat_.type = MatType::Phong;
  groundMat_.color = hexColor(0x2d4a18);
  groundMat_.specular = hexColor(0x080808);
  groundMat_.shininess = 4;
  // Dirt patches + rocks, merged: one mesh per material
  Geometry dirt, rocks;
  for (int i = 0; i < 20; i++) {
    Geometry p = geo::plane(rnd() * 2 + .4f, rnd() * 1.2f + .3f).nonIndexed();
    p.transform(trs({(rnd() - .5f) * 36, .01f, (rnd() - .5f) * 22}, {-PI / 2, 0, 0}));
    dirt.append(p);
  }
  for (int i = 0; i < 16; i++) {
    float a = i / 16.f * PI * 2, r = 14 + rnd() * 2.5f;
    Geometry p = geo::dodecahedron(rnd() * .5f + .25f);
    p.transform(trs({std::cos(a) * r, .3f, std::sin(a) * r}, {rnd(), rnd(), rnd()}));
    rocks.append(p);
  }
  dirtMesh_ = Mesh::create(dirt);
  rockMesh_ = Mesh::create(rocks);
  dirtMat_ = groundMat_;
  dirtMat_.color = hexColor(0x4a3a18);
  rockMat_.type = MatType::Lambert;
  rockMat_.color = hexColor(0x554433);

  // Procedural stand-ins (used until / unless an SC2 model loads)
  {
    ProcBuilder b;   // bunker
    b.add(geo::box(3, 1.5f, 3), lamMat(0x556644), trs({0, .75f, 0}), true);
    b.add(geo::box(3.4f, .3f, 3.4f), lamMat(0x445533), trs({0, 1.55f, 0}), false);
    for (int i = 0; i < 14; i++) {
      float a = i / 14.f * PI * 2;
      b.add(geo::sphere(.28f, 6, 4), lamMat(0x8a7a50), trs({std::cos(a) * 2.1f, .18f, std::sin(a) * 2.1f}, {}, {1, .55f, 1}), false);
    }
    bunkerProc_ = b.bake();
  }
  const uint32_t mcols[6] = {0x224488, 0x334499, 0x1a3377, 0x2a3388, 0x1a4488, 0x223366};
  for (int i = 0; i < 6; i++) {
    ProcBuilder b;
    b.add(geo::box(.44f, .6f, .34f), lamMat(mcols[i]), trs({0, .55f, 0}), true);
    b.add(geo::sphere(.21f, 8, 6), lamMat(mcols[i]), trs({0, 1.04f, 0}), false);
    b.add(geo::box(.07f, .07f, .48f), lamMat(0x111111), trs({.27f, .73f, .18f}), false);
    marineProc_[i] = b.bake();
  }
  for (int tier = 0; tier < 3; tier++) {   // zergling by wave tier (wave 4+, 7+)
    ProcBuilder b;
    uint32_t col = tier == 2 ? 0x332200 : tier == 1 ? 0x1a2a0a : 0x223322;
    b.add(geo::box(.65f, .38f, .85f), lamMat(col), trs({0, .3f, 0}), true);
    b.add(geo::box(.45f, .35f, .45f), lamMat(0x2a3d22), trs({0, .48f, .38f}), true);
    for (float ex : {-0.2f, 0.2f}) b.add(geo::sphere(.065f, 5, 4), basMat(tier == 2 ? 0xff8800 : 0xff2200), trs({ex, .55f, .62f}), false);
    for (auto l : {glm::vec2(-.22f, -.2f), glm::vec2(.22f, -.2f), glm::vec2(-.22f, .18f), glm::vec2(.22f, .18f)})
      b.add(geo::box(.1f, .28f, .1f), lamMat(0x1a2a1a), trs({l.x, .14f, l.y}), false);
    for (int i = 0; i < 4; i++) b.add(geo::cone(.06f, .24f, 4), lamMat(0x334433), trs({0, .65f, -.22f + i * .16f}), false);
    zerglingProc_[tier] = b.bake();
  }
  {
    ProcBuilder b;   // roach
    b.add(geo::box(1.1f, .6f, 1.3f), lamMat(0x3a2200), trs({0, .4f, 0}), true);
    b.add(geo::box(1.f, .35f, 1.1f), lamMat(0x5a3300), trs({0, .78f, 0}), true);
    b.add(geo::box(.7f, .4f, .55f), lamMat(0x4a2800), trs({0, .55f, .72f}), true);
    for (float ex : {-0.18f, 0.18f}) b.add(geo::sphere(.09f, 5, 4), basMat(0xff6600), trs({ex, .68f, .98f}), false);
    for (auto l : {glm::vec2(-.4f, -.3f), glm::vec2(.4f, -.3f), glm::vec2(-.4f, .3f), glm::vec2(.4f, .3f), glm::vec2(-.3f, 0), glm::vec2(.3f, 0)})
      b.add(geo::box(.14f, .22f, .14f), lamMat(0x2a1800), trs({l.x, .18f, l.y}), false);
    roachProc_ = b.bake();
  }
  {
    ProcBuilder b;   // viking
    b.add(geo::box(.5f, .25f, 1.6f), lamMat(0x8899aa), glm::mat4(1), false);
    b.add(geo::box(2.2f, .1f, .7f), lamMat(0x7788aa), trs({0, 0, -.2f}), false);
    b.add(geo::sphere(.22f, 8, 6), lamMat(0x334455), trs({0, .18f, .55f}), false);
    for (float ex : {-0.9f, 0.9f}) {
      b.add(geo::cylinder(.12f, .15f, .5f, 7), lamMat(0x556677), trs({ex, -.05f, -.4f}, {PI / 2, 0, 0}), false);
      b.add(geo::sphere(.1f, 6, 4), basMat(0x4488ff), trs({ex, -.05f, -.66f}), false);
    }
    b.add(geo::box(.08f, .4f, .45f), lamMat(0x778899), trs({0, .2f, -.7f}), false);
    vikingProc_ = b.bake();
  }

  // Effects
  bulletMesh_ = Mesh::create(geo::box(.07f, .07f, .5f));   // tracer streak, long axis = flight direction
  bulletMat_.type = MatType::Basic;
  bulletMat_.fog = false;   // a tracer burns: it shows out in the dark
  acidMesh_ = Mesh::create(geo::icosahedron(.11f, 1));
  acidMat_.type = MatType::Basic;
  acidMat_.color = hexColor(0x9cff3a);
  crateMesh_ = Mesh::create(geo::box(.65f, .65f, .65f));
  crateMat_ = *lamMat(0xaa8800);
  buildNightFx();
  beamMesh_ = Mesh::create(geo::cylinder(.25f, .6f, 24, 8));
  ringMesh_ = Mesh::create(geo::ring(.95f, 1, 32));
  laserMesh_ = Mesh::create(geo::cylinder(.04f, .04f, 28, 5));
  trailMesh_ = Mesh::create(geo::cylinder(.25f, 1, 1, 10));   // the nuke's streak: unit height, narrowing to the top
  fireballMesh_ = Mesh::create(geo::sphere(4, 12, 8));
  snipeMesh_ = Mesh::create(geo::cylinder(.04f, .04f, 1, 4));   // scaled to the shot's length
  dotRingMesh_ = Mesh::create(geo::ring(.3f, .6f, 16));
  decalMesh_ = Mesh::create(geo::plane(2.6f, 2.6f));
  missileBodyMat_.type = MatType::Standard;
  missileBodyMat_.color = hexColor(0x6b7078);
  missileBodyMat_.metalness = .6f;
  missileBodyMat_.roughness = .35f;
  missileBodyMat_.emissive = hexColor(0x301000);
  missileGlowMat_.type = MatType::Basic;
  missileGlowMat_.color = hexColor(0xff9a3a);
  missileGlowMat_.transparent = true;
  missileGlowMat_.opacity = .9f;
  missileGlowMat_.blend = Blend::Additive;
  missileGlowMat_.depthWrite = false;
  missileGlowMat_.fog = false;
  nukeTargetTex_ = loadTexture(NUKE_TARGET_TEX);
  if (!nukeTargetTex_) logWarn("nuke target texture failed to load — using a plain ring");
  rebuildBunkerTris();
}

// The night effects' textures (drawn here: no art for them), shapes and materials
void Game::buildNightFx() {
  // soft round glow: white, its alpha falling to 0 at the rim (the quads stretch it into streaks)
  {
    const int N = 64;
    std::vector<uint8_t> px(N * N * 4);
    for (int j = 0; j < N; j++)
      for (int i = 0; i < N; i++) {
        float x = (i + .5f) / N * 2 - 1, y = (j + .5f) / N * 2 - 1, k = std::max(0.f, 1 - std::sqrt(x * x + y * y));
        uint8_t* p = &px[(j * N + i) * 4];
        p[0] = p[1] = p[2] = 255;
        p[3] = (uint8_t)std::lround(255 * k * k);
      }
    glowTex_ = Texture::create(N, N, px.data());
  }
  // Sensor Tower blip: a red disc and ring around a pale "!" (rows bottom-up: +y is the top of the glyph)
  {
    const int N = 128;
    std::vector<uint8_t> px(N * N * 4);
    const float aa = 1.5f / N;   // edge softness in the [-1, 1] square
    auto cover = [&](float d) { return glm::clamp(0.5f - d / aa, 0.f, 1.f); };   // d: signed distance, < 0 inside
    const glm::vec3 red = hexColor(0xff3b2f), pale = hexColor(0xffd0c8);
    for (int j = 0; j < N; j++)
      for (int i = 0; i < N; i++) {
        float x = (i + .5f) / N * 2 - 1, y = (j + .5f) / N * 2 - 1, r = std::hypot(x, y);
        float a = std::max(0.22f * cover(r - 0.8f), cover(std::abs(r - 0.8f) - 0.07f));   // fill, ring
        float bar = std::hypot(x, y - glm::clamp(y, -0.02f, 0.46f)) - 0.09f, dot = std::hypot(x, y + 0.33f) - 0.1f;
        float g = std::max(cover(bar), cover(dot));
        glm::vec3 c = lerp3(red, pale, g);
        uint8_t* p = &px[(j * N + i) * 4];
        p[0] = (uint8_t)std::lround(255 * c.r);
        p[1] = (uint8_t)std::lround(255 * c.g);
        p[2] = (uint8_t)std::lround(255 * c.b);
        p[3] = (uint8_t)std::lround(255 * std::max(a, g));
      }
    blipTex_ = Texture::create(N, N, px.data());
  }
  // flat quads lying in the xz plane (normal +y), the texture's top toward -z
  Geometry halo = geo::plane(.5f, 1.6f);   // tracer halo: long axis = flight direction
  halo.transform(trs({0, 0, 0}, {-PI / 2, 0, 0}));
  glowQuadMesh_ = Mesh::create(halo);
  Geometry blip = geo::plane(1, 1);
  blip.transform(trs({0, 0, 0}, {-PI / 2, 0, 0}));
  blipMesh_ = Mesh::create(blip);
  {   // Viking missile, unit length along +z, nozzle at z = -0.5: body, nose cone, four tail fins
    Geometry m = geo::cylinder(.09f, .09f, .72f, 10);
    m.transform(trs({0, 0, -.14f}, {PI / 2, 0, 0}));   // cylinder / cone axis y → +z
    Geometry nose = geo::cone(.09f, .28f, 10);
    nose.transform(trs({0, 0, .36f}, {PI / 2, 0, 0}));
    m = m.nonIndexed();
    m.append(nose.nonIndexed());
    for (int k = 0; k < 4; k++) {
      float a = k * PI / 2;
      Geometry fin = geo::box(.02f, .14f, .2f);
      fin.transform(trs({std::cos(a) * .15f, std::sin(a) * .15f, -.38f}, {0, 0, a - PI / 2}));
      m.append(fin.nonIndexed());
    }
    vmMesh_ = Mesh::create(m);
    streakMesh_ = Mesh::create(geo::cylinder(1, 1, 1, 6));
    vmBodyMat_.type = MatType::Standard;
    vmBodyMat_.color = hexColor(0xc4c9d0);
    vmBodyMat_.metalness = .5f;
    vmBodyMat_.roughness = .4f;
  }
  // Sensor Tower range ring, per level: a soft glowing band with a brighter core (the glow texture's
  // centre column gives the profile across it) and a triangle every 30°, its tip toward the bunker
  for (int lvl = 1; lvl <= 3; lvl++) {
    const float R = cfg::sensor_range[lvl];
    Geometry g;
    auto vtx = [&](float x, float z, float v) { g.v.push_back({{x, 0, z}, {0, 1, 0}, {0.5f, v}}); };
    auto band = [&](float w) {   // inner edge v = 0, outer v = 1
      const int S = 160;
      for (int i = 0; i < S; i++) {
        float a0 = i * 2 * PI / S, a1 = (i + 1) * 2 * PI / S, ri = R - w, ro = R + w;
        float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
        vtx(c0 * ri, s0 * ri, 0); vtx(c0 * ro, s0 * ro, 1); vtx(c1 * ro, s1 * ro, 1);
        vtx(c0 * ri, s0 * ri, 0); vtx(c1 * ro, s1 * ro, 1); vtx(c1 * ri, s1 * ri, 0);
      }
    };
    band(0.45f);   // glow
    band(0.12f);   // core
    for (int k = 0; k < 12; k++) {
      float a = k * PI / 6, c = std::cos(a), s = std::sin(a);   // radial (c, s), tangent (-s, c)
      const float rBase = R + 0.35f, rTip = R - 0.6f, hw = 0.32f;
      vtx(c * rBase - s * hw, s * rBase + c * hw, 0.5f);
      vtx(c * rBase + s * hw, s * rBase - c * hw, 0.5f);
      vtx(c * rTip, s * rTip, 0.5f);
    }
    sensorRingMesh_[lvl] = Mesh::create(g);
  }
  {   // a soft glowing band along a unit circle (the nuke's warning ring at night), scaled with it
    Geometry g;
    const int S = 96;
    for (int i = 0; i < S; i++) {
      float a0 = i * 2 * PI / S, a1 = (i + 1) * 2 * PI / S, ri = 0.95f, ro = 1.05f;
      float c0 = std::cos(a0), s0 = std::sin(a0), c1 = std::cos(a1), s1 = std::sin(a1);
      auto vtx = [&](float c, float s, float r, float v) { g.v.push_back({{c * r, 0, s * r}, {0, 1, 0}, {0.5f, v}}); };
      vtx(c0, s0, ri, 0); vtx(c0, s0, ro, 1); vtx(c1, s1, ro, 1);
      vtx(c0, s0, ri, 0); vtx(c1, s1, ro, 1); vtx(c1, s1, ri, 0);
    }
    glowRingMesh_ = Mesh::create(g);
  }
  for (Material* m : {&tracerGlowMat_, &tracerPoolMat_, &blipMat_, &sensorRingMat_, &vmFlameGlowMat_, &vmFlameCoreMat_}) {
    m->type = MatType::Basic;
    m->transparent = true;
    m->blend = Blend::Additive;
    m->depthWrite = false;
    m->doubleSide = true;
    m->fog = false;   // they glow in the dark
  }
  tracerGlowMat_.map = tracerPoolMat_.map = sensorRingMat_.map = glowTex_;
  sensorRingMat_.color = hexColor(0xff503c);
  blipMat_.map = blipTex_;
  vmFlameGlowMat_.color = hexColor(0xff6a1a);
  vmFlameGlowMat_.opacity = .55f;
  vmFlameCoreMat_.color = hexColor(0xfff2d0);
  vmFlameCoreMat_.opacity = .95f;
  vmFlameGlowMat_.doubleSide = vmFlameCoreMat_.doubleSide = false;
}

// Asset arrived (loaded one per frame by main): swap it into the scene
void Game::onAssetLoaded(const char* key) {
  if (!key) return;
  if (!strcmp(key, "bunker")) buildBunker();
  if (!strcmp(key, "shrike")) mountShrike();
  if (!strcmp(key, "bunker") || !strcmp(key, "shrike")) buildPortrait();
}

bool Game::assetsSettled() const {
  for (int s = 0; s < ESSENTIAL_STAGES; s++)
    for (const char* k : LOAD_STAGES[s])
      if (k && !models::isSettled(k)) return false;
  return true;
}

void Game::buildBunker() {
  bunkerModel_ = models::get("bunker");
  if (!bunkerModel_) return;
  bunkerParts_ = bunkerModel_->parts;   // own materials: the team colour is re-tinted per upgrade tier
  for (ModelPart& p : bunkerParts_)
    if (p.mat.map) { p.mat.teamColorOn = true; p.mat.teamColor = hexColor(TEAM_TERRAN); }
  fireSpotsValid_ = false;
  updateBunkerVisual();
  mountShrike();
}

// Turret base height: top of the Neosteel frame (Mesh.002), else the roof
float Game::shrikeMountY(const std::vector<ModelPart>& parts, const glm::mat4& m) const {
  const ModelPart* top = nullptr;
  for (const ModelPart& p : parts) if (p.name == "Mesh.002") top = &p;
  if (!top) for (const ModelPart& p : parts) if (p.name == "Mesh.001") top = &p;
  if (!top) return 1.0f - shrike::sink;
  float y = -1e30f;
  for (const Vertex& v : top->cpu.v) y = std::max(y, (m * glm::vec4(v.p, 1)).y);
  return y - shrike::sink;
}

void Game::mountShrike() {
  if (shrikeMounted_ || !bunkerModel_ || !models::isReady("shrike")) return;
  shrikeModel_ = models::get("shrike");
  shrikeParts_ = shrikeModel_->parts;
  for (ModelPart& p : shrikeParts_)
    if (p.mat.map) { p.mat.teamColorOn = true; p.mat.teamColor = hexColor(SHRIKE_GOLD); }
  shrikeY_ = shrikeMountY(bunkerParts_, bunkerModel_->wrapper());
  // Barrel muzzles: centre of each barrel's front face (vertices within 4 cm of
  // the forwardmost point, split left / right), in turret space at yaw 0
  glm::mat4 w = shrikeModel_->wrapper();
  std::vector<glm::vec3> pts;
  for (const ModelPart& p : shrikeParts_) for (const Vertex& v : p.cpu.v) pts.push_back(glm::vec3(w * glm::vec4(v.p, 1)));
  float zMax = -1e30f;
  for (auto& p : pts) zMax = std::max(zMax, p.z);
  glm::vec3 tips[2];
  bool found[2] = {false, false};
  for (int s = 0; s < 2; s++) {
    float side = s == 0 ? -1.f : 1.f;
    glm::vec3 c(0);
    int n = 0;
    for (auto& p : pts)
      if (p.z > zMax - 0.04f && (p.x > 0 ? 1.f : p.x < 0 ? -1.f : 0.f) == side) { c += p; n++; }
    if (n) { tips[s] = c / (float)n; tips[s].z = zMax + 0.02f; found[s] = true; }
  }
  if (found[0] && found[1] && std::abs(tips[0].x - tips[1].x) > 0.2f) { shrikeTips_[0] = tips[0]; shrikeTips_[1] = tips[1]; }
  else logWarn("Shrike: barrel tips not found in the model, using defaults");
  shrikeMounted_ = true;
  updateBunkerVisual();
}

// Bunker stages: Lv0-1 bunker · Lv2 Neosteel frame (Mesh.002) · Lv3 frame +
// Shrike turret. Frame panels: blue at Lv2, gold at Lv3 (team-colour mask only).
void Game::updateBunkerVisual() {
  glm::vec3 tier = hexColor(hpLvl_ >= 3 ? SHRIKE_GOLD : TEAM_TERRAN);
  for (ModelPart& p : bunkerParts_)
    if (p.name == "Mesh.002") p.mat.teamColor = tier;
  fireSpotsValid_ = false;   // the frame changes the surface the fire sits on
  portCache_.clear();        // ...and where the firing slits are
  rebuildBunkerTris();
  updatePortraitBunker();
}

void Game::rebuildBunkerTris() {
  bunkerTris_.clear();
  if (!bunkerModel_) {   // procedural bunker: body box + roof
    for (const BakedPart& bp : bunkerProc_) (void)bp;
    Geometry b = geo::box(3, 1.5f, 3).transform(trs({0, .75f, 0})).nonIndexed();
    Geometry r = geo::box(3.4f, .3f, 3.4f).transform(trs({0, 1.55f, 0})).nonIndexed();
    for (auto* g : {&b, &r}) for (const Vertex& v : g->v) bunkerTris_.push_back(v.p);
    return;
  }
  glm::mat4 w = bunkerModel_->wrapper();
  for (const ModelPart& p : bunkerParts_) {
    if (p.name == "Mesh.002" && hpLvl_ < 2) continue;
    for (size_t t = 0; t < p.cpu.triCount(); t++)
      for (int k = 0; k < 3; k++) bunkerTris_.push_back(glm::vec3(w * glm::vec4(p.cpu.corner(t, k), 1)));
  }
  if (shrikeMounted_ && hpLvl_ >= 3) {
    glm::mat4 sm = trs({0, shrikeY_, 0}, {0, shrikeYaw_, 0}) * shrikeModel_->wrapper();
    for (const ModelPart& p : shrikeParts_)
      for (size_t t = 0; t < p.cpu.triCount(); t++)
        for (int k = 0; k < 3; k++) bunkerTris_.push_back(glm::vec3(sm * glm::vec4(p.cpu.corner(t, k), 1)));
  }
}

// Nearest front-facing hit (three's raycast skips back faces of front-side materials)
bool Game::rayBunker(const glm::vec3& o, const glm::vec3& d, glm::vec3& hit) const {
  float best = 1e30f;
  for (size_t i = 0; i + 2 < bunkerTris_.size(); i += 3) {
    const glm::vec3 &a = bunkerTris_[i], &b = bunkerTris_[i + 1], &c = bunkerTris_[i + 2];
    glm::vec3 e1 = b - a, e2 = c - a, n = glm::cross(e1, e2);
    if (glm::dot(n, d) >= 0) continue;
    glm::vec3 pv = glm::cross(d, e2);
    float det = glm::dot(e1, pv);
    if (std::abs(det) < 1e-12f) continue;
    float inv = 1 / det;
    glm::vec3 tv = o - a;
    float u = glm::dot(tv, pv) * inv;
    if (u < 0 || u > 1) continue;
    glm::vec3 qv = glm::cross(tv, e1);
    float v = glm::dot(d, qv) * inv;
    if (v < 0 || u + v > 1) continue;
    float t = glm::dot(e2, qv) * inv;
    if (t > 0 && t < best) { best = t; hit = o + d * t; }
  }
  return best < 1e30f;
}

// ─── Instance pools ──────────────────────────────────────────────────────────
Pool* Game::poolFor(const std::string& key, const Baked& parts, const glm::mat4& local) {
  auto& p = pools_[key];
  if (!p) { p = std::make_unique<Pool>(); p->parts = parts; p->local = local; }
  return p.get();
}
Pool* Game::framePool(const std::string& skin, const char* anim, int k) {
  std::string key = skin + ":" + anim + std::to_string(k);
  auto it = pools_.find(key);
  if (it != pools_.end()) return it->second.get();
  const Model* m = models::get(skin.c_str());
  const auto& frames = !strcmp(anim, "walk") ? m->walk : m->attack;
  return poolFor(key, {{frames[k], &m->parts[0].mat, true}}, m->wrapper());
}
Pool* Game::modelPool(const std::string& skin) {
  std::string key = skin + ":model";
  auto it = pools_.find(key);
  if (it != pools_.end()) return it->second.get();
  const Model* m = models::get(skin.c_str());
  Baked b;
  for (const ModelPart& p : m->parts) b.push_back({p.mesh, &p.mat, p.mat.type != MatType::Basic});
  return poolFor(key, b, m->wrapper());
}

// How an enemy is drawn: its SC2 skin's walk cycle (one pool per frame) and
// attack animation, else the static model, else the procedural look
void Game::enemyLook(Enemy& e, const char* skin, const std::string& procKey, const Baked& proc) {
  e.walk.clear();
  e.attack.clear();
  const Model* m = skin ? models::get(skin) : nullptr;
  if (m) {
    for (int k = 0; k < (int)m->walk.size(); k++) e.walk.push_back(framePool(skin, "walk", k));
    for (int k = 0; k < (int)m->attack.size(); k++) e.attack.push_back(framePool(skin, "attack", k));
  }
  e.pool = !e.walk.empty() ? e.walk[0] : m ? modelPool(skin) : poolFor(procKey, proc, glm::mat4(1));
  e.model = m != nullptr;
  e.stride = m ? m->def->stride : 1;
  e.step = rnd();
  e.atkT = rnd();
}

EnemyP Game::makeZergling(float x, float z) {
  auto e = std::make_shared<Enemy>();
  e->type = 'z';
  e->pos = {x, 0, z};
  // Metabolic Boost (wave 8+): the winged SC2 model, its webby skin later, 60% faster
  bool boost = wave_ >= cfg::metabolic_wave;
  const char* skin = nullptr;
  for (const char* k : {wave_ >= cfg::webby_wave ? "zergling_webby" : nullptr, boost ? "zergling_upgrade" : nullptr, "zergling"})
    if (k && models::isReady(k)) { skin = k; break; }
  int tier = wave_ >= 7 ? 2 : wave_ >= 4 ? 1 : 0;
  enemyLook(*e, skin, "zergling:" + std::to_string(tier), zerglingProc_[tier]);
  e->hp = e->maxHp = 60.f + wave_ * 28;
  e->speed = (0.014f + wave_ * 0.004f) * (boost ? cfg::metabolic_boost : 1);
  e->dmg = 10.f + wave_ * 3;
  e->leg = rnd() * 6.28f;
  e->light = true;   // SC2: Zergling is Light
  e->rScore = 10 + wave_ * 2;
  e->rMin = 6 + wave_ * 2;
  e->rGas = wave_ / 2;
  return e;
}

EnemyP Game::makeRoach(float x, float z) {
  auto e = std::make_shared<Enemy>();
  e->type = 'r';
  e->pos = {x, 0, z};
  const char* skin = nullptr;
  for (const char* k : {wave_ >= 13 ? "roach_ex3" : nullptr, wave_ >= 9 ? "roach_ex1a" : nullptr, "roach"})
    if (k && models::isReady(k)) { skin = k; break; }
  enemyLook(*e, skin, "roach", roachProc_);
  e->atkT = 0;
  e->mouth = roachMouth(skin);
  e->hp = e->maxHp = 200.f + wave_ * 70;
  e->speed = 0.008f + wave_ * 0.002f;
  e->dmg = 18.f + wave_ * 5;
  e->leg = rnd() * 6.28f;
  e->armored = true;   // SC2: Roach is Armored
  e->rScore = 30 + wave_ * 5;
  e->rMin = 18 + wave_ * 4;
  e->rGas = 3 + wave_;
  return e;
}

// Advance a walk cycle by `dist` world units (one loop per stride, ANIM_SPEED faster)
void Game::walkStep(Enemy& e, float dist) {
  e.step = std::fmod(e.step + dist / e.stride * ANIM_SPEED, 1.f);
  e.pool = e.walk[std::min((int)e.walk.size() - 1, (int)std::floor(e.step * e.walk.size()))];
}

// Distance from the bunker's centre at which an enemy stops: Zerglings bite the hull, Roaches spit from range
float Game::attackReach(const Enemy& e) const { return e.type == 'r' ? roach_acid::range : 2.0f; }

// ─── Roach acid ──────────────────────────────────────────────────────────────
glm::vec3 Game::roachMouth(const char* skin) {
  const Model* m = skin ? models::get(skin) : nullptr;
  if (!m) return {0, .6f, 1.0f};   // the procedural Roach's face
  return m->local({roach_acid::mouth[0], roach_acid::mouth[1], roach_acid::mouth[2]});
}

void Game::roachAttack(Enemy& e, float dt, float ex, float ez) {
  e.rotY = std::atan2(-ex, -ez);
  e.rotZ = 0;
  e.pos.y = 0;
  float t0 = e.atkT, spit = roach_acid::spit / ANIM_SPEED;
  e.atkT += dt;
  if (t0 < spit && e.atkT >= spit) spitAcid(e);
  if (e.atkT >= roach_acid::cooldown) e.atkT -= roach_acid::cooldown;
  // the 1 s animation (ANIM_SPEED× faster), then hold its last frame until the next spit
  if (!e.attack.empty())
    e.pool = e.attack[std::min((int)e.attack.size() - 1, (int)std::floor(e.atkT * ANIM_SPEED / roach_acid::anim * e.attack.size()))];
  else if (!e.model) e.pos.y = std::abs(std::sin(e.leg)) * .07f;
}

void Game::spitAcid(Enemy& e) {
  glm::vec3 p = glm::vec3(trs(e.pos, {0, e.rotY, e.rotZ}) * glm::vec4(e.mouth, 1));
  float ex = e.pos.x, ez = e.pos.z, d = std::hypot(ex, ez);
  if (d == 0) d = 1;
  float tx = ex / d * 1.35f, tz = ez / d * 1.35f, ty = .5f + rnd() * .4f;   // the hull facing the Roach
  const float T = roach_acid::flight, g = roach_acid::gravity;
  acids_.push_back({p, {(tx - p.x) / T, (ty - p.y) / T + .5f * g * T, (tz - p.z) / T}, 0, e.dmg * roach_acid::cooldown});
  for (int i = 0; i < 4; i++) emit(glow_, p.x, p.y, p.z, (rnd() - .5f) * 2, rnd() * 1.5f, (rnd() - .5f) * 2, .25f, .22f, .05f, .9f, 0xc8ff50, 0x3a9a12, 4, 1, 0);
  audio::play("spit");
}

void Game::updateAcids(float dt) {
  const float g = roach_acid::gravity, arm = 1 / (1 + armorLvl_ * .35f);
  for (size_t i = 0; i < acids_.size();) {
    Acid& a = acids_[i];
    a.t += dt;
    a.v.y -= g * dt;
    a.p += a.v * dt;
    if (rnd() < .75f)   // dripping trail
      emit(glow_, a.p.x, a.p.y, a.p.z, (rnd() - .5f) * .6f, (rnd() - .5f) * .6f, (rnd() - .5f) * .6f, .3f, .26f, .05f, .85f, 0xb8ff40, 0x2c8a10, 2, 0, 0);
    if (a.t < roach_acid::flight) { i++; continue; }
    bunkerHp_ = std::max(0.f, bunkerHp_ - a.dmg * arm);
    for (int k = 0; k < 9; k++)   // splash + a puff of corrosive smoke
      emit(glow_, a.p.x, a.p.y, a.p.z, (rnd() - .5f) * 5, rnd() * 4 + 1, (rnd() - .5f) * 5, .45f + rnd() * .25f, .24f, .06f, 1, 0xc8ff50, 0x3a9a12, 18, .8f, 0);
    emit(smoke_, a.p.x, a.p.y, a.p.z, 0, .8f, 0, 1.0f, .5f, 1.3f, .45f, 0x6a8a3a, 0x3a4a2a, -.2f, .5f, 1);
    acidFx(a.p);
    audio::play("acid");
    if (dmgFlashCD_ <= 0) { flash(1); dmgFlashCD_ = .6f; }
    acids_.erase(acids_.begin() + i);
  }
}

// Dying enemies: pop, roll onto their side, then sink into the ground (~1.2 s)
void Game::updateCorpses(float dt) {
  for (size_t i = 0; i < corpses_.size();) {
    Corpse& c = corpses_[i];
    c.t += dt;
    if (c.t < 0.12f) c.pos.y = c.t / 0.12f * 0.18f;
    else c.pos.y = 0.18f - std::pow(std::min(1.f, (c.t - 0.12f) / 1.0f), 2.f) * 0.9f;
    c.rotZ = c.spin * std::min(1.f, c.t / 0.3f) * PI * 0.5f;
    if (c.t < 1.2f) i++;
    else corpses_.erase(corpses_.begin() + i);
  }
}

// Sensor Tower: a Zerg out in the dark within its range shows as a blip, and the Shrike may engage it
bool Game::sensed(const Enemy& e) const {
  return sensorLvl_ >= 1 && !e.dead && !e.visible && std::hypot(e.pos.x, e.pos.z) <= cfg::sensor_range[sensorLvl_];
}

// ─── Shrike turret (HP Lv3) ──────────────────────────────────────────────────
// Picks the best enemy in range (distance + how far the gun must turn, enemies
// attacking the bunker first; the current target is kept unless another is
// clearly better), leads it and fires twin rounds: two extra marines, no ammo.
float Game::shrikeScore(const Enemy& e) const {
  float d = std::hypot(e.pos.x, e.pos.z);
  if (d > shrike::range) return INFINITY;
  bool attacking = d < attackReach(e);
  return d + std::abs(angDiff(std::atan2(e.pos.x, e.pos.z), shrikeYaw_)) * 1.2f - (attacking ? 3 : 0);
}
EnemyP Game::shrikePick() {
  EnemyP best;
  float bestS = INFINITY;
  bool curLive = false;
  for (auto* list : {&zerglings_, &roaches_})
    for (auto& e : *list) {
      if (e->dead || !(e->visible || sensed(*e))) continue;   // at night: in the light, or a Sensor Tower blip
      if (e == shrikeTgt_) curLive = true;
      float sc = shrikeScore(*e);
      if (sc < bestS) { bestS = sc; best = e; }
    }
  if (shrikeTgt_ && curLive) {
    float sc = shrikeScore(*shrikeTgt_);
    if (sc < INFINITY && sc <= bestS + 2.0f) return shrikeTgt_;   // hysteresis: stay on target
  }
  return best;
}
// Marine overdrive: a bunker full of Marines (6/6) drives the turret much faster
bool Game::shrikeOverdrive() const {
  return hpLvl_ >= 3 && roster_.size() == 6 && std::all_of(roster_.begin(), roster_.end(), [](Unit u) { return u == Unit::Marine; });
}

void Game::updateShrike(float dt) {
  bool od = shrikeOverdrive();
  if (od != shrikeOD_) {
    shrikeOD_ = od;
    if (od) setStatus("Six Marines \xE2\x80\x94 Shrike overdrive: rate of fire \xC3\x97" "2.5!");
  }
  if (hpLvl_ < 3) { shrikeTgt_.reset(); return; }
  shrikeCD_ = std::max(0.f, shrikeCD_ - dt);
  EnemyP tgt = shrikeTgt_ = shrikePick();
  float want = 0, tx = 0, tz = 0, d = 0;
  if (tgt) {
    glm::vec3 p = tgt->pos;
    d = std::max(0.01f, std::hypot(p.x, p.z));
    tx = p.x;
    tz = p.z;
    if (d > attackReach(*tgt)) {   // lead moving enemies only (they walk straight at the bunker)
      float v = tgt->speed * (tgt->slowed ? tgt->slowFactor : 1) * 60;
      float lead = v * d / (unitDef(Unit::Marine).bulletSpeed * 60);
      tx -= p.x / d * lead;
      tz -= p.z / d * lead;
    }
    want = std::atan2(tx, tz);
    // critically damped spring toward the aim (substeps keep it stable at low frame rates)
    float w = shrike::omega;
    int steps = std::max(1, (int)std::ceil(dt / 0.008f));
    float h = dt / steps;
    for (int i = 0; i < steps; i++) {
      shrikeVel_ += (w * w * angDiff(want, shrikeYaw_) - 2 * w * shrikeVel_) * h;
      shrikeVel_ = glm::clamp(shrikeVel_, -shrike::maxTurn, shrike::maxTurn);
      shrikeYaw_ = angDiff(shrikeYaw_ + shrikeVel_ * h, 0);
    }
  } else {   // idle: ease into a slow scanning sweep in the direction it was already turning
    float tv = 0.5f * (shrikeVel_ < 0 ? -1.f : 1.f);
    shrikeVel_ = tv + (shrikeVel_ - tv) * std::exp(-dt * 20);
    shrikeYaw_ = angDiff(shrikeYaw_ + shrikeVel_ * dt, 0);
  }
  shrikeRecoil_ = std::max(0.f, shrikeRecoil_ - dt * 9);
  if (!tgt || shrikeCD_ > 0 || wavePhase_ != WavePhase::Combat) return;
  // fire once the target is inside the gun's line of fire (wider for close targets)
  float hitR = tgt->type == 'r' ? .75f : .5f;
  if (std::abs(angDiff(want, shrikeYaw_)) > std::max(0.1f, std::atan2(hitR, d))) return;
  shrikeCD_ = od ? shrike::overdriveInterval : shrike::interval;
  shrikeRecoil_ = 1;
  // twin barrels = 2 marine rounds, each from its own muzzle at one aim point: a V that crosses into an X
  float sy = std::sin(shrikeYaw_), cy = std::cos(shrikeYaw_);
  float aimY = tgt->type == 'r' ? 0.55f : AIM_Y;
  for (const glm::vec3& tip : shrikeTips_) {
    float ox = cy * tip.x + sy * tip.z, oz = -sy * tip.x + cy * tip.z, oy = shrikeY_ + tip.y;
    Bullet b = makeBullet(tx, tz, 0, Unit::Marine, ox, oy, oz, aimY);
    b.len = 1.8f;
    if (od) b.color = shrike::overdriveColor;
    bullets_.push_back(b);
    muzzleFlash(ox, oy, oz, b.vx / b.speed, b.vz / b.speed, od ? 0xff4020 : 0xffb030, false);
  }
  audio::play("shrike");
}

// ─── Stimpack (T): a full Marine squad fires 50% faster for 11 s ─────────────
bool Game::stimUnlocked() const {
  return roster_.size() == 6 && std::all_of(roster_.begin(), roster_.end(), [](Unit u) { return u == Unit::Marine; });
}
float Game::stimCost() const { return superStimUnlocked_ ? 0 : cfg::stim_hp * roster_.size(); }
void Game::doStim() {
  if (phase_ != Phase::Playing || isPaused()) return;
  if (wavePhase_ != WavePhase::Combat) { setStatus("Stimpack only during a wave"); audio::play("deny"); return; }
  if (!stimUnlocked()) { setStatus("Stimpack needs a full Marine squad (6/6)"); audio::play("deny"); return; }
  if (stimT_ > 0) { setStatus("Already stimmed!"); return; }
  float cost = stimCost();
  if (cost && bunkerHp_ <= cost) { setStatus("Not enough HP to stim"); audio::play("deny"); return; }
  bunkerHp_ -= cost;
  stimT_ = cfg::stim_duration;
  stimVolley_ = 0;
  char b[200];
  if (superStimUnlocked_)
    snprintf(b, sizeof b, "Super Stimpack! Marines fire 50%% faster, bunker repair +%d HP/s for %d s", (int)std::round(repairRate()), (int)cfg::stim_duration);
  else
    snprintf(b, sizeof b, "Stimpack! Marines fire 50%% faster for %d s (\xE2\x88\x92%d HP)", (int)cfg::stim_duration, (int)cost);
  setStatus(b);
  audio::play("stim");
}

// ─── Particles ───────────────────────────────────────────────────────────────
// Burst of glowing sparks / debris: fast outward + upward spray, gravity, ~0.55 s
// (on the GPU: the same spray, six times denser)
void Game::spawnParts(const glm::vec3& p, uint32_t col, int n) {
  if (gpuFx()) {
    GpuParticles::Burst b;
    b.pos = p;
    b.count = n * 6;
    b.shape = GpuParticles::Box;
    b.vel = {0, 3.6f, 0};
    b.box = {6.5f, 12, 6.5f};
    b.c0 = b.c1 = col;
    b.life0 = .4f; b.life1 = .7f;
    b.s0 = .2f; b.s1 = .08f;
    b.grav = 54;
    b.drag = .5f;
    gpuFx_.burst(b);
    return;
  }
  for (int i = 0; i < n; i++)
    emit(glow_, p.x, p.y, p.z, (rnd() - .5f) * 13, rnd() * 12 + 3.6f, (rnd() - .5f) * 13, .4f + rnd() * .3f, .2f, .08f, 1, col, col, 54, 0.5f, 0);
}

void Game::setGpuFx(bool on) {
  gpuFxOn_ = on;
  storage::set("gpuParticles", on ? "1" : "0");
  R.setGpuParticles(gpuFx() ? &gpuFx_ : nullptr);
  if (!on) gpuFx_.clear();
}

// Explosion: fireball, sparks, a ring of dust along the ground and a smoke column.
// The count lives in the small particles (sparks, embers): additive fire is
// limited by overdraw — a first nuke with 18 000 large flames painted ~90 layers
// over the screen (white, and ~15 ms of GPU fill per frame on a 3050 Ti).
// The nuke keeps only the sparks and embers: its mushroom cloud draws its fire and its
// dust ring (these bursts, at its scale, hazed the whole field and hid the cloud).
void Game::explosionFx(const glm::vec3& p, uint32_t col, float k, bool nuke) {
  if (!gpuFx()) return;
  using B = GpuParticles::Burst;
  const float k2 = k * k, rk = std::sqrt(k);
  if (!nuke) {
    B fire;   // fireball: fewer, fainter flames cooling from orange to deep red, rising
    fire.pos = p; fire.count = (int)(300 * k2); fire.radius = .5f * k; fire.speed = 7 * k; fire.up = 1.5f * k;
    fire.c0 = 0xffa040; fire.c1 = 0x601000; fire.a0 = .35f; fire.life0 = .3f; fire.life1 = .8f + .3f * (k - 1);
    fire.s0 = .35f * k; fire.s1 = .12f * k; fire.grav = -1; fire.drag = 2.5f;
    gpuFx_.burst(fire);
  }
  B spark;  // sparks: the bulk of the count, small, flung far
  spark.pos = p; spark.count = (int)(1200 * k2); spark.speed = 14 * k; spark.up = 2 * k;
  spark.c0 = 0xffe9a0; spark.c1 = col; spark.a0 = .9f; spark.life0 = .5f; spark.life1 = 1.2f + .2f * k;
  spark.s0 = .08f; spark.s1 = .03f; spark.grav = 10; spark.drag = 1;
  gpuFx_.burst(spark);
  B emb;    // embers raining down, landing on the ground
  emb.pos = p; emb.count = (int)(300 * k2); emb.speed = 6 * k; emb.up = 4 * k;
  emb.c0 = 0xffc060; emb.c1 = 0xff3000; emb.a0 = .8f; emb.life0 = 1; emb.life1 = 2 + k;
  emb.s0 = .1f; emb.s1 = .04f; emb.grav = 6; emb.drag = .4f;
  gpuFx_.burst(emb);
  if (nuke) return;
  // smoke is overdraw-bound (normal blending, large sprites): its count grows slower than the fire's
  B ring;   // shockwave dust skimming the ground
  ring.pos = {p.x, .15f, p.z}; ring.count = (int)(400 * k * rk); ring.shape = GpuParticles::Ring; ring.speed = 12 * k;
  ring.c0 = 0x9a8a60; ring.c1 = 0x4a4030; ring.a0 = .3f; ring.life0 = .8f; ring.life1 = 1.6f * rk;
  ring.s0 = .5f * rk; ring.s1 = 1.6f * rk; ring.grav = -.3f; ring.drag = 3; ring.smoke = true; ring.fade = true;
  gpuFx_.burst(ring);
  B plume;  // smoke column
  plume.pos = p; plume.count = (int)(120 * k * rk); plume.shape = GpuParticles::Column; plume.speed = 3 * k; plume.radius = .3f * k;
  plume.c0 = 0x2a2622; plume.c1 = 0x6a645c; plume.a0 = .4f; plume.life0 = 1.5f; plume.life1 = 3 * rk;
  plume.s0 = .6f * rk; plume.s1 = 2.4f * rk; plume.grav = -1; plume.drag = .8f; plume.smoke = true; plume.fade = true;
  gpuFx_.burst(plume);
}

// A Zerg dying: a splash of ichor that lands on the ground, and a little smoke
void Game::deathFx(const Enemy& e) {
  if (!gpuFx()) return;
  bool r = e.type == 'r';
  GpuParticles::Burst b;
  b.pos = e.pos + glm::vec3(0, .4f, 0); b.count = r ? 450 : 220; b.shape = GpuParticles::Box;
  b.vel = {0, 2, 0}; b.box = {r ? 5.f : 4.f, r ? 7.f : 6.f, r ? 5.f : 4.f}; b.radius = r ? .4f : .25f;
  b.c0 = r ? 0xff8a30 : 0x6aff4a; b.c1 = r ? 0x3a1200 : 0x0a3a10; b.life0 = .6f; b.life1 = 1.4f;
  b.s0 = .14f; b.s1 = .06f; b.grav = 18; b.drag = .8f;
  gpuFx_.burst(b);
  GpuParticles::Burst sm;
  sm.pos = e.pos + glm::vec3(0, .3f, 0); sm.count = r ? 40 : 24; sm.speed = 1.2f; sm.up = .6f; sm.radius = .3f;
  sm.c0 = r ? 0x4a3020 : 0x2a3a20; sm.c1 = 0x5a5a50; sm.a0 = .45f; sm.life0 = .8f; sm.life1 = 1.6f;
  sm.s0 = .4f; sm.s1 = 1.2f; sm.grav = -.6f; sm.drag = 1.5f; sm.smoke = true; sm.fade = true;
  gpuFx_.burst(sm);
}

// Roach acid splashing on the hull
void Game::acidFx(const glm::vec3& p) {
  if (!gpuFx()) return;
  GpuParticles::Burst b;
  b.pos = p; b.count = 160; b.shape = GpuParticles::Box; b.vel = {0, 1, 0}; b.box = {3.5f, 4, 3.5f};
  b.c0 = 0xd8ff60; b.c1 = 0x2a8a10; b.life0 = .4f; b.life1 = .9f; b.s0 = .12f; b.s1 = .05f; b.grav = 16; b.drag = .8f;
  gpuFx_.burst(b);
}

// Muzzle flash at (x, y, z) firing along (dx, dz): core, flame tongue, sparks, smoke, a light flash on the hull
void Game::muzzleFlash(float x, float y, float z, float dx, float dz, uint32_t col, bool big) {
  float k = big ? 1.35f : 1;
  emit(glow_, x, y, z, dx * 1.5f, .2f, dz * 1.5f, .05f, .55f * k, .25f * k, 1, 0xffffff, 0xffe08a, 0, 0, 0);
  for (int i = 1; i <= 3; i++)
    emit(glow_, x + dx * .12f * i * k, y, z + dz * .12f * i * k, dx * 4, .1f, dz * 4, .045f + i * .005f, (.42f - i * .08f) * k, .05f, .9f, 0xfff0c0, col, 0, 6, 0);
  for (int i = 0; i < 5; i++) {
    float a = (rnd() - .5f) * .9f, c = std::cos(a), sn = std::sin(a), sp = 5 + rnd() * 6;
    emit(glow_, x, y, z, (dx * c - dz * sn) * sp, rnd() * 2.5f, (dz * c + dx * sn) * sp, .15f + rnd() * .15f, .09f, .03f, 1, 0xffe9a0, 0xff7a20, 9, 1.5f, 0);
  }
  emit(smoke_, x + dx * .2f, y + .05f, z + dz * .2f, dx * .6f, .5f, dz * .6f, .5f + rnd() * .3f, .18f * k, .55f * k, .22f, 0x6a6a6a, 0x9a9a9a, -.3f, 1.2f, 1);
  muzzlePos_ = {x + dx * .3f, y + .1f, z + dz * .3f};
  muzzleI_ = std::max(muzzleI_, big ? 3.2f : 2.2f);
}

// ─── Crates, beams, units ────────────────────────────────────────────────────
void Game::makeCrate(float x, float z) {
  auto p = std::make_shared<Prop>();
  p->mesh = crateMesh_;
  p->mat = crateMat_;
  p->m = trs({x, .32f, z}, {0, rnd() * PI, 0});
  crates_.push_back(p);
  crateXZ_.push_back({x, z});
}

void Game::makeBeam(float x, float z, uint32_t col) {
  auto p = std::make_shared<Prop>();
  p->mesh = beamMesh_;
  p->mat.type = MatType::Basic;
  p->mat.color = hexColor(col);
  p->mat.transparent = true;
  p->mat.opacity = .8f;
  p->m = trs({x, 12, z});
  beams_.push_back({p, .5f, .5f, 0, {x, 12, z}});
}

int Game::slotsUsed() const {
  int s = 0;
  for (Unit u : roster_) s += unitDef(u).slots;
  return s;
}

// The figure that leaves the bunker wears its unit's SC2 model, else the Marine's, else a procedural marine
void Game::setUnitModel(UnitFig& m, Unit type, int idx) {
  const char* key = unitDef(type).key;
  if (m.typeSet && m.type == type && !(m.fallback && models::isReady(key))) return;
  const Model* md = models::get(key);
  if (!md) md = models::get("marine");
  m.modelP = md;
  m.type = type;
  m.typeSet = true;
  m.idx = idx;
  m.fallback = !models::isReady(key);
  m.stride = md && !md->walk.empty() ? md->def->stride : 1;
  m.step = 0;
  m.frame = 0;
}

void Game::syncMarines() {
  size_t count = roster_.size();
  while (marines_.size() < count) {
    int idx = (int)marines_.size();
    UnitFig m;
    m.homeX = idx < 6 ? MARINE_SLOTS[idx][0] : .3f + idx * .4f;
    m.homeZ = idx < 6 ? MARINE_SLOTS[idx][1] : 0;
    m.pos = {m.homeX, 0, m.homeZ};
    m.leg = rnd() * 6.28f;
    setUnitModel(m, roster_[idx], idx);
    marines_.push_back(std::move(m));
  }
  while (marines_.size() > count) {
    // Remove an idle unit first; one out on a crate run carries the resupply
    // callback, so if it must go, finish its job
    int i = -1;
    for (int k = (int)marines_.size() - 1; k >= 0; k--) if (!marines_[k].running) { i = k; break; }
    if (i < 0) i = (int)marines_.size() - 1;
    UnitFig m = std::move(marines_[i]);
    marines_.erase(marines_.begin() + i);
    if (m.running) {
      collectCrateAt(m.tx, m.tz);
      for (auto& q : m.queue) collectCrateAt(q.x, q.y);
      m.queue.clear();
      auto cb = std::move(m.cb);
      m.cb = nullptr;
      if (cb) cb();
    }
  }
  for (size_t i = 0; i < marines_.size(); i++)   // marines_[i] stands for roster_[i]
    if (!marines_[i].running) setUnitModel(marines_[i], roster_[i], (int)i);
}

void Game::collectCrateAt(float x, float z) {
  for (size_t i = 0; i < crates_.size();) {
    if (std::abs(crateXZ_[i].x - x) < 1.2f && std::abs(crateXZ_[i].y - z) < 1.2f) {
      crates_.erase(crates_.begin() + i);
      crateXZ_.erase(crateXZ_.begin() + i);
    } else i++;
  }
}

// ─── Resupply: one crate per wave ────────────────────────────────────────────
// The Viking's crate (airstrike) and the end-of-wave crate both refill the
// magazine. The end-of-wave resupply rides on a Viking crate still on its way
// (about to drop, on the ground or being carried in) and skips its own crate
// when one already came in this wave: a Marine picks up one crate, not two.
void Game::awaitCrate(std::function<void()> cb) { crateCbs_.push_back(std::move(cb)); }
void Game::deliverCrate(float x, float z) {
  makeCrate(x, z);
  sendMarine(x, z, [this]() { crateIn(); });
}
void Game::crateIn() {
  auto cbs = std::move(crateCbs_);
  crateCbs_.clear();
  waveCrates_++;
  logInfo("wave %d: crate carried in (%d this wave)", wave_, waveCrates_);
  ammo_ = maxAmmo_;
  audio::play("crate");
  for (auto& f : cbs) f();
}

void Game::sendMarine(float tx, float tz, std::function<void()> cb) {
  // Only one unit leaves the bunker: a crate dropped while one is out joins its
  // trip (a safety net — the resupply above keeps to one crate at a time)
  for (UnitFig& out : marines_) {
    if (!out.running) continue;
    out.queue.push_back({tx, tz});
    auto prev = std::move(out.cb);
    out.cb = [prev, cb]() { if (prev) prev(); if (cb) cb(); };
    if (out.returning) {
      out.tx = out.queue.front().x;
      out.tz = out.queue.front().y;
      out.queue.erase(out.queue.begin());
      out.returning = false;
    }
    return;
  }
  // A Marine fetches the crate when one is free, else any idle unit
  int i = -1;
  for (size_t k = 0; k < roster_.size(); k++)
    if (roster_[k] == Unit::Marine && k < marines_.size() && !marines_[k].running) { i = (int)k; break; }
  if (i < 0)
    for (size_t k = 0; k < marines_.size(); k++) if (!marines_[k].running) { i = (int)k; break; }
  if (i < 0) { collectCrateAt(tx, tz); if (cb) cb(); return; }   // nobody free: resupply at once
  UnitFig& m = marines_[i];
  setUnitModel(m, roster_[i], i);
  m.running = true;
  m.visible = true;
  m.tx = tx;
  m.tz = tz;
  m.returning = false;
  m.queue.clear();
  m.cb = std::move(cb);
}

void Game::updateMarines(float dt) {
  for (size_t i = 0; i < marines_.size(); i++) {
    UnitFig& m = marines_[i];
    if (!m.running) continue;
    m.leg += dt * 9;
    if (m.modelP && !m.modelP->walk.empty()) {   // SC2 walk cycle at the running pace
      m.step = std::fmod(m.step + 2.8f * dt / m.stride * ANIM_SPEED, 1.f);
      m.frame = std::min((int)m.modelP->walk.size() - 1, (int)std::floor(m.step * m.modelP->walk.size()));
    } else m.pos.y = std::abs(std::sin(m.leg)) * .1f;   // procedural marine hops
    if (!m.returning) {
      float dx = m.tx - m.pos.x, dz = m.tz - m.pos.z, d = std::sqrt(dx * dx + dz * dz);
      if (d < .5f) {
        collectCrateAt(m.tx, m.tz);
        if (!m.queue.empty()) { m.tx = m.queue.front().x; m.tz = m.queue.front().y; m.queue.erase(m.queue.begin()); }
        else m.returning = true;
      } else {
        m.pos.x += dx / d * 2.8f * dt;
        m.pos.z += dz / d * 2.8f * dt;
        m.rotY = std::atan2(dx, dz);
      }
    } else {
      float dx = m.homeX - m.pos.x, dz = m.homeZ - m.pos.z, d = std::sqrt(dx * dx + dz * dz);
      if (d < .5f) {
        m.pos = {m.homeX, 0, m.homeZ};
        m.running = false;
        m.visible = false;
        if (m.cb) {
          auto cb = std::move(m.cb);
          m.cb = nullptr;
          cb();   // may change the roster: don't touch m after this
        }
      } else {
        m.pos.x += dx / d * 2.8f * dt;
        m.pos.z += dz / d * 2.8f * dt;
        m.rotY = std::atan2(dx, dz);
      }
    }
  }
}

// ─── Strikes ─────────────────────────────────────────────────────────────────
std::vector<EnemyP> Game::allEnemies() const {
  std::vector<EnemyP> v = zerglings_;
  v.insert(v.end(), roaches_.begin(), roaches_.end());
  return v;
}

void Game::strikeAt(float tx, float tz, uint32_t col, float r) {
  makeBeam(tx, tz, col);
  audio::play("boom");
  spawnParts({tx, .5f, tz}, col, 22);
  explosionFx({tx, .5f, tz}, col, 1);
  for (auto& e : allEnemies()) {
    if (e->dead) continue;
    float dx = e->pos.x - tx, dz = e->pos.z - tz;
    if (std::sqrt(dx * dx + dz * dz) < r) killEnemy(e, true);
  }
}

void Game::randPos(float& x, float& z) {
  float r = 8 + rnd() * 4, a = rnd() * PI * 2;
  x = std::cos(a) * r;
  z = std::sin(a) * r;
}

void Game::doAirstrike() {
  if (isPaused()) return;
  if (airCD_ > 0 || wavePhase_ != WavePhase::Combat || ammo_ > 0) { setStatus(ammo_ > 0 ? "Use when ammo is empty!" : "Not ready yet!"); return; }
  if (vikingActive_) { setStatus("Viking already inbound!"); return; }
  airCD_ = strikeCooldown();
  waveAirs_++;
  std::vector<EnemyP> alive;
  for (auto& e : allEnemies()) if (!e->dead) alive.push_back(e);
  // one missile per enemy cluster (enemies within 3 of each other)
  std::vector<std::pair<glm::vec2, EnemyP>> targets;
  std::vector<Enemy*> assigned;
  auto isAssigned = [&](Enemy* e) { return std::find(assigned.begin(), assigned.end(), e) != assigned.end(); };
  for (auto& e : alive) {
    if (isAssigned(e.get())) continue;
    float ex = e->pos.x, ez = e->pos.z;
    for (auto& o : alive) {
      float dx = o->pos.x - ex, dz = o->pos.z - ez;
      if (std::sqrt(dx * dx + dz * dz) < 3 && !isAssigned(o.get())) assigned.push_back(o.get());
    }
    targets.push_back({{ex, ez}, e});
  }
  std::sort(targets.begin(), targets.end(), [](auto& a, auto& b) { return a.first.x < b.first.x; });   // flight order
  float ca = rnd() * PI * 2;
  glm::vec2 cratePos(std::cos(ca) * 3.5f, std::sin(ca) * 3.5f);   // safe pick-up spot by the bunker
  if (!alive.empty()) {
    setStatus("Viking incoming \xE2\x80\x94 " + std::to_string(alive.size()) + " target" + (alive.size() > 1 ? "s" : "") + " acquired!");
    launchViking(targets, cratePos);
  } else {
    setStatus("Viking inbound \xE2\x80\x94 resupply run!");
    launchViking({}, cratePos);
  }
}

// Air-to-ground missile: leaves the muzzle of one of the Viking's two launcher tubes (in
// turn), runs on ahead, then dives on its target, homing on the lead enemy while it
// lives; its blast kills within 4.5
void Game::fireVikingMissile(glm::vec2 tgt, EnemyP e) {
  vMissileSide_ = -vMissileSide_;
  VMissile m;
  // The SC2 model's launchers: the two long tubes either side of the nose, muzzles at
  // (±0.2, 0.09, 0.91) in its normalised space (+z = nose). The procedural stand-in
  // uses the same points.
  m.p0 = m.p = glm::vec3(vikingMatrix() * glm::vec4(0.2f * vMissileSide_, 0.09f, 0.91f, 1));
  m.p1 = m.p0 + glm::vec3(VM_RUN, -0.5f, 0);
  m.tgt = tgt;
  m.e = std::move(e);
  vMissiles_.push_back(m);
  for (int i = 0; i < 6; i++)   // ignition
    emit(glow_, m.p0.x, m.p0.y, m.p0.z, -2 - rnd() * 3, (rnd() - .5f) * 1.5f, (rnd() - .5f) * 1.5f, .18f, .35f, .1f, .95f, 0xffd080, 0xff5010, 0, 2, 0);
  audio::play("rocket");
}

void Game::updateVikingMissiles(float dt) {
  for (size_t i = 0; i < vMissiles_.size();) {
    VMissile& m = vMissiles_[i];
    if (m.e && !m.e->dead) m.tgt = {m.e->pos.x, m.e->pos.z};
    glm::vec3 prevTail = m.p - m.dir * VM_TAIL;
    m.t = std::min(1.f, m.t + dt / VM_FLIGHT);
    float u = m.t * (0.6f + 0.4f * m.t);   // speeds up through the dive
    glm::vec3 p2(m.tgt.x, 0.15f, m.tgt.y);
    m.p = (1 - u) * (1 - u) * m.p0 + 2 * (1 - u) * u * m.p1 + u * u * p2;
    glm::vec3 d = (1 - u) * (m.p1 - m.p0) + u * (p2 - m.p1);
    if (glm::length(d) > 1e-4f) m.dir = glm::normalize(d);
    // smoke trail: a puff every ~0.12 along this frame's path (one per frame left a dotted line
    // at this speed), and a few embers; the flame itself is drawn (render)
    glm::vec3 tail = m.p - m.dir * VM_TAIL;
    m.trail.push_back({tail, gameT_});   // the light streak: the last VM_STREAK s of the path
    while (m.trail.size() > 2 && gameT_ - m.trail.front().w > VM_STREAK) m.trail.erase(m.trail.begin());
    int n = std::clamp((int)std::ceil(glm::length(tail - prevTail) / 0.12f), 1, 16);
    for (int k = 1; k <= n; k++) {
      glm::vec3 q = prevTail + (tail - prevTail) * ((float)k / n);
      emit(smoke_, q.x, q.y, q.z, (rnd() - .5f) * .2f, .2f, (rnd() - .5f) * .2f, .8f, .07f, .4f, .4f, 0xb0aaa0, 0x605c56, -.1f, .8f, 1);
    }
    if (rnd() < .5f)
      emit(glow_, tail.x, tail.y, tail.z, -m.dir.x * 2 + (rnd() - .5f), -m.dir.y * 2 + (rnd() - .5f), -m.dir.z * 2 + (rnd() - .5f), .2f, .05f, .015f, .9f, 0xffd080, 0xc02000, 2, 1, 0);
    if (m.t < 1) { i++; continue; }
    glm::vec2 hit = m.tgt;
    vMissiles_.erase(vMissiles_.begin() + i);
    strikeAt(hit.x, hit.y, 0xff8800, 4.5f);   // may kill enemies: m is gone already
  }
}

void Game::launchViking(std::vector<std::pair<glm::vec2, EnemyP>> targets, glm::vec2 cratePos) {
  if (vikingActive_) return;
  vikingActive_ = true;
  vikingPos_ = {-22, 5, 0};
  // x = -22 → +22 at 14 units/s; each missile leaves VM_FLIGHT s before the Viking
  // would pass over its target, so it lands about then, homing on the lead enemy
  const float SPEED = 14, TOTAL_X = 44, START_X = -22;
  for (auto& t : targets) {
    glm::vec2 p = t.first;
    EnemyP e = t.second;
    after(std::max(0.2f, (p.x - START_X) / SPEED - VM_FLIGHT), [this, p, e]() { fireVikingMissile(p, e); });
  }
  // Crate by the bunker as the Viking exits. It is promised from launch on: a wave
  // cleared by these missiles waits for it (resupply)
  awaitCrate([this]() { setStatus("Ammo restocked! Good hunting, Commander."); });
  // At night it drops a parachute flare that lights the battlefield (as in Beach Head)
  // (checked again when it fires: Y may have switched the cycle off meanwhile)
  if (dnEnabled_ && dnFactor_ > 0.5f) after(0.6f, [this]() { if (dnEnabled_ && dnFactor_ > 0.5f) startFlare(); });
  after((22 - START_X) / SPEED * 0.85f, [this, cratePos]() {
    setStatus("Crate deployed! Marine incoming...");
    deliverCrate(cratePos.x, cratePos.y);
  });
  after(TOTAL_X / SPEED + 0.4f, [this]() { removeViking(); });
}
void Game::removeViking() { vikingActive_ = false; }
// Its clone's rotation replaces the template's: y = π/2 (nose +z → +x), banked
glm::mat4 Game::vikingMatrix() const { return trs(vikingPos_, {0, PI / 2, -0.05f}); }
void Game::updateViking(float dt) {
  if (!vikingActive_) return;
  vikingPos_.x += 14 * dt;
  vikingPos_.y = 5 + std::sin(gameT_ * 4) * 0.15f;
}

// ─── Nuke: laser designator, the warhead falls, everything in a huge radius dies ─
void Game::doNuke(bool force) {
  if (!force) {
    if (isPaused()) return;
    if (nukeCD_ > 0 || wave_ < cfg::nuke_wave) {
      setStatus(wave_ < cfg::nuke_wave ? "Nuke unlocks wave " + std::to_string(cfg::nuke_wave) + "!" : "Nuke on cooldown!");
      return;
    }
    if (nukeInFlight_) { setStatus("Nuclear launch already in progress!"); return; }
    nukeCD_ = nukeCooldown();
  }
  waveNukes_++;
  nukeInFlight_ = true;
  // target: centroid of the living enemies, pushed out from the bunker
  float tx = 0, tz = 0;
  int n = 0;
  for (auto& e : allEnemies()) if (!e->dead) { tx += e->pos.x; tz += e->pos.z; n++; }
  if (n) {
    tx /= n;
    tz /= n;
    float d = std::sqrt(tx * tx + tz * tz);
    if (d < 3) { tx *= 3 / std::max(d, 0.1f); tz *= 3 / std::max(d, 0.1f); }
  } else {
    float a = rnd() * PI * 2;
    tx = std::cos(a) * 8;
    tz = std::sin(a) * 8;
  }
  setStatus("NUCLEAR LAUNCH DETECTED \xE2\x80\x94 2 seconds to impact!");
  audio::play("nukeWarn");

  // The dive path leans NUKE_SLANT toward the far side of the target as the camera
  // sees it, so the warhead and its streak come in from the top of the screen. (A
  // vertical one, seen from the steep overview camera, pointed sideways or at the
  // camera for targets in the lower half of the screen and ballooned near the lens.)
  glm::vec2 away = glm::normalize(glm::vec2(-camPos_.x, -camPos_.z));
  glm::vec3 up = glm::normalize(glm::vec3(away.x * NUKE_SLANT, 1, away.y * NUKE_SLANT));
  glm::mat4 tilt = glm::rotate(glm::mat4(1), std::acos(up.y), glm::normalize(glm::vec3(up.z, 0, -up.x)));   // +y → up
  auto along = [=](float h) { return glm::vec3(tx, 0, tz) + up * (h / up.y); };   // the path's point at height h
  auto laser = std::make_shared<Prop>();
  laser->mesh = laserMesh_;
  laser->mat.type = MatType::Basic;
  laser->mat.color = hexColor(0xff0000);
  laser->mat.transparent = true;
  laser->mat.opacity = .85f;
  laser->mat.fog = false;
  laser->m = glm::translate(glm::mat4(1), glm::vec3(tx, 0, tz) + up * 14.f) * tilt;   // 28 long: target → sky
  // target marker: the SC2 nuke reticle (additive), or a plain red ring
  auto dot = std::make_shared<Prop>();
  dot->mat.type = MatType::Basic;
  dot->mat.transparent = true;
  if (nukeTargetTex_) {
    dot->mesh = decalMesh_;
    dot->mat.map = nukeTargetTex_;
    dot->mat.blend = Blend::Additive;
    dot->mat.depthWrite = false;
  } else {
    dot->mesh = dotRingMesh_;
    dot->mat.color = hexColor(0xff0000);
    dot->mat.doubleSide = true;
  }
  dot->mat.fog = false;   // the reticle and the warning ring show in the dark
  dot->m = trs({tx, .05f, tz}, {-PI / 2, 0, 0});
  // the warhead streaks down the laser from the sky, nose first
  const Model* mm = models::get("nukeMissile");
  auto missile = std::make_shared<Prop>();
  float half = mm ? mm->defaultPos.y : 0;   // the template floats by half its height
  if (mm) {
    missile->model = mm;
    missile->cast = true;
    missile->m = glm::translate(glm::mat4(1), along(half + NUKE_SKY_Y)) * tilt * glm::rotate(glm::mat4(1), PI, {1, 0, 0});
  } else missile->visible = false;
  // Its trail, as in StarCraft: one straight streak from the tail up out of view,
  // a white-hot core in an orange glow (both taper toward the sky), and a flare at
  // the tail. (Sparks sprayed from the tail every frame read as fireworks.)
  auto glowProp = [&](Mesh* mesh, uint32_t col, float op, int order) {
    auto p = std::make_shared<Prop>();
    p->mesh = mesh;
    p->mat.type = MatType::Basic;
    p->mat.color = hexColor(col);
    p->mat.transparent = true;
    p->mat.blend = Blend::Additive;
    p->mat.depthWrite = false;
    p->mat.opacity = op;
    p->mat.fog = false;   // the burning trail shows out in the dark
    p->order = order;
    p->visible = false;   // until the warhead is in the air
    return p;
  };
  auto trailGlow = glowProp(trailMesh_, 0xff6a1a, .45f, 1), trailCore = glowProp(trailMesh_, 0xfff2d0, .95f, 2);
  auto flare = glowProp(fireballMesh_, 0xffd28a, .9f, 3);
  auto setTrail = [=](float bottom) {   // the streak from height `bottom` up the path into the sky
    float top = NUKE_SKY_Y + 8, len = (top - bottom) / up.y;
    glm::mat4 m = glm::translate(glm::mat4(1), along((top + bottom) / 2)) * tilt;
    trailGlow->m = glm::scale(m, {0.24f, len, 0.24f});
    trailCore->m = glm::scale(m, {0.07f, len, 0.07f});
  };
  auto warn = std::make_shared<Prop>();
  warn->mesh = ringMesh_;
  warn->mat.type = MatType::Basic;
  warn->mat.color = hexColor(0xff2200);
  warn->mat.doubleSide = true;
  warn->mat.transparent = true;
  warn->mat.opacity = .6f;
  warn->mat.fog = false;
  warn->m = trs({tx, .06f, tz}, {-PI / 2, 0, 0}, glm::vec3(0.1f));
  // at night the warning ring glows softly: a soft band along it, as strong as the night is dark
  auto warnGlow = glowProp(glowRingMesh_, 0xff3018, 0, 1);
  warnGlow->mat.map = glowTex_;
  warnGlow->mat.doubleSide = true;
  warnGlow->visible = true;
  for (auto& p : {laser, dot, missile, warn, warnGlow, trailGlow, trailCore, flare}) props_.push_back(p);

  auto warnT = std::make_shared<float>(0.f);
  auto spin = std::make_shared<float>(0.f);
  auto trailFading = std::make_shared<bool>(false);   // after the blast the trail outlives this ticker
  addTicker([=, this](float dt) {
    *warnT += dt;
    float prog = std::min(1.f, *warnT / WARN_DUR);
    warn->m = trs({tx, .06f, tz}, {-PI / 2, 0, 0}, glm::vec3(std::max(0.1f, prog * NUKE_RADIUS)));
    warn->mat.opacity = 0.4f + std::sin(*warnT * 12) * 0.3f;
    warnGlow->m = trs({tx, .07f, tz}, glm::vec3(0), glm::vec3(std::max(0.1f, prog * NUKE_RADIUS)));
    warnGlow->mat.opacity = 0.9f * dnFactor_ * (0.6f + 0.4f * std::sin(*warnT * 12));
    dot->mat.opacity = std::sin(*warnT * (6 + prog * 14)) > 0 ? 0.9f : 0.1f;
    laser->mat.opacity = 0.5f + std::sin(*warnT * 8) * 0.35f;
    // accelerating dive from the sky, lands at detonation
    float fall = glm::clamp((*warnT - NUKE_FALL_AT) / (WARN_DUR - NUKE_FALL_AT), 0.f, 1.f);
    float y = half + NUKE_SKY_Y * (1 - std::pow(fall, 1.6f)), tail = y + half * up.y;   // nose first: the tail is up the path
    *spin += dt * 4;
    missile->m = glm::translate(glm::mat4(1), along(y)) * tilt * glm::rotate(glm::mat4(1), PI, {1, 0, 0}) * glm::rotate(glm::mat4(1), *spin, {0, 1, 0});
    bool airborne = fall > 0;   // (a close-up camera looks up: nothing hangs in the sky before the dive)
    missile->visible = mm && airborne;
    trailGlow->visible = trailCore->visible = flare->visible = airborne;
    setTrail(tail);
    flare->m = trs(along(tail), glm::vec3(0), glm::vec3(0.07f + 0.015f * std::sin(*warnT * 40)));   // fireballMesh_: r 4
    if (*warnT < WARN_DUR) return true;
    // ── detonation ──
    audio::play("nukeBoom");
    setTrail(0);   // the streak ends in the blast, then fades
    *trailFading = true;
    auto fadeT = std::make_shared<float>(0.f);
    addTicker([=](float dt) {
      *fadeT += dt;
      float k = std::max(0.f, 1 - *fadeT / NUKE_TRAIL_FADE);
      trailGlow->mat.opacity = .45f * k;
      trailCore->mat.opacity = .95f * k;
      return k > 0;
    }, [=, this]() { for (auto& p : {trailGlow, trailCore}) props_.erase(std::remove(props_.begin(), props_.end(), p), props_.end()); });
    auto shock = std::make_shared<Prop>();
    shock->mesh = ringMesh_;
    shock->mat.type = MatType::Basic;
    shock->mat.color = hexColor(0xff6600);
    shock->mat.doubleSide = true;
    shock->mat.transparent = true;
    shock->mat.opacity = .9f;
    shock->mat.fog = false;
    shock->m = trs({tx, .1f, tz}, {-PI / 2, 0, 0}, glm::vec3(.2f));
    beams_.push_back({shock, 1.2f, 1.2f, 1, {tx, .1f, tz}});
    // the mushroom cloud of fire (it replaces the solid cone and ball the blast used to
    // show), and the fireball's light (at night it lights the field)
    mushrooms_.push_back({{tx, 0, tz}, 0});
    nukeLightT_ = 0;
    nukeLightPos_ = {tx, 3, tz};
    flash(0);
    spawnParts({tx, .5f, tz}, 0xff6600, 50);
    explosionFx({tx, 1, tz}, 0xff4400, 4.5f, true);   // GPU: sparks and embers (the cloud has the fire and the dust)
    for (int i = 0; i < 40; i++) {   // black debris flung out of the blast
      float a = rnd() * 2 * PI, v = 4 + rnd() * 7;
      emit(smoke_, tx, .4f, tz, std::cos(a) * v, 4 + rnd() * 6, std::sin(a) * v, 1.2f + rnd() * .6f, .16f, .1f, .95f, 0x1a1410, 0x2a2018, 14, .3f, 0);
    }
    spawnParts({tx, 3, tz}, 0xff4400, 30);
    spawnParts({tx, .5f, tz}, 0xffaa00, 25);
    int killed = 0;   // the bunker is hardened: only enemies die
    for (auto& e : allEnemies()) {
      if (e->dead) continue;
      float dx = e->pos.x - tx, dz = e->pos.z - tz;
      if (std::sqrt(dx * dx + dz * dz) <= NUKE_RADIUS) { killEnemy(e, true); killed++; }
    }
    setStatus("Nuclear detonation \xE2\x80\x94 " + std::to_string(killed) + " unit" + (killed != 1 ? "s" : "") + " annihilated. Bunker intact.");
    return false;
  }, [=, this]() {
    for (auto& p : {laser, dot, missile, warn, warnGlow, flare}) props_.erase(std::remove(props_.begin(), props_.end(), p), props_.end());
    if (!*trailFading) for (auto& p : {trailGlow, trailCore}) props_.erase(std::remove(props_.begin(), props_.end(), p), props_.end());
    nukeInFlight_ = false;
  });
}

// ─── Kill ────────────────────────────────────────────────────────────────────
void Game::killEnemy(const EnemyP& e, bool reward) {
  if (e->dead) return;
  e->dead = true;
  e->slowed = false;
  spawnParts(e->pos, e->type == 'r' ? 0xff6600 : 0x22cc44, e->type == 'r' ? 12 : 7);
  deathFx(*e);
  audio::play(e->type == 'r' ? "rdie" : "zdie");
  if (e->pool && e->visible) {   // death animation
    if (corpses_.size() >= 80) corpses_.erase(corpses_.begin());
    corpses_.push_back({e->pos, e->rotY, e->rotZ, e->pool, 0, rnd() < .5f ? -1.f : 1.f});
  }
  if (reward) {
    score_ += e->rScore;
    kills_++;
    waveKills_++;
    minerals_ += e->rMin;
    gas_ += e->rGas;
  }
}

// ─── Shooting ────────────────────────────────────────────────────────────────
// The round flies straight from the muzzle through the aim point at height ty
// and keeps going; height is visual only (hits are tested in the ground plane).
Bullet Game::makeBullet(float tx, float tz, float spread, Unit u, float ox, float oy, float oz, float ty) {
  const UnitDef& d = unitDef(u);
  float a = std::atan2(tz - oz, tx - ox) + spread;
  Bullet b;
  b.vx = std::cos(a) * d.bulletSpeed;
  b.vz = std::sin(a) * d.bulletSpeed;
  float D = std::hypot(tx - ox, tz - oz);
  b.vy = D > 0.05f ? (ty - oy) / D * d.bulletSpeed : 0;   // per-frame drop to reach ty at the aim point
  b.pos = {ox, oy, oz};
  b.color = d.bulletColor;
  b.speed = d.bulletSpeed;
  b.unit = u;
  orientBullet(b);
  return b;
}
void Game::orientBullet(Bullet& b) {
  float yaw = std::atan2(b.vx, b.vz), len = std::sqrt(b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
  if (len == 0) len = 1;
  float p = -std::asin(glm::clamp(b.vy / len, -1.f, 1.f));
  b.sinY = std::sin(yaw); b.cosY = std::cos(yaw); b.sinP = std::sin(p); b.cosP = std::cos(p);
}

// Units fire through the embrasure band of the wall facing their target: a
// horizontal ray at that height toward the bunker's axis meets the hull
// (cached per 5° bearing; cleared when the hull changes shape)
glm::vec3 Game::bunkerPort(float bearing) {
  int k = (int)std::lround(bearing / (PI / 36));
  auto it = portCache_.find(k);
  if (it != portCache_.end()) return it->second;
  float a = k * PI / 36, cx = std::cos(a), cz = std::sin(a);
  glm::vec3 p(cx * 1.45f, PORT_Y, cz * 1.45f), hit;
  if (rayBunker({cx * 5, PORT_Y, cz * 5}, {-cx, 0, -cz}, hit)) p = {hit.x + cx * 0.06f, PORT_Y, hit.z + cz * 0.06f};
  portCache_[k] = p;
  return p;
}

// Each click fires one volley: every unit in the bunker shoots once
void Game::shoot(float tx, float tz) {
  if (shootCD_ > 0 || wavePhase_ != WavePhase::Combat || ammo_ <= 0) return;
  ammo_--;
  shootCD_ = .1f;
  int n = roster_.empty() ? 1 : (int)roster_.size();
  bool stimExtra = stimT_ > 0 && stimVolley_++ % 2 == 0;   // stimmed: +1 round every other volley
  for (int i = 0; i < (int)roster_.size(); i++) {
    Unit u = roster_[i];
    float atx = tx, atz = tz;
    if (u == Unit::Marauder) {   // Marauders lock onto the nearest Armored enemy
      EnemyP best;
      float bd = 0;
      for (auto& e : allEnemies()) {
        if (e->dead || !e->armored) continue;
        float d = glm::length(e->pos);
        if (!best || d < bd) { best = e; bd = d; }
      }
      if (best) { atx = best->pos.x; atz = best->pos.z; }
    }
    float off = (i - (n - 1) / 2.f);
    glm::vec3 p = bunkerPort(std::atan2(atz, atx) + off * 0.2f);   // slits fan out ~11° apart
    Bullet b = makeBullet(atx, atz, off * .006f, u, p.x, p.y, p.z, AIM_Y);
    bullets_.push_back(b);
    if (u == Unit::Marine && stimT_ > 0) {
      bullets_.back().color = STIM_COLOR;
      if (stimExtra) {
        Bullet b2 = makeBullet(atx, atz, off * .006f + .02f, u, p.x, p.y + .05f, p.z, AIM_Y);
        b2.color = STIM_COLOR;
        bullets_.push_back(b2);
      }
    }
    muzzleFlash(p.x, p.y, p.z, b.vx / b.speed, b.vz / b.speed, unitDef(u).bulletColor, u == Unit::Marauder);
  }
  bool played[3] = {false, false, false};   // one gunshot voice per unit type
  for (Unit u : roster_) if (!played[(int)u]) { played[(int)u] = true; audio::play(unitDef(u).key); }
  if (ammo_ == 0) setStatus("Out of ammo \xE2\x80\x94 R: Airstrike  N: Nuke");
}

// ─── Ghost auto-snipe: threat = hp / distance ────────────────────────────────
void Game::ghostSnipeTick(float) {
  if (!snipeUnlocked_) return;
  int ghosts = (int)std::count(roster_.begin(), roster_.end(), Unit::Ghost);
  if ((int)snipeReady_.size() < ghosts) snipeReady_.resize(ghosts, 0);
  for (int gi = 0; gi < ghosts; gi++) {
    if (gameT_ < snipeReady_[gi]) continue;
    EnemyP target;
    float best = -1;
    for (auto& e : allEnemies()) {
      if (e->dead) continue;
      float d = std::max(0.5f, glm::length(e->pos)), threat = (e->hp ? e->hp : 1) / d;
      if (!target || threat > best) { target = e; best = threat; }
    }
    if (!target) return;
    snipeReady_[gi] = gameT_ + SNIPE_CD[snipeLvl_];
    // purple beam from the bunker to the target
    glm::vec3 tp = target->pos;
    float len = glm::length(tp);
    auto beam = std::make_shared<Prop>();
    beam->mesh = snipeMesh_;
    beam->mat.type = MatType::Basic;
    beam->mat.color = hexColor(0xcc44ff);
    beam->mat.transparent = true;
    beam->mat.opacity = .9f;
    glm::vec3 dir = len > 0 ? tp / len : glm::vec3(0, 1, 0);
    glm::vec3 side = std::abs(dir.y) < .99f ? glm::normalize(glm::cross(dir, glm::vec3(0, 1, 0))) : glm::vec3(1, 0, 0);
    glm::vec3 up = glm::cross(side, dir);
    glm::mat4 m(1);
    m[0] = glm::vec4(side, 0);
    m[1] = glm::vec4(dir * len, 0);   // the cylinder's axis (Y) along the shot
    m[2] = glm::vec4(up, 0);
    m[3] = glm::vec4(tp * 0.5f, 1);
    beam->m = m;
    beams_.push_back({beam, .3f, .3f, 0, tp * 0.5f});
    killEnemy(target, true);
    spawnParts(tp, 0xcc44ff, 18);
    setStatus(std::string("Ghost snipe \xE2\x80\x94 ") + (target->type == 'r' ? "Roach" : "Zergling") + " eliminated!");
    audio::play("snipe");
  }
}

// ─── Bunker fire (HP < 50%) ──────────────────────────────────────────────────
// Damage spots on the hull ignite one after another as HP falls: flame tongues,
// embers and a smoke plume that thickens with damage; a flickering light.
void Game::updateBunkerFire(float dt) {
  float hpPct = bunkerHp_ / maxHp_;
  if (phase_ != Phase::Playing || hpPct >= FIRE_HP) { fireI_ = 0; return; }
  if (!fireSpotsValid_) {
    fireSpots_.clear();
    for (auto& xz : FIRE_SPOTS_XZ) {
      glm::vec3 hit;
      bool h = rayBunker({xz[0], 10, xz[1]}, {0, -1, 0}, hit);
      fireSpots_.push_back({xz[0], h ? hit.y : 1.2f, xz[1]});
    }
    fireSpotsValid_ = true;
  }
  float sev = std::min(1.f, (FIRE_HP - hpPct) / 0.4f);   // 0 at 50% HP → 1 at 10% and below
  int lit = std::min((int)fireSpots_.size(), 1 + (int)std::floor(sev * fireSpots_.size()));
  float lx = 0, lz = 0;
  for (int s = 0; s < lit; s++) {
    glm::vec3 p = fireSpots_[s];
    float* acc = fireAccum_[s];
    lx += p.x;
    lz += p.z;
    acc[0] += (8 + 12 * sev) * dt;
    acc[1] += (2.5f + 5 * sev) * dt;
    acc[2] += (1 + 5 * sev) * dt;
    while (acc[0] >= 1) {   // flame tongue (modest alpha: additive overlap must not blow out to white)
      acc[0]--;
      float sz = (0.34f + 0.18f * sev) * (0.7f + 0.6f * rnd());
      emit(glow_, p.x + (rnd() - .5f) * .24f, p.y + rnd() * .05f, p.z + (rnd() - .5f) * .24f, (rnd() - .5f) * .5f, .9f + rnd() * .9f + .6f * sev,
           (rnd() - .5f) * .5f, .35f + rnd() * .3f, sz, .08f, .55f, 0xffb347, 0xff2200, -1.5f, 1.0f, 0);
    }
    while (acc[1] >= 1) {   // smoke: drifts downwind, grows, darker when heavy
      acc[1]--;
      emit(smoke_, p.x + (rnd() - .5f) * .2f, p.y + .35f, p.z + (rnd() - .5f) * .2f, .25f + (rnd() - .5f) * .3f, .55f + rnd() * .5f,
           (rnd() - .5f) * .3f, 2.0f + rnd() * 1.2f, .35f, 1.4f + .6f * sev, .28f + .3f * sev, sev > .6f ? 0x141414 : 0x3a3a3a, 0x6a6a6a, -.2f, .3f, 1);
    }
    while (acc[2] >= 1) {   // ember
      acc[2]--;
      emit(glow_, p.x, p.y + .1f, p.z, (rnd() - .5f) * 1.6f, 1.5f + rnd() * 1.8f, (rnd() - .5f) * 1.6f, .8f + rnd() * .7f, .07f, .03f, 1, 0xffc060,
           0xff3000, -.3f, .6f, 0);
    }
  }
  firePos_ = {lx / lit, 1.8f, lz / lit};
  fireI_ = (0.6f + 2.2f * sev) * (0.8f + 0.2f * std::sin(gameT_ * 23) * std::sin(gameT_ * 7.3f));
}

// ─── Camera rig ──────────────────────────────────────────────────────────────
// Always looks at the bunker. Zoom: tactical overview → close-up (the camera
// lowers its angle as it comes in); Insert / Delete rotate 90°; Home resets.
namespace cam {
const float farDist = std::hypot(22.f, 8.f), farElev = std::atan2(22.f, 8.f), nearDist = 6.5f, nearElev = 0.62f, nearLookY = 1.0f;
}
void Game::updateCamera(float dt) {
  float k = std::min(1.f, dt * 9);
  zoom_ += (zoomT_ - zoom_) * k;
  camYaw_ += (camYawT_ - camYaw_) * k;
  float z = zoom_ * zoom_ * (3 - 2 * zoom_);
  float dist = cam::farDist + (cam::nearDist - cam::farDist) * z;
  float elev = cam::farElev + (cam::nearElev - cam::farElev) * z;
  float ly = cam::nearLookY * z, hz = std::cos(elev) * dist;
  camPos_ = {std::sin(camYaw_) * hz, ly + std::sin(elev) * dist, std::cos(camYaw_) * hz};
  view_ = glm::lookAt(camPos_, glm::vec3(0, ly, 0), glm::vec3(0, 1, 0));
  proj_ = glm::perspective(glm::radians(55.f), canvasW_ / std::max(1.f, canvasH_), 0.1f, 200.f);
}
void Game::rotateView(int dir) {
  float q = PI / 2;
  camYawT_ = std::round(camYawT_ / q) * q + dir * q;   // always lands on a 90° step
}
void Game::resetView() {
  zoomT_ = 0;
  camYawT_ = std::round(camYaw_ / (2 * PI)) * 2 * PI;   // back to north-up the short way
}
bool Game::project(const glm::vec3& p, float& sx, float& sy) const {
  glm::vec4 c = proj_ * view_ * glm::vec4(p, 1);
  if (c.w <= 0) return false;
  glm::vec3 n = glm::vec3(c) / c.w;
  if (n.z > 1) return false;
  sx = canvasX_ + (n.x * 0.5f + 0.5f) * canvasW_;
  sy = canvasY_ + (-n.y * 0.5f + 0.5f) * canvasH_;
  return true;
}
// Cursor (window coords) → point on the ground (y = 0)
bool Game::groundPoint(double cx, double cy, glm::vec3& out) const {
  float nx = (float)((cx - canvasX_) / canvasW_) * 2 - 1, ny = -(float)((cy - canvasY_) / canvasH_) * 2 + 1;
  glm::mat4 inv = glm::inverse(proj_ * view_);
  glm::vec4 a = inv * glm::vec4(nx, ny, -1, 1), b = inv * glm::vec4(nx, ny, 1, 1);
  glm::vec3 o = glm::vec3(a) / a.w, d = glm::normalize(glm::vec3(b) / b.w - o);
  if (std::abs(d.y) < 1e-6f) return false;
  float t = -o.y / d.y;
  if (t < 0) return false;
  out = o + d * t;
  return std::abs(out.x) <= 60 && std::abs(out.z) <= 60;   // the 120×120 ground plane
}

// ─── Day / night (experimental) ──────────────────────────────────────────────
struct GfxLights { uint32_t ambCol; float amb, sun, fill; };
static const GfxLights GFX_HIGH = {0x99aabb, 0.7f, 1.8f, 0.5f}, GFX_LOW = {0xaaaaaa, 1.0f, 1.1f, 0};

void Game::applyDNLights(float f) {
  dnLightF_ = f;
  const GfxLights& b = gfxHigh_ ? GFX_HIGH : GFX_LOW;
  ambCol_ = lerp3(hexColor(b.ambCol), hexColor(0x112233), f);
  ambI_ = b.amb * (1 - f) + 0.18f * f;
  // sun: day full → night off, through a warm low sun at dusk / dawn
  sunCol_ = f < 0.45f ? lerp3(hexColor(0xfff4cc), hexColor(0xff9a55), f / 0.45f) : lerp3(hexColor(0xff9a55), hexColor(0x001122), (f - 0.45f) / 0.55f);
  sunI_ = b.sun * (1 - f);
  fillI_ = b.fill * (1 - f);
  bunkerLightI_ = f * 5.5f;
  // Fog: the radius shrinks by the same ratio every second (geometric blend).
  // Blended linearly, it stayed outside the battlefield for the first 60% of
  // dusk, then closed in on the bunker all at once.
  fogNear_ = DN_DAY_FOG_NEAR * std::pow(DN_NIGHT_FOG_NEAR / DN_DAY_FOG_NEAR, f);
  fogFar_ = DN_DAY_FOG_FAR * std::pow(DN_NIGHT_FOG_FAR / DN_DAY_FOG_FAR, f);
  fogCol_ = lerp3(hexColor(0x2a3a18), hexColor(0x050a12), f);
}

// Night factor at time t of the cycle: 0 through the day, eased up through dusk,
// 1 through the night, eased down through dawn (smootherstep: no jump in the
// level, its rate or its acceleration). The old curve snapped from full night
// back to day at the end of dusk, then faded in again.
static float dnCycleFactor(float t) {
  auto ease = [](float x) { return x * x * x * (x * (x * 6 - 15) + 10); };
  if (t < DN_DAY) return 0;
  if (t < DN_DAY + DN_FADE) return ease((t - DN_DAY) / DN_FADE);
  if (t < DN_DAY + DN_FADE + DN_NIGHT) return 1;
  return 1 - ease((t - DN_DAY - DN_FADE - DN_NIGHT) / DN_FADE);
}

void Game::updateDayNight(float dt) {
  if (dnEnabled_) dnTime_ = std::fmod(dnTime_ + dt, DN_CYCLE);
  float target = dnEnabled_ ? dnCycleFactor(dnTime_) : 0;
  // Slew-limited: the cycle itself moves slower than this, so only switching
  // the cycle on / off is blended (the night lifts over DN_SLEW s, not at once)
  float step = dt / DN_SLEW;
  dnFactor_ = std::max(dnFactor_ - step, std::min(dnFactor_ + step, target));
  // the lights follow the factor lifted by a flare: re-applied also as one lights up or burns out
  float lit = dnLit();
  if (lit != dnLightF_) applyDNLights(lit);
}

// Top-bar label (a night still lifting after the cycle restarted at day reads DAWN)
std::string Game::dayNightLabel() const {
  if (!dnEnabled_) return "";
  std::string phase = dnFactor_ > 0.97f ? "NIGHT" : dnFactor_ < 0.03f ? "DAY" : dnTime_ >= DN_DAY && dnTime_ < DN_DAY + DN_FADE + DN_NIGHT ? "DUSK" : "DAWN";
  return std::string(dnFactor_ > 0.5f ? "\xE2\x98\xBD " : "\xE2\x98\x80 ") + phase;
}

// Top-bar countdown to the next phase of the cycle: "DUSK IN 0:42" (day), "NIGHT IN" (dusk),
// "DAWN IN" (night), "DAY IN" (dawn)
std::string Game::dayNightNext() const {
  const float ends[4] = {DN_DAY, DN_DAY + DN_FADE, DN_DAY + DN_FADE + DN_NIGHT, DN_CYCLE};
  const char* names[4] = {"DUSK", "NIGHT", "DAWN", "DAY"};
  int k = 0;
  while (k < 3 && dnTime_ >= ends[k]) k++;
  float until = ends[k] - dnTime_;
  // the label turns NIGHT / DAY at factor 0.97 / 0.03, a moment before that phase begins:
  // then count to the one after ("NIGHT IN 0:01" under NIGHT read wrong)
  const char* shown = dnFactor_ > 0.97f ? "NIGHT" : dnFactor_ < 0.03f ? "DAY" : "";
  if (!strcmp(names[k], shown)) {
    until = k < 3 ? ends[k + 1] - dnTime_ : DN_CYCLE + ends[0] - dnTime_;
    k = (k + 1) % 4;
  }
  int sec = std::max(0, (int)std::ceil(until));
  char buf[32];
  snprintf(buf, sizeof buf, "%s IN %d:%02d", names[k], sec / 60, sec % 60);
  return buf;
}

void Game::toggleDayNight() {
  dnEnabled_ = !dnEnabled_;
  // back to day: the night lifts over DN_SLEW s (updateDayNight — run by frame()
  // while the cycle is off even when the game is paused or over)
  if (!dnEnabled_) dnTime_ = 0;
  setStatus(dnEnabled_ ? "Day/Night cycle ON \xE2\x80\x94 [Y] to toggle" : "Day/Night cycle OFF");
}
void Game::setDayNightTime(float t) {   // test: the cycle on at t seconds, lights set at once
  dnEnabled_ = true;
  dnTime_ = std::fmod(std::max(0.f, t), DN_CYCLE);
  dnFactor_ = dnCycleFactor(dnTime_);
  applyDNLights(dnLit());
}

// ─── Viking flare (night airstrikes) ─────────────────────────────────────────
// Strength: up over 0.4 s, full, down over the last 1.5 s; 0 when none burns
float Game::flareK() const {
  if (flareT_ < 0) return 0;
  return std::min(1.f, flareT_ / 0.4f) * std::min(1.f, (FLARE_DUR - flareT_) / 1.5f);
}
// Nuke fireball light: up in 0.08 s, full for NUKE_LIGHT_HOLD s, then cooling; 0 when none
float Game::nukeLightK() const {
  if (nukeLightT_ < 0) return 0;
  float t = nukeLightT_;
  return std::min(1.f, t / 0.08f) * (t < NUKE_LIGHT_HOLD ? 1 : std::exp(-(t - NUKE_LIGHT_HOLD) / 1.4f)) * std::min(1.f, NUKE_LIGHT_DUR - t);
}
// The factor the lights and fog use: a flare lifts the night toward FLARE_F, a nuke's
// fireball toward NUKE_LIGHT_F. The top-bar label and the "is it night" tests keep the
// true factor (dnFactor_)
float Game::dnLit() const {
  return dnFactor_ - std::max(flareK() * std::max(0.f, dnFactor_ - FLARE_F), nukeLightK() * std::max(0.f, dnFactor_ - NUKE_LIGHT_F));
}

void Game::startFlare() {
  flareT_ = 0;   // one already burning starts over
  // it hangs beyond the bunker as the camera sees it, so it is not between the two
  glm::vec2 c(camPos_.x, camPos_.z);
  flareDir_ = glm::length(c) > 1e-4f ? -glm::normalize(c) : glm::vec2(0, -1);
  if (!flareCore_) {
    auto glowProp = [&](uint32_t col, int order) {
      auto p = std::make_shared<Prop>();
      p->mesh = fireballMesh_;
      p->mat.type = MatType::Basic;
      p->mat.color = hexColor(col);
      p->mat.transparent = true;
      p->mat.blend = Blend::Additive;
      p->mat.depthWrite = false;
      p->order = order;
      p->visible = false;   // until it is lit
      props_.push_back(p);
      return p;
    };
    flareGlow_ = glowProp(0xffd9a0, 1);
    flareCore_ = glowProp(0xfff6d8, 2);
  }
  setStatus("Viking flare \xE2\x80\x94 the battlefield is lit!");
}

// Live frames only: it burns and falls in game time
void Game::updateFlare(float dt) {
  if (flareT_ < 0) return;
  flareT_ += dt;
  if (flareT_ >= FLARE_DUR) { endFlare(); return; }
  float k = flareK();
  glm::vec2 side(-flareDir_.y, flareDir_.x), xz = flareDir_ * 2.5f + side * (0.35f * std::sin(flareT_ * 1.3f));   // sways from the centre
  glm::vec3 p(xz.x, 11 - (11 - 6.5f) * flareT_ / FLARE_DUR, xz.y);   // drifts down under its parachute
  flareCore_->m = trs(p, glm::vec3(0), glm::vec3(0.22f / 4));   // fireballMesh_: r 4
  flareGlow_->m = trs(p, glm::vec3(0), glm::vec3(0.8f / 4));
  flareCore_->mat.opacity = 0.95f * k;
  flareGlow_->mat.opacity = 0.35f * k;
  flareCore_->visible = flareGlow_->visible = k > 0;
}

void Game::endFlare() {
  flareT_ = -1;
  for (const PropP& p : {flareCore_, flareGlow_}) props_.erase(std::remove(props_.begin(), props_.end(), p), props_.end());
  flareCore_.reset();
  flareGlow_.reset();
}

// ─── Scheduler ───────────────────────────────────────────────────────────────
void Game::tickScheduler(float dt) {
  std::vector<std::function<void()>> due;
  for (size_t i = 0; i < timers_.size();) {
    timers_[i].t -= dt;
    if (timers_[i].t <= 0) { due.push_back(std::move(timers_[i].fn)); timers_.erase(timers_.begin() + i); }
    else i++;
  }
  for (auto& f : due) f();
  std::vector<Ticker> cur;
  cur.swap(tickers_);
  std::vector<Ticker> keep;
  for (auto& tk : cur) {
    if (tk.fn(dt)) keep.push_back(std::move(tk));
    else if (tk.clean) tk.clean();
  }
  for (auto& tk : tickers_) keep.push_back(std::move(tk));   // added while ticking
  tickers_ = std::move(keep);
}
void Game::clearScheduler() {
  auto cur = std::move(tickers_);
  tickers_.clear();
  for (auto& tk : cur) if (tk.clean) tk.clean();
  timers_.clear();
}

// ─── Waves ───────────────────────────────────────────────────────────────────
void Game::startWave() {
  // Gentle early ramp: 2, 3, 4, 6, 8, 10, 13, 16, 19, 23, 27 … then linear
  static const int ZT[12] = {0, 2, 3, 4, 6, 8, 10, 13, 16, 19, 23, 27};
  int zc = wave_ < 12 ? ZT[wave_] : 27 + (wave_ - 11) * 4;
  int rc = wave_ >= cfg::roach_wave ? (int)std::floor((wave_ - cfg::roach_wave + 1) * 1.8f) : 0;
  bool finalWave = mode_ == Mode::Campaign && wave_ == cfg::victory_wave;
  if (finalWave) { zc = (int)std::round(zc * 1.25f); rc = (int)std::round(rc * 1.25f); }   // final assault
  spawnInterval_ = std::max(0.4f, 1.4f - wave_ * 0.08f);
  spawnQueue_.clear();
  spawnQueue_.insert(spawnQueue_.end(), zc, 'z');
  spawnQueue_.insert(spawnQueue_.end(), rc, 'r');
  std::shuffle(spawnQueue_.begin(), spawnQueue_.end(), rng_);
  spawnTimer_ = 0;
  wavePhase_ = WavePhase::Combat;
  waveKills_ = waveNukes_ = waveAirs_ = waveCrates_ = 0;
  std::string roachMsg = rc > 0 ? " + " + std::to_string(rc) + " Roach" + (rc > 1 ? "es" : "") : "";
  std::string boostMsg = wave_ == cfg::metabolic_wave ? " \xE2\x80\x94 Metabolic Boost: Zerglings run faster!"
                         : endlessWaves() ? " \xE2\x80\x94 bunker reinforced \xC3\x97" + std::to_string(endlessWaves()) : "";
  if (finalWave) setStatus("FINAL WAVE " + std::to_string(wave_) + ": " + std::to_string(zc) + " Zerglings" + roachMsg + " \xE2\x80\x94 hold the line!");
  else setStatus("Wave " + std::to_string(wave_) + ": " + std::to_string(zc) + " Zergling" + (zc > 1 ? "s" : "") + roachMsg + boostMsg);
  audio::play("wave");
}

void Game::waveCleared() {
  wavePhase_ = WavePhase::Cleared;
  int bm = 40 + wave_ * 15, bg = 10 + wave_ * 5;
  minerals_ += bm;
  gas_ += bg;
  setStatus("Wave " + std::to_string(wave_) + " cleared! +\xE2\x97\x86" + std::to_string(bm) + " +\xE2\x96\xB2" + std::to_string(bg) + " \xE2\x80\x94 " +
            (mode_ == Mode::Endless ? "Armory open (B) \xC2\xB7 resupply incoming..." : "Resupply incoming..."));
  audio::play("clear");
  after(1.5f, [this]() {
    // Countdown: 3... 2... 1... once the magazine is full again
    auto countdown = [this]() {
      setStatus("Next wave in 3...");
      after(1, [this]() { setStatus("Next wave in 2..."); });
      after(2, [this]() { setStatus("Next wave in 1..."); });
      after(3, [this]() {
        wave_++;
        reinforce();   // Endless: the bunker is reinforced as the swarm grows
        setStatus("Wave " + std::to_string(wave_) + " \xE2\x80\x94 incoming!");
        startWave();
      });
    };
    // One crate per wave (see resupply): the Viking's crate is still on its way,
    // or already came in — no second crate run
    const char* how = !crateCbs_.empty() ? "the Viking's crate on its way" : waveCrates_ > 0 ? "the Viking's crate already in" : "a supply crate";
    logInfo("wave %d cleared: resupply by %s (crates carried this wave: %d)", wave_, how, waveCrates_);
    if (!crateCbs_.empty()) { awaitCrate(countdown); setStatus("Resupply: the Viking's crate restocks the bunker"); return; }
    if (waveCrates_ > 0) { ammo_ = maxAmmo_; audio::play("crate"); countdown(); return; }
    float tx, tz;
    randPos(tx, tz);
    makeBeam(tx, tz, 0xff8800);
    spawnParts({tx, .5f, tz}, 0xff8800, 14);
    awaitCrate(countdown);
    deliverCrate(tx, tz);
  });
  // Campaign won: the resupply above waits (the scheduler is frozen) until "Continue · Endless"
  if (mode_ == Mode::Campaign && wave_ >= cfg::victory_wave) campaignVictory();
  else autoSave();   // checkpoint: the title screen offers CONTINUE from here
}

// Max HP and the magazine grow by a step per Endless wave; endlessSteps_ counts
// the steps granted, so a resumed checkpoint or a wave-skipping cheat catches up
void Game::reinforce() {
  int n = endlessWaves() - endlessSteps_;
  if (n <= 0) return;
  endlessSteps_ += n;
  maxHp_ += cfg::endless_hp * n;
  bunkerHp_ += cfg::endless_hp * n;
  maxAmmo_ += cfg::endless_ammo * n;
  ammo_ += cfg::endless_ammo * n;
  floatTexts_.push_back({{0, 3.4f, 0}, "REINFORCED  +" + std::to_string((int)(cfg::endless_hp * n)) + " HP  +" +
                         std::to_string((int)std::round(cfg::endless_dmg * 100 * n)) + "% DMG", 2.6f, 2.6f, 0xffd24a});
}
float Game::repairMult() const { return 1 + cfg::endless_repair * endlessWaves(); }
// Lv3 self-repair and the Super Stimpack (while stimmed) stack; Endless speeds it up
float Game::repairRate() const {
  return ((hpLvl_ >= 3 ? cfg::repair_rate : 0) + (superStimUnlocked_ && stimT_ > 0 ? cfg::superstim_heal : 0)) * repairMult();
}

// ─── Endless armory ──────────────────────────────────────────────────────────
static const int ARMORY_BASE[4][2] = {{300, 75}, {400, 100}, {250, 60}, {350, 120}};
void Game::armoryCost(int k, int& m, int& g) const {
  float f = std::pow(cfg::armory_growth, (float)armory_[k]);
  m = (int)std::round(ARMORY_BASE[k][0] * f / 5) * 5;
  g = (int)std::round(ARMORY_BASE[k][1] * f / 5) * 5;
}
float Game::armoryCdMult(int n) const {
  if (n < 0) n = armory_[3];
  return std::max(cfg::armory_cd_min, std::pow(cfg::armory_cd, (float)n));
}
float Game::strikeCooldown() const { return cfg::airstrike_cd * armoryCdMult(); }
float Game::nukeCooldown() const { return cfg::nuke_cd * armoryCdMult(); }
bool Game::armoryMaxed(int k) const { return k == 3 && armoryCdMult() <= cfg::armory_cd_min; }
bool Game::armoryAffordable() const {
  for (int k = 0; k < 4; k++) {
    int m, g;
    armoryCost(k, m, g);
    if (!armoryMaxed(k) && minerals_ >= m && gas_ >= g) return true;
  }
  return false;
}

std::string Game::needText(float m, float g) const {
  int nm = std::max(0, (int)std::ceil(m - minerals_)), ng = std::max(0, (int)std::ceil(g - gas_));
  std::string s = "Not enough \xE2\x80\x94 need ";
  if (nm) s += "\xE2\x97\x86" + std::to_string(nm);
  if (nm && ng) s += " ";
  if (ng) s += "\xE2\x96\xB2" + std::to_string(ng);
  return s + " more";
}

// ─── Upgrades (command card 1-4) ─────────────────────────────────────────────
void Game::buy(const std::string& type) {
  float spent0 = minerals_ + gas_;
  std::string s;
  auto tryBuy = [&](int& lvl, const int (*costs)[2], const char* maxMsg) -> bool {
    if (lvl >= 3) { s = maxMsg; return false; }
    int m = costs[lvl][0], g = costs[lvl][1];
    if (minerals_ < m || gas_ < g) { s = needText(m, g); return false; }
    minerals_ -= m;
    gas_ -= g;
    lvl++;
    return true;
  };
  if (type == "dmg") { if (tryBuy(dmgLvl_, cfg::dmg_costs, "Damage MAX!")) s = "Damage Lv. " + std::to_string(dmgLvl_); }
  else if (type == "armor") { if (tryBuy(armorLvl_, cfg::armor_costs, "Armor MAX!")) s = "Armor Lv. " + std::to_string(armorLvl_); }
  else if (type == "hp") {
    if (tryBuy(hpLvl_, cfg::hp_costs, "HP MAX!")) {
      maxHp_ += 250;
      bunkerHp_ = std::min(bunkerHp_ + 250, maxHp_);
      updateBunkerVisual();
      s = hpLvl_ == 3 ? "HP MAX \xE2\x80\x94 Shrike turret online, self-repair active!" : hpLvl_ == 2 ? "HP Lv. 2 \xE2\x80\x94 Neosteel frame installed" : "HP Lv. " + std::to_string(hpLvl_);
    }
  } else if (type == "ammo") {
    if (tryBuy(ammoLvl_, cfg::ammo_costs, "Ammo MAX!")) {
      maxAmmo_ += 10;
      ammo_ = std::min(ammo_ + 10, maxAmmo_);
      s = ammoLvl_ == 3 ? "Ammo MAX!" : "Ammo Lv. " + std::to_string(ammoLvl_) + " (+10)";
    }
  } else s = "Unknown";
  setStatus(s);
  audio::play(minerals_ + gas_ < spent0 ? "buy" : "deny");
}

void Game::buyArmory(int k) {
  static const char* NAMES[4] = {"Hull plating", "Weapon tuning", "Magazine", "Fleet logistics"};
  if (mode_ != Mode::Endless) return;
  if (armoryMaxed(k)) { setStatus(std::string(NAMES[k]) + " is maxed"); audio::play("deny"); return; }
  int m, g;
  armoryCost(k, m, g);
  if (minerals_ < m || gas_ < g) { setStatus(needText(m, g)); audio::play("deny"); return; }
  minerals_ -= m;
  gas_ -= g;
  armory_[k]++;
  if (k == 0) { maxHp_ += cfg::armory_hp; bunkerHp_ += cfg::armory_hp; }
  if (k == 2) { maxAmmo_ += cfg::armory_ammo; ammo_ += cfg::armory_ammo; }
  if (k == 3) { airCD_ = std::min(airCD_, strikeCooldown()); nukeCD_ = std::min(nukeCD_, nukeCooldown()); }
  char fx[80], tot[80];
  int n = armory_[k];
  switch (k) {
    case 0: snprintf(fx, sizeof fx, "+%d max HP", (int)cfg::armory_hp); snprintf(tot, sizeof tot, "+%d HP", n * (int)cfg::armory_hp); break;
    case 1: snprintf(fx, sizeof fx, "+%d%% damage", (int)std::round(cfg::armory_dmg * 100)); snprintf(tot, sizeof tot, "+%d%% damage", (int)std::round(n * cfg::armory_dmg * 100)); break;
    case 2: snprintf(fx, sizeof fx, "+%d rounds", cfg::armory_ammo); snprintf(tot, sizeof tot, "+%d rounds", n * cfg::armory_ammo); break;
    default: snprintf(fx, sizeof fx, "strike & nuke cooldowns \xE2\x88\x92%d%%", (int)std::round((1 - cfg::armory_cd) * 100)); snprintf(tot, sizeof tot, "cooldowns \xC3\x97%.2f", armoryCdMult(n)); break;
  }
  setStatus(std::string(NAMES[k]) + " \xC3\x97" + std::to_string(n) + ": " + fx + " (total " + tot + ")");
  audio::play("buy");
}

// ─── Roster ──────────────────────────────────────────────────────────────────
std::string Game::hireUnit(Unit u) {
  const UnitDef& d = unitDef(u);
  if (slotsLeft() < d.slots) return "Need " + std::to_string(d.slots) + " slots (only " + std::to_string(slotsLeft()) + " free)";
  if (minerals_ < d.cost[0] || gas_ < d.cost[1]) return needText(d.cost[0], d.cost[1]);
  minerals_ -= d.cost[0];
  gas_ -= d.cost[1];
  roster_.push_back(u);
  if (u == Unit::Ghost) snipeReady_.push_back(0);   // a new ghost may snipe at once
  syncMarines();
  return std::string(d.name) + " hired! (" + std::to_string(slotsUsed()) + "/6 slots)";
}

void Game::dismissUnit(int idx) {
  if (idx < 0 || idx >= (int)roster_.size()) return;
  if (roster_.size() <= 1) { setStatus("The bunker needs at least one unit!"); return; }   // an empty bunker fires nothing
  Unit u = roster_[idx];
  const UnitDef& d = unitDef(u);
  if (u == Unit::Ghost) {   // snipe cooldowns are kept per ghost in roster order
    int ord = (int)std::count(roster_.begin(), roster_.begin() + idx, Unit::Ghost);
    if (ord < (int)snipeReady_.size()) snipeReady_.erase(snipeReady_.begin() + ord);
  }
  int rm = (int)std::floor(d.cost[0] * 0.75f), rg = (int)std::floor(d.cost[1] * 0.75f);   // 75% refund
  minerals_ += rm;
  gas_ += rg;
  roster_.erase(roster_.begin() + idx);
  syncMarines();
  setStatus(std::string(d.name) + " dismissed \xE2\x80\x94 refunded \xE2\x97\x86" + std::to_string(rm) + " \xE2\x96\xB2" + std::to_string(rg));
}

void Game::researchUpgrade(const std::string& key, int tier) {
  struct UD { const char* key; Unit unit; const char* name; int cost[2]; struct T { int cost[2]; const char* desc; } tiers[3]; int nt; };
  static const UD U[3] = {
    {"superstim", Unit::Marine, "Super Stimpack", {250, 150}, {}, 0},
    {"concussive", Unit::Marauder, "Concussive Shell", {300, 75},
     {{{250, 80}, "AOE Lv.1 \xE2\x80\x94 slow blast radius 1.5"}, {{400, 130}, "AOE Lv.2 \xE2\x80\x94 radius 2.2"}, {{600, 200}, "AOE Lv.3 \xE2\x80\x94 radius 3.0 (MAX)"}}, 3},
    {"snipe", Unit::Ghost, "Snipe", {250, 100},
     {{{200, 80}, "Snipe Lv.1 \xE2\x80\x94 CD 8s \xE2\x86\x92 6s"}, {{320, 130}, "Snipe Lv.2 \xE2\x80\x94 CD 6s \xE2\x86\x92 4s"}, {{500, 200}, "Snipe Lv.3 \xE2\x80\x94 CD 4s \xE2\x86\x92 2.5s (MAX)"}}, 3},
  };
  if (key == "sensor") {   // Sensor Tower: needs no unit; tier = the level owned (-1 = not researched)
    int lvl = std::max(0, tier);
    if (lvl >= 3) return;
    const int* c = cfg::sensor_costs[lvl];
    if (minerals_ < c[0] || gas_ < c[1]) { setStatus(needText(c[0], c[1])); return; }
    minerals_ -= c[0];
    gas_ -= c[1];
    sensorLvl_ = lvl + 1;
    char r[16];
    snprintf(r, sizeof r, "%g", (double)cfg::sensor_range[sensorLvl_]);
    setStatus("Sensor Tower lv." + std::to_string(sensorLvl_) + " \xE2\x80\x94 blips out to " + r);
    return;
  }
  const UD* d = nullptr;
  for (const UD& x : U) if (key == x.key) d = &x;
  if (!d) return;
  if (std::find(roster_.begin(), roster_.end(), d->unit) == roster_.end()) {
    setStatus(std::string("Need a ") + unitDef(d->unit).key + " in bunker first!");
    return;
  }
  if (tier < 0) {
    if (minerals_ < d->cost[0] || gas_ < d->cost[1]) { setStatus(needText(d->cost[0], d->cost[1])); return; }
    minerals_ -= d->cost[0];
    gas_ -= d->cost[1];
    if (key == "concussive") concussiveUnlocked_ = true;
    if (key == "snipe") snipeUnlocked_ = true;
    if (key == "superstim") superStimUnlocked_ = true;
    setStatus(std::string(d->name) + " researched!");
  } else {
    if (tier >= d->nt) return;
    const auto& t = d->tiers[tier];
    if (minerals_ < t.cost[0] || gas_ < t.cost[1]) { setStatus(needText(t.cost[0], t.cost[1])); return; }
    minerals_ -= t.cost[0];
    gas_ -= t.cost[1];
    if (key == "concussive") concussiveLvl_ = tier + 1;
    if (key == "snipe") snipeLvl_ = tier + 1;
    setStatus(t.desc);
  }
}

void Game::resetRoster() {
  roster_ = {Unit::Marine, Unit::Marine};
  concussiveUnlocked_ = snipeUnlocked_ = superStimUnlocked_ = false;
  snipeLvl_ = concussiveLvl_ = 0;
  sensorLvl_ = 0;
  snipeReady_.clear();
  syncMarines();
}

// ─── Main update (game.js loop) ──────────────────────────────────────────────
void Game::update(float dt) {
  updateCamera(dt);   // also while paused: the player can look around
  muzzleI_ = std::max(0.f, muzzleI_ - dt * 45);   // the flash decays in ~60 ms
  if (phase_ != Phase::Playing || isPaused()) {   // game clock, timers and events are frozen
    // A night switched off still lifts behind the menu, title and results (it is
    // not game time: with the cycle off nothing else moves). Left frozen, the
    // title showed the night and the next game started in it.
    if (!dnEnabled_ && dnFactor_ > 0) updateDayNight(dt);
    return;
  }
  gameT_ += dt;
  dmgFlashCD_ = std::max(0.f, dmgFlashCD_ - dt);
  shootCD_ = std::max(0.f, shootCD_ - dt);
  holdFire(dt);
  airCD_ = std::max(0.f, airCD_ - dt);
  if (stimT_ > 0) {
    stimT_ = std::max(0.f, stimT_ - dt);
    if (superStimUnlocked_ && rnd() < dt * 14) {   // Super Stimpack: the squad patches the bunker up
      float a = rnd() * 6.28f, r = 1.1f + rnd() * .4f;
      emit(glow_, std::cos(a) * r, .3f + rnd() * .6f, std::sin(a) * r, 0, 1.4f, 0, .7f, .22f, .05f, .9f, 0x6aff8a, 0x1a9a3a, 0, 0, 0);
    }
  }
  if (nukeCD_ > 0) nukeCD_ = std::max(0.f, nukeCD_ - dt);

  tickScheduler(dt);   // wave countdown, Viking, its missiles, nuke
  if (phase_ != Phase::Playing) return;   // an event may have ended the game
  if (benchFire_ && shootCD_ <= 0) {   // test load: a player clicking as fast as the game allows
    EnemyP t;
    for (auto& e : allEnemies()) if (!e->dead && (!t || glm::length(e->pos) < glm::length(t->pos))) t = e;
    if (t) { ammo_ = maxAmmo_; shoot(t->pos.x, t->pos.z); }
  }

  // beams fade
  for (size_t i = 0; i < beams_.size();) {
    Beam& b = beams_[i];
    b.life -= dt;
    if (b.kind == 1) {   // shockwave ring expands outward
      float prog = 1 - b.life / b.startLife;
      b.prop->m = trs(b.pos, {-PI / 2, 0, 0}, glm::vec3(std::max(0.2f, prog * 18)));
      b.prop->mat.opacity = std::max(0.f, b.life / b.startLife * 0.7f);
    } else b.prop->mat.opacity = std::max(0.f, b.life / .5f * .8f);
    if (b.life <= 0) beams_.erase(beams_.begin() + i);
    else i++;
  }

  // spawn
  if (wavePhase_ == WavePhase::Combat && !spawnQueue_.empty()) {
    spawnTimer_ -= dt;
    if (spawnTimer_ <= 0) {
      char type = spawnQueue_.back();
      spawnQueue_.pop_back();
      float r = 13 + rnd() * 2, a = rnd() * PI * 2;
      if (type == 'z') zerglings_.push_back(makeZergling(std::cos(a) * r, std::sin(a) * r));
      else roaches_.push_back(makeRoach(std::cos(a) * r, std::sin(a) * r));
      spawnTimer_ = type == 'r' ? spawnInterval_ * 1.6f : spawnInterval_;
    }
  }

  // enemies
  float arm = 1 / (1 + armorLvl_ * .35f);
  float nightFogFar = dnFactor_ > 0.1f ? fogFar_ : INFINITY;   // beyond it enemies are invisible (also while a night lifts after switching it off)
  for (auto& ep : allEnemies()) {
    Enemy& e = *ep;
    if (e.dead) continue;
    e.leg += dt * (e.type == 'r' ? 5 : 9);
    if (e.flash > 0) e.flash -= dt;
    float ex = e.pos.x, ez = e.pos.z, d = std::sqrt(ex * ex + ez * ez);
    e.visible = nightFogFar < INFINITY ? d < nightFogFar * 1.05f : true;
    if (d < attackReach(e)) {
      if (e.type == 'r') roachAttack(e, dt, ex, ez);   // ranged: acid, damage on impact
      else {                                          // melee: bites the hull
        bunkerHp_ = std::max(0.f, bunkerHp_ - e.dmg * dt * arm);
        bool bite;
        if (!e.attack.empty()) {   // SC2 Attack animation on a loop; bites as the jaws close
          float t0 = e.atkT;
          e.atkT += dt * ANIM_SPEED / zergling_bite::anim;
          bite = t0 < zergling_bite::bite && e.atkT >= zergling_bite::bite;
          e.atkT = std::fmod(e.atkT, 1.f);
          e.pool = e.attack[std::min((int)e.attack.size() - 1, (int)std::floor(e.atkT * e.attack.size()))];
          e.rotY = std::atan2(-ex, -ez);
          e.pos.y = 0;
        } else {
          if (!e.biteSet) { e.biteT = rnd() * .4f; e.biteSet = true; }
          e.biteT -= dt;
          bite = e.biteT <= 0;
          if (bite) e.biteT = .45f + rnd() * .35f;
          if (!e.walk.empty()) walkStep(e, dt * .5f);   // claws at the hull: tread on the spot
          else e.pos.y = std::abs(std::sin(e.leg)) * .07f;
        }
        if (bite) {   // sparks where it bites the hull
          float k = 1.45f / std::max(.01f, d);
          spawnParts({ex * k, .45f + rnd() * .4f, ez * k}, 0xffcc66, 3);
          audio::play("bite");
        }
        if (dmgFlashCD_ <= 0) { flash(1); dmgFlashCD_ = 0.6f; }
        if (bunkerHp_ <= 0) gameOver();
      }
    } else {
      float slowF = e.slowed ? e.slowFactor : 1;
      float move = e.speed * slowF * 60 * dt;
      e.pos.x -= ex / d * move;
      e.pos.z -= ez / d * move;
      // SC2 walk cycle paced by the distance covered; procedural blocks hop; a model without one bobs
      if (!e.walk.empty()) walkStep(e, move);
      else {
        e.pos.y = std::abs(std::sin(e.leg)) * (e.model ? .035f : e.type == 'r' ? .1f : .13f);
        if (e.model) e.rotZ = std::sin(e.leg) * .05f;
      }
      e.rotY = std::atan2(-ex, -ez);
    }
  }
  auto isDead = [](const EnemyP& e) { return e->dead; };
  zerglings_.erase(std::remove_if(zerglings_.begin(), zerglings_.end(), isDead), zerglings_.end());
  roaches_.erase(std::remove_if(roaches_.begin(), roaches_.end(), isDead), roaches_.end());
  updateAcids(dt);   // acid in flight keeps going if its Roach died
  if (bunkerHp_ <= 0) { gameOver(); return; }

  // bullets
  float baseDmg = 10 * (1 + dmgLvl_ * .6f) * (1 + cfg::endless_dmg * endlessWaves()) * (1 + cfg::armory_dmg * armory_[1]);
  for (auto& e : allEnemies()) if (e->slowed && gameT_ > e->slowUntil) e->slowed = false;   // slows expire in game time
  float fs = dt * 60;   // movement tuned at 60 fps
  std::vector<EnemyP> enemyList = allEnemies();
  bool missiles = models::isReady("marauderMissile");
  // Swept hit test along this frame's path: fast rounds must not skip through
  // zerglings. The first live enemy along the round's path is hit.
  auto sweep = [&](const Bullet& b, float& tBest) -> int {
    float px = b.pos.x, pz = b.pos.z, sx = b.vx * fs, sz = b.vz * fs, sLen2 = sx * sx + sz * sz;
    if (sLen2 == 0) sLen2 = 1;
    int best = -1;
    tBest = 2;
    for (int k = 0; k < (int)enemyList.size(); k++) {
      const Enemy& c = *enemyList[k];
      if (c.dead) continue;
      float ex = c.pos.x - px, ez = c.pos.z - pz;
      float t = glm::clamp((ex * sx + ez * sz) / sLen2, 0.f, 1.f);   // closest point on the segment
      float dx = ex - sx * t, dz = ez - sz * t, hitR = c.type == 'r' ? .75f : .5f;
      if (dx * dx + dz * dz < hitR * hitR && t < tBest) { tBest = t; best = k; }   // first one along the path
    }
    return best;
  };
  for (Bullet& b : bullets_) {
    if (!b.alive) continue;
    float tBest;
    int ci = sweep(b, tBest);
    float px = b.pos.x, pz = b.pos.z;
    float sx = b.vx * fs, sz = b.vz * fs;
    b.pos.x += sx;
    b.pos.z += sz;
    b.dist += b.speed * fs;
    if (b.vy) {
      b.pos.y += b.vy * fs;
      if (b.pos.y < 0.12f) { b.pos.y = 0.12f; b.vy = 0; orientBullet(b); }   // past the aim point: skim the ground
    }
    if (b.unit == Unit::Marauder && missiles) {   // punisher grenade: fire trail from its tail
      float k = .35f / std::max(1e-6f, std::sqrt(b.vx * b.vx + b.vy * b.vy + b.vz * b.vz));
      glm::vec3 p = b.pos;
      emit(glow_, p.x - b.vx * k, p.y - b.vy * k, p.z - b.vz * k, (rnd() - .5f) * .4f, rnd() * .3f, (rnd() - .5f) * .4f, .16f, .28f, .08f, .95f, 0xffa040, 0xc02000, 0, 1, 0);
      emit(smoke_, p.x - b.vx * k * 1.5f, p.y - b.vy * k * 1.5f, p.z - b.vz * k * 1.5f, (rnd() - .5f) * .3f, .35f, (rnd() - .5f) * .3f, .55f, .16f, .5f, .5f, 0x9a9488, 0x55524c, -.1f, .6f, 1);
    }
    EnemyP hitE = ci >= 0 ? enemyList[ci] : nullptr;
    if (hitE) {
      Enemy& e = *hitE;
      b.pos = {px + sx * tBest, b.pos.y, pz + sz * tBest};
      const UnitDef& def = unitDef(b.unit);
      bool hasBonus = (def.bonusVsArmored && e.armored) || (def.bonusVsLight && e.light);
      float bonus = hasBonus ? def.bonusMult : 1.0f;
      float dmg = baseDmg * def.dmgMult * bonus;
      e.hp -= dmg;
      e.flash = 0.08f;
      audio::play("hit");
      if (floatTexts_.size() >= 60) floatTexts_.erase(floatTexts_.begin());   // late waves: ~60 numbers/s
      floatTexts_.push_back({{e.pos.x, e.pos.y + 1.2f, e.pos.z}, std::to_string((int)std::round(dmg)), 0.9f, 0.9f, bonus > 1 ? 0xff6600u : 0xffffffu});
      if (b.unit == Unit::Marauder && concussiveUnlocked_) {   // Concussive Shells: slow (AOE from level 1)
        float until = gameT_ + 2.5f;
        e.slowed = true; e.slowUntil = until; e.slowFactor = 0.4f;
        float aoeR = CONC_RADIUS[concussiveLvl_];
        if (aoeR > 0) {
          for (auto& o : enemyList) {
            if (o->dead || o == hitE) continue;
            float dx = o->pos.x - e.pos.x, dz = o->pos.z - e.pos.z;
            if (dx * dx + dz * dz <= aoeR * aoeR) { o->slowed = true; o->slowUntil = until; o->slowFactor = 0.4f; }
          }
          spawnParts(e.pos, 0x4488ff, 10);
        }
      }
      spawnParts(b.pos, def.bulletColor, 4);
      b.alive = false;
      if (e.hp <= 0) killEnemy(hitE, true);
    }
    if (b.alive && b.dist > 18) b.alive = false;
  }
  bullets_.erase(std::remove_if(bullets_.begin(), bullets_.end(), [](const Bullet& b) { return !b.alive; }), bullets_.end());

  glow_.update(dt);
  smoke_.update(dt);

  float rr = repairRate();   // Lv3 self-repair + Super Stimpack, stacked
  if (rr > 0 && bunkerHp_ < maxHp_) bunkerHp_ = std::min(maxHp_, bunkerHp_ + rr * dt);
  updateBunkerFire(dt);

  // wave clear (acid still in the air lands first: its damage belongs to this wave)
  if (wavePhase_ == WavePhase::Combat && spawnQueue_.empty() && zerglings_.empty() && roaches_.empty() && acids_.empty()) {
    wavePhase_ = WavePhase::Clearing;
    waveCleared();
  }
  updateMarines(dt);
  updateViking(dt);
  updateVikingMissiles(dt);
  checkBunkerHover(dt);
  ghostSnipeTick(dt);
  updateShrike(dt);
  updateCorpses(dt);
  updateFlare(dt);      // before the lights: they follow its strength (and the nuke light's)
  if (nukeLightT_ >= 0 && (nukeLightT_ += dt) >= NUKE_LIGHT_DUR) nukeLightT_ = -1;
  updateDayNight(dt);
  updateMushrooms(dt);
  tutorialTick();
  for (FloatText& f : floatTexts_) { f.life -= dt; f.p.y += dt * 0.8f; }
  floatTexts_.erase(std::remove_if(floatTexts_.begin(), floatTexts_.end(), [](const FloatText& f) { return f.life <= 0; }), floatTexts_.end());
}

void Game::updateMushrooms(float dt) {
  for (Mushroom& mc : mushrooms_) mc.t += dt;
  mushrooms_.erase(std::remove_if(mushrooms_.begin(), mushrooms_.end(), [](const Mushroom& mc) { return mc.t >= MUSH_DUR; }), mushrooms_.end());
}

// ─── Render ──────────────────────────────────────────────────────────────────
// A fixed pseudo-random value in [0, 1) per puff (the cloud keeps no per-puff state)
static float puffHash(int i, int salt) {
  float s = std::sin(i * 12.9898f + salt * 78.233f) * 43758.5453f;
  return s - std::floor(s);
}

// Fire by temperature: white-hot → yellow → orange → deep red → soot
static glm::vec3 fireRamp(float T) {
  static const glm::vec3 c[5] = {hexColor(0x2e2420), hexColor(0x8a2a0c), hexColor(0xf06a18), hexColor(0xffb43a), hexColor(0xfff0c0)};
  T = glm::clamp(T, 0.f, 1.f) * 4;
  int i = std::min(3, (int)T);
  return lerp3(c[i], c[i + 1], T - i);
}

// The mushroom cloud at its age, as soft sprites, as StarCraft II draws it: a cap of fire
// (a rolling torus under a dome) on a fiery stem, burning white-yellow inside and orange
// at the edges, cooling to dark red and soot; a grey dust ring surging out along the
// ground around it. `lit` dims what has cooled to smoke with the scene's light (at night
// it goes dark once the fireball fades); the fire is self-lit.
static void mushroomSprites(const Mushroom& mc, float lit, std::vector<ParticleSystem::P>& smoke, std::vector<ParticleSystem::P>& fire) {
  const float t = mc.t, g = 1 - std::exp(-t / 0.9f), spread = 1 - std::exp(-t / 1.0f);
  const float heat = std::exp(-t / 2.8f);   // the cloud's temperature, 1 → 0
  const float capH = 1.4f + 6.0f * g + 0.2f * t, capR = 0.9f + 2.7f * spread, tube = 0.7f + 0.9f * g, puff = 1.3f + 1.6f * g;
  const float fade = std::min(1.f, t / 0.15f) * (1 - glm::smoothstep(MUSH_DUR - 2.5f, MUSH_DUR, t));
  int n = 0;
  // inner: how hot this puff burns for the cloud's temperature (1 = its core)
  auto add = [&](glm::vec3 p, float size, float a, float inner) {
    int i = n++;
    float T = heat * inner * (0.9f + 0.2f * puffHash(i, 1));
    ParticleSystem::P q{};
    q.pos = mc.p + p;
    q.size = size * (0.85f + 0.3f * puffHash(i, 2));
    q.col = fireRamp(T);
    if (T < 0.3f) q.col *= lit + (1 - lit) * T / 0.3f;   // cooled to smoke: lit by the scene
    q.alpha = a * fade * (0.6f + 0.4f * std::min(1.f, T * 2));
    smoke.push_back(q);
    if (T < 0.45f) return;   // the hottest parts glow
    q.size *= 0.6f;
    q.col = fireRamp(std::min(1.f, T + 0.2f));
    q.alpha = 0.5f * std::min(1.f, (T - 0.45f) / 0.55f) * fade;
    fire.push_back(q);
  };
  const float PI2 = 2 * PI_F, stemTop = std::max(0.5f, capH - tube * 0.6f);
  for (int i = 0; i < 22; i++) {   // stem: a column of fire, wider at the foot, twisting slowly
    float f = i / 21.f, a = puffHash(i, 3) * PI2 + t * 0.7f, r = (0.12f + 0.25f * (1 - f)) * puffHash(i, 4);
    add({std::cos(a) * r, 0.3f + f * (stemTop - 0.3f), std::sin(a) * r}, (1.2f + 0.9f * (1 - f)) * (0.6f + 0.4f * g), 0.6f, 1.05f - 0.15f * f);
  }
  for (int k = 0; k < 90; k++) {   // cap: the rolling torus (golden-angle offsets spread the puffs over its tube)
    float a = PI2 * k / 90 + puffHash(k, 5) * 0.1f, phi = -t * 1.4f + k * 2.39996f, s = std::sin(phi), cph = std::cos(phi);
    float rr = capR + tube * cph;
    add({std::cos(a) * rr, capH + tube * 0.8f * s, std::sin(a) * rr}, puff * 0.85f, 0.55f, 0.85f + 0.2f * s - 0.1f * cph);   // the outer rim cools first
  }
  for (int k = 0; k < 18; k++) {   // dome and core of the cap: the hottest
    float a = PI2 * puffHash(k, 6), r = capR * 0.7f * std::sqrt(puffHash(k, 7));
    add({std::cos(a) * r, capH + tube * (0.2f + 0.6f * puffHash(k, 8)), std::sin(a) * r}, puff * 1.1f, 0.55f, 1.1f - 0.3f * r / std::max(capR, 0.1f));
  }
  // the dust ring: grey smoke rolling out along the ground, thickest at its rim, settling
  float ringR = 1.8f + 5.0f * (1 - std::exp(-t / 0.8f)), settle = 1 - glm::smoothstep(3.f, MUSH_DUR, t);
  for (int k = 0; k < 56; k++) {
    float a = PI2 * k / 56 + puffHash(k, 9) * 0.1f, r = ringR + (puffHash(k, 10) - 0.5f) * 0.9f;
    ParticleSystem::P q{};
    q.pos = mc.p + glm::vec3(std::cos(a) * r, 0.35f + 0.3f * puffHash(k, 11), std::sin(a) * r);
    q.size = (1.5f + 1.0f * g) * (0.8f + 0.4f * puffHash(k, 12));
    float shade = 0.75f + 0.25f * puffHash(k, 13);
    q.col = lerp3(hexColor(0x6a6460), hexColor(0x9a948c), shade) * lit;
    q.alpha = 0.42f * settle * std::min(1.f, t / 0.2f);
    smoke.push_back(q);
  }
  if (heat > 0.35f) {   // the white flash at the heart of the young fireball
    ParticleSystem::P q{};
    q.pos = mc.p + glm::vec3(0, capH * 0.8f, 0);
    q.size = capR * 2.6f;
    q.col = hexColor(0xfff0c0);
    q.alpha = 0.55f * (heat - 0.35f) / 0.65f;
    fire.push_back(q);
  }
}

static void writeBulletMatrix(glm::mat4& m, const Bullet& b, float L) {
  float sy = b.sinY, cy = b.cosY, sp = b.sinP, cp = b.cosP;
  m[0] = {cy, 0, -sy, 0};
  m[1] = {sy * sp, cp, cy * sp, 0};
  m[2] = {sy * cp * L, -sp * L, cy * cp * L, 0};
  m[3] = {b.pos.x, b.pos.y, b.pos.z, 1};
}

void Game::render() {
  R.begin();
  R.draw(groundMesh_, &groundMat_, glm::mat4(1), false, true);
  R.draw(dirtMesh_, &dirtMat_, glm::mat4(1), false, false);
  R.draw(rockMesh_, &rockMat_, glm::mat4(1), false, false);

  // bunker (+ Neosteel frame from Lv2, Shrike turret from Lv3)
  if (bunkerModel_) {
    glm::mat4 w = bunkerModel_->wrapper();
    for (const ModelPart& p : bunkerParts_) {
      if (p.name == "Mesh.002" && hpLvl_ < 2) continue;
      R.draw(p.mesh, &p.mat, w, true, true);
    }
    if (shrikeMounted_ && hpLvl_ >= 3) {
      glm::vec3 dp = shrikeModel_->defaultPos;
      glm::mat4 gm = trs({0, shrikeY_, 0}, {0, shrikeYaw_, 0}) * trs({dp.x, dp.y, -0.08f * shrikeRecoil_}, {0, shrikeModel_->defaultRotY, 0});
      for (const ModelPart& p : shrikeParts_) R.draw(p.mesh, &p.mat, gm, true, true);
    }
  } else {
    for (const BakedPart& p : bunkerProc_) R.draw(p.mesh, p.mat, glm::mat4(1), p.cast, true);
  }

  // enemies + corpses: one instanced draw per pool part
  for (auto& [k, pool] : pools_) pool->inst.clear();
  auto addInst = [&](Pool* pool, const glm::vec3& pos, float rotY, float rotZ, const glm::vec3& col) {
    pool->inst.push_back({trs(pos, {0, rotY, rotZ}) * pool->local, col});
  };
  for (auto* list : {&zerglings_, &roaches_})
    for (auto& e : *list) {
      if (e->dead || !e->visible || !e->pool) continue;
      glm::vec3 c(1);
      if (e->flash > 0) c = glm::vec3(2.6f);             // hit flash
      else if (e->slowed) c = {0.6f, 0.8f, 1.5f};        // concussive slow: icy blue
      addInst(e->pool, e->pos, e->rotY, e->rotZ, c);
    }
  for (const Corpse& c : corpses_) addInst(c.pool, c.pos, c.rotY, c.rotZ, glm::vec3(std::max(0.25f, 1 - c.t * 1.4f)));
  for (auto& [k, pool] : pools_)
    if (!pool->inst.empty())
      for (const BakedPart& p : pool->parts) R.drawInstanced(p.mesh, p.mat, pool->inst.data(), (int)pool->inst.size(), p.cast, false);

  // units out of the bunker
  for (const UnitFig& m : marines_) {
    if (!m.visible) continue;
    glm::mat4 g = trs(m.pos, {0, m.rotY, 0});
    if (m.modelP) {
      glm::mat4 w = g * m.modelP->wrapper();
      if (!m.modelP->walk.empty()) R.draw(m.modelP->walk[m.frame], &m.modelP->parts[0].mat, w, true, true);
      else for (const ModelPart& p : m.modelP->parts) R.draw(p.mesh, &p.mat, w, true, true);
    } else for (const BakedPart& p : marineProc_[m.idx % 6]) R.draw(p.mesh, p.mat, g, p.cast, true);
  }

  for (auto& c : crates_) R.draw(c->mesh, &c->mat, c->m, false, false);

  // Viking (its clone's rotation replaces the template's: y = π/2, banked)
  if (vikingActive_) {
    glm::mat4 vm = vikingMatrix();
    if (const Model* vk = models::get("viking"))
      for (const ModelPart& p : vk->parts) R.draw(p.mesh, &p.mat, vm, true, true);
    else for (const BakedPart& p : vikingProc_) R.draw(p.mesh, p.mat, vm, p.cast, true);
  }

  // transient props (nuke visuals, the flare) and fading beams
  for (auto& p : props_) {
    if (!p->visible) continue;
    if (p->model) for (const ModelPart& mp : p->model->parts) R.draw(mp.mesh, &mp.mat, p->m, p->cast, true);
    else R.draw(p->mesh, &p->mat, p->m, p->cast, false, glm::vec3(1), p->order);
  }
  for (auto& b : beams_) R.draw(b.prop->mesh, &b.prop->mat, b.prop->m, false, false);

  // bullets: one tracer batch, punisher grenades (and the Viking's missiles) as their own batch.
  // Tracers ignore the fog; at night they run white-hot in a soft halo over a faint pool of
  // light on the ground beneath them (a little halo by day too)
  {
    static std::vector<Instance> tr, mi, halo, pool;
    tr.clear();
    mi.clear();
    halo.clear();
    pool.clear();
    const Model* mm = models::get("marauderMissile");
    const float night = dnFactor_;
    auto addGlow = [&](const glm::mat4& m, uint32_t col) {
      Instance h;
      h.m = m;
      h.c = hexColor(col);
      halo.push_back(h);
      if (night <= 0.01f) return;
      Instance p = h;   // the pool: wider, flat on the ground
      glm::vec3 f(m[2].x, 0, m[2].z);
      float fl = glm::length(f);
      f = fl > 1e-5f ? f / fl : glm::vec3(0, 0, 1);
      p.m = glm::mat4(glm::vec4(-f.z * 2.4f, 0, f.x * 2.4f, 0), glm::vec4(0, 1, 0, 0), glm::vec4(f * 1.6f, 0), glm::vec4(m[3].x, 0.03f, m[3].z, 1));
      pool.push_back(p);
    };
    for (const Bullet& b : bullets_) {
      if (!b.alive) continue;
      Instance in;
      writeBulletMatrix(in.m, b, 1);
      addGlow(in.m, b.color);
      if (mm && b.unit == Unit::Marauder) { mi.push_back(in); continue; }
      writeBulletMatrix(in.m, b, b.len);
      in.c = lerp3(hexColor(b.color), glm::vec3(1), 0.35f * night);
      tr.push_back(in);
    }
    // Viking missiles, on their own axes (z along the flight), with the rocket's exhaust flame
    // from the nozzle: a white-hot core in an orange glow, both tapering away, flickering
    static std::vector<Instance> vm, flameCore, flameGlow, streakCore, streakGlow;
    vm.clear();
    flameCore.clear();
    flameGlow.clear();
    streakCore.clear();
    streakGlow.clear();
    int vi = 0;
    for (const VMissile& v : vMissiles_) {
      glm::vec3 side = std::abs(v.dir.y) < 0.99f ? glm::normalize(glm::cross(glm::vec3(0, 1, 0), v.dir)) : glm::vec3(1, 0, 0);
      glm::vec3 up = glm::cross(v.dir, side);
      Instance in;
      in.m = glm::mat4(glm::vec4(side * VM_LEN, 0), glm::vec4(up * VM_LEN, 0), glm::vec4(v.dir * VM_LEN, 0), glm::vec4(v.p, 1));
      vm.push_back(in);
      glm::vec3 nozzle = v.p - v.dir * VM_TAIL;
      float flick = 0.85f + 0.15f * std::sin(gameT_ * 70 + vi++ * 1.7f);
      auto flame = [&](float len, float w) {   // trailMesh_: unit height on y, wide end (r 1) at y = -0.5
        Instance f;
        f.m = glm::mat4(glm::vec4(side * w, 0), glm::vec4(-v.dir * len, 0), glm::vec4(up * w, 0), glm::vec4(nozzle - v.dir * (len / 2), 1));
        return f;
      };
      flameGlow.push_back(flame(0.45f * flick, 0.05f));
      flameCore.push_back(flame(0.25f * flick, 0.02f));
      // the light streak along its recent path: thin cylinders, dimming and narrowing with age
      for (size_t k = 0; k + 1 < v.trail.size(); k++) {
        glm::vec3 a(v.trail[k]), b(v.trail[k + 1]), d = b - a;
        float L = glm::length(d);
        if (L < 1e-4f) continue;
        float age = gameT_ - v.trail[k + 1].w, kk = glm::clamp(1 - age / VM_STREAK, 0.f, 1.f);
        glm::vec3 s = std::abs(d.y) < 0.99f * L ? glm::normalize(glm::cross(d, glm::vec3(0, 1, 0))) : glm::vec3(1, 0, 0);
        glm::vec3 u = glm::normalize(glm::cross(s, d));
        auto seg = [&](float w, uint32_t col) {
          Instance f;
          f.m = glm::mat4(glm::vec4(s * w, 0), glm::vec4(d, 0), glm::vec4(u * w, 0), glm::vec4((a + b) * 0.5f, 1));
          f.c = hexColor(col) * kk;
          return f;
        };
        streakGlow.push_back(seg(0.04f * (0.4f + 0.6f * kk), 0xff8a3a));
        streakCore.push_back(seg(0.014f, 0xfff0c0));
      }
    }
    R.drawInstanced(vmMesh_, &vmBodyMat_, vm.data(), (int)vm.size(), true, false);
    R.drawInstanced(trailMesh_, &vmFlameGlowMat_, flameGlow.data(), (int)flameGlow.size(), false, false, 2);
    R.drawInstanced(trailMesh_, &vmFlameCoreMat_, flameCore.data(), (int)flameCore.size(), false, false, 3);
    R.drawInstanced(streakMesh_, &vmFlameGlowMat_, streakGlow.data(), (int)streakGlow.size(), false, false, 2);
    R.drawInstanced(streakMesh_, &vmFlameCoreMat_, streakCore.data(), (int)streakCore.size(), false, false, 3);
    tracerGlowMat_.opacity = 0.25f + 0.65f * night;
    tracerPoolMat_.opacity = 0.3f * night;
    R.drawInstanced(glowQuadMesh_, &tracerPoolMat_, pool.data(), (int)pool.size(), false, false, 1);
    R.drawInstanced(glowQuadMesh_, &tracerGlowMat_, halo.data(), (int)halo.size(), false, false, 2);
    R.drawInstanced(bulletMesh_, &bulletMat_, tr.data(), (int)tr.size(), false, false);
    if (mm && !mi.empty())
      for (const ModelPart& p : mm->parts) {
        bool glowPart = p.name == "region_1";
        R.drawInstanced(p.mesh, glowPart ? &missileGlowMat_ : &missileBodyMat_, mi.data(), (int)mi.size(), !glowPart, false);
      }
  }
  // acid globs (wobbling)
  {
    static std::vector<Instance> ai;
    ai.clear();
    int n = 0;
    for (const Acid& a : acids_) {
      float w = 1 + std::sin(a.t * 40 + n++) * .15f;
      Instance in;
      in.m = glm::scale(glm::translate(glm::mat4(1), a.p), {w, 2 - w, w});
      ai.push_back(in);
    }
    R.drawInstanced(acidMesh_, &acidMat_, ai.data(), (int)ai.size(), false, false);
  }
  // nuke mushroom clouds: smoke sprites back to front, the fire glowing over them
  mushSmoke_.p.clear();
  mushFire_.p.clear();
  for (const Mushroom& mc : mushrooms_) mushroomSprites(mc, 1 - 0.75f * dnLightF_, mushSmoke_.p, mushFire_.p);
  if (!mushSmoke_.p.empty()) {
    auto viewZ = [this](const glm::vec3& p) { return view_[0][2] * p.x + view_[1][2] * p.y + view_[2][2] * p.z; };
    std::sort(mushSmoke_.p.begin(), mushSmoke_.p.end(), [&](const ParticleSystem::P& a, const ParticleSystem::P& b) { return viewZ(a.pos) < viewZ(b.pos); });
    R.drawParticles(&mushSmoke_, Blend::Normal, 2);
    R.drawParticles(&mushFire_, Blend::Additive, 3);
  }
  // Sensor Tower at night: its range ring on the ground, and a red "!" disc lying on the ground over
  // each Zerg out in the dark within it, upright as the camera sees it (its top away from the camera)
  if (phase_ == Phase::Playing && sensorLvl_ >= 1) {
    if (dnFactor_ > 0.1f) {
      sensorRingMat_.opacity = 0.75f * dnFactor_;
      R.draw(sensorRingMesh_[sensorLvl_], &sensorRingMat_, glm::translate(glm::mat4(1), {0, 0.03f, 0}), false, false, glm::vec3(1), 1);
    }
    static std::vector<Instance> bi;
    bi.clear();
    float pulse = 0.65f + 0.35f * std::sin(uiTime_ * 2 * PI * 1.5f);
    // the screen's up direction on the ground: the camera's up and forward axes, flattened (no roll)
    glm::vec2 f(view_[0][1] - view_[0][2], view_[2][1] - view_[2][2]);
    f = glm::length(f) > 1e-4f ? glm::normalize(f) : glm::vec2(0, -1);
    const glm::vec3 F(f.x, 0, f.y), right = glm::cross(F, glm::vec3(0, 1, 0));
    for (auto* list : {&zerglings_, &roaches_})
      for (auto& e : *list) {
        if (!sensed(*e)) continue;
        const float s = 1.4f * (0.94f + 0.06f * pulse);
        Instance in;   // the glyph's top → F (the quad turned half a turn: its +z), its x → the screen's right
        in.m = glm::mat4(glm::vec4(-right * s, 0), glm::vec4(0, 1, 0, 0), glm::vec4(F * s, 0), glm::vec4(e->pos.x, 0.05f, e->pos.z, 1));
        in.c = glm::vec3(pulse);
        bi.push_back(in);
      }
    R.drawInstanced(blipMesh_, &blipMat_, bi.data(), (int)bi.size(), false, false, 2);
  }
  R.drawParticles(&smoke_, Blend::Normal, 2);   // glow over smoke
  R.drawParticles(&glow_, Blend::Additive, 3);

  SceneParams sp;
  sp.view = view_;
  sp.proj = proj_;
  sp.camPos = camPos_;
  sp.ambientColor = ambCol_;
  sp.ambientIntensity = ambI_;
  sp.sun = {{10, 28, 8}, sunCol_, sunI_};
  sp.fill = {{-8, 10, -6}, hexColor(0x4488cc), fillI_};
  sp.points[0] = {{0, 2.5f, 0}, hexColor(0xff9944), bunkerLightI_, 14, 2};   // night glow
  sp.points[1] = {muzzlePos_, hexColor(0xffb060), muzzleI_, 4.5f, 2};        // muzzle flash on the hull
  sp.points[2] = {firePos_, hexColor(0xff6a1a), fireI_, 6, 2};              // the bunker burns
  sp.points[3] = {nukeLightPos_, hexColor(0xffa050), nukeLightK() * (1 + 3 * dnFactor_), 30, 1.5f};   // a nuke's fireball (strongest at night)
  sp.fogColor = fogCol_;
  sp.fogNear = fogNear_;
  sp.fogFar = fogFar_;
  sp.shadows = gfxHigh_;
  sp.msaa = gfxHigh_;
  float sx = (float)fbW_ / std::max(1, winW_), sy = (float)fbH_ / std::max(1, winH_);
  sp.vpX = (int)std::round(canvasX_ * sx);
  sp.vpW = std::max(1, (int)std::round(canvasW_ * sx));
  sp.vpH = std::max(1, (int)std::round(canvasH_ * sy));
  sp.vpY = fbH_ - (int)std::round((canvasY_ + canvasH_) * sy);
  R.render(sp);
}

// ─── Frame ───────────────────────────────────────────────────────────────────
void Game::layout() {
  glfwGetWindowSize(win_, &winW_, &winH_);
  glfwGetFramebufferSize(win_, &fbW_, &fbH_);
  float topBar = 32 * uiScale_, console = 148 * uiScale_;
  canvasX_ = 0;
  canvasY_ = topBar;
  canvasW_ = (float)std::max(1, winW_);
  canvasH_ = std::max(1.f, winH_ - topBar - console);   // a very short window must not give a negative viewport
  double cx, cy;
  glfwGetCursorPos(win_, &cx, &cy);
  mx_ = (cx - canvasX_) / canvasW_;
  my_ = (cy - canvasY_) / canvasH_;
}

void Game::frame(float dt) {
  double t0 = glfwGetTime();
  layout();
  update(dt);
  double t1 = glfwGetTime();
  gpuFx_.update(phase_ == Phase::Playing && !isPaused() ? dt : 0);   // spawn this frame's bursts, move the GPU particles
  render();
  double t2 = glfwGetTime();
  drawUI(dt);
  double t3 = glfwGetTime();
  // performance overlay: CPU ms per stage and the frame interval, over ~0.5 s
  perfAcc_.update += t1 - t0;
  perfAcc_.render += t2 - t1;
  perfAcc_.ui += t3 - t2;
  perfAcc_.frame += dt;
  perfAcc_.worst = std::max(perfAcc_.worst, (double)dt);
  perfAcc_.frames++;
  if (phase_ == Phase::Playing) {   // the whole game since start(), for the exit log
    perfRun_.update += t1 - t0; perfRun_.render += t2 - t1; perfRun_.ui += t3 - t2; perfRun_.frame += dt;
    perfRun_.worst = std::max(perfRun_.worst, (double)dt);
    perfRun_.frames++;
  }
  if (t3 - perfT_ >= 0.5) {
    perfShown_ = perfAcc_;
    perfAcc_ = Perf();
    perfT_ = t3;
  }
}

// ─── Start / game over ───────────────────────────────────────────────────────
void Game::clearEntities() {
  clearScheduler();   // drop pending wave countdown / Viking / missile launches / nuke (runs their cleanup)
  removeViking();
  crateCbs_.clear();   // the crate on its way went with the Viking / marines; a stale wait would stall the next game
  waveCrates_ = 0;
  holding_ = false;
  nukeInFlight_ = false;
  glow_.clear();
  gpuFx_.clear();
  smoke_.clear();
  memset(fireAccum_, 0, sizeof fireAccum_);
  shrikeTgt_.reset();
  shrikeVel_ = shrikeRecoil_ = shrikeCD_ = 0;
  stimT_ = 0;
  stimVolley_ = 0;
  shrikeOD_ = false;
  for (auto& e : allEnemies()) e->dead = true;
  zerglings_.clear();
  roaches_.clear();
  bullets_.clear();
  beams_.clear();
  crates_.clear();
  crateXZ_.clear();
  marines_.clear();
  spawnQueue_.clear();
  corpses_.clear();
  acids_.clear();
  vMissiles_.clear();
  mushrooms_.clear();
  nukeLightT_ = -1;
  endFlare();   // the flare and the nuke light go with the scene: the lights return to the true day / night
  applyDNLights(dnFactor_);
  props_.clear();
}

void Game::start(Mode m) {
  if (m == Mode::Endless && mode_ != Mode::Endless && !endlessUnlocked()) return;   // replaying Endless never needs the flag
  mode_ = m;
  audio::music("battle");
  clearEntities();
  newGameState();
  tutDismiss();   // a box left open (Restart / Load from the menu) goes with the old game
  tutNext_ = 0;
  overlay_ = Overlay::None;
  phase_ = Phase::Playing;
  startWave();
}

void Game::newGameState() {
  score_ = 0; wave_ = 1; kills_ = 0; minerals_ = 150; gas_ = 50;
  ammo_ = maxAmmo_ = cfg::base_ammo;
  bunkerHp_ = maxHp_ = 750;
  dmgLvl_ = armorLvl_ = hpLvl_ = ammoLvl_ = 0;
  for (int& a : armory_) a = 0;
  endlessSteps_ = 0;
  updateBunkerVisual();
  updatePortraitBunker(true);
  holoAngle_ = 0;
  floatTexts_.clear();
  victoryAchieved_ = false;
  resetRoster();
  airCD_ = nukeCD_ = shootCD_ = 0;
  wavePhase_ = WavePhase::Idle;
  gameT_ = 0;
  perfRun_ = Perf();
  dmgFlashCD_ = 0;
}

void Game::gameOver() {
  if (phase_ != Phase::Playing) return;   // several biters can drop the hull to 0 in one frame
  phase_ = Phase::GameOver;
  bool record = false;
  if (mode_ == Mode::Endless) {
    int bw, bs;
    if (!bestEndless(bw, bs) || wave_ > bw || (wave_ == bw && score_ > bs)) {
      storage::set("endless_best_wave", std::to_string(wave_));
      storage::set("endless_best_score", std::to_string(score_));
      record = true;
    }
  }
  showResults(false, false, record);
}

// Campaign won (final wave cleared): unlock Endless, show the victory screen
void Game::campaignVictory() {
  if (victoryAchieved_ || phase_ != Phase::Playing) return;
  victoryAchieved_ = true;
  score_ += 5000;   // victory bonus
  bool newly = !endlessUnlocked();
  storage::set("endless_unlocked", "1");
  // checkpoint = "Continue · Endless": CONTINUE on the title resumes in Endless
  storage::Record ov = {{"gameMode", "endless"}};
  writeSave("autosave_v1", &ov);
  phase_ = Phase::Victory;   // freezes the game behind the results screen
  flash(0);
  audio::play("clear");
  showResults(true, newly, false);
}

void Game::continueEndless() {
  if (phase_ != Phase::Victory) return;
  mode_ = Mode::Endless;
  overlay_ = Overlay::None;
  phase_ = Phase::Playing;
  setStatus("ENDLESS MODE \xE2\x80\x94 the swarm never stops. Resupply incoming...");
  audio::music("battle");
}

void Game::showTitle() {
  audio::music("title");
  clearEntities();
  closeRoster();
  closeMenu();
  closeCheat();
  tutDismiss();
  phase_ = Phase::Menu;
  wavePhase_ = WavePhase::Idle;
  roster_ = {Unit::Marine, Unit::Marine};
  syncMarines();
  overlay_ = Overlay::Title;
}

void Game::showResults(bool victory, bool newly, bool record) {
  audio::music(victory ? "victory" : "defeat");
  resVictory_ = victory;
  resNewly_ = newly;
  resRecord_ = record;
  overlay_ = Overlay::Results;
}

bool Game::endlessUnlocked() const { return storage::get("endless_unlocked") == "1"; }
bool Game::bestEndless(int& wave, int& score) const {
  std::string w = storage::get("endless_best_wave"), s = storage::get("endless_best_score");
  if (w.empty()) return false;
  wave = std::atoi(w.c_str());
  score = std::atoi(s.c_str());
  return true;
}

void Game::shutdown() {
  const Perf& p = perfShown_;
  if (p.frames)
    logInfo("perf (last 0.5 s): %.0f FPS, frame %.2f ms, worst %.1f ms; CPU update %.2f, render %.2f, UI %.2f ms; %d draws, %d particles",
            p.frames / p.frame, p.frame / p.frames * 1000, p.worst * 1000, p.update / p.frames * 1000, p.render / p.frames * 1000,
            p.ui / p.frames * 1000, R.drawCount(), (int)(glow_.p.size() + smoke_.p.size()));
  const Perf& r = perfRun_;
  if (r.frames)
    logInfo("perf (whole game, %d frames): %.0f FPS, frame %.2f ms, worst %.1f ms; CPU update %.2f, render %.2f, UI %.2f ms; GPU particles %s",
            r.frames, r.frames / r.frame, r.frame / r.frames * 1000, r.worst * 1000, r.update / r.frames * 1000, r.render / r.frames * 1000,
            r.ui / r.frames * 1000, gpuFx() ? "on" : "off");
}
