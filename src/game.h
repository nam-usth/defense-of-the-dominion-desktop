// The game — a port of js/game.js: scene, units, waves, combat, effects, HUD,
// menus, saving. game.cpp holds the simulation and the 3D scene, game_ui.cpp
// the HUD and menus (Dear ImGui), game_save.cpp saving and the cheat codes.
#pragma once
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "assets.h"
#include "display.h"
#include "gpu_particles.h"
#include "nuke_fx.h"
#include "render.h"
#include "storage.h"
#include "weather.h"

struct GLFWwindow;
struct ImFont;

enum class Phase { Menu, Playing, GameOver, Victory };
enum class WavePhase { Idle, Combat, Clearing, Cleared };
enum class Mode { Campaign, Endless };
enum class Unit { Marine, Marauder, Ghost };

struct UnitDef {
  int slots;
  int cost[2];
  float dmgMult;
  uint32_t bulletColor;
  float bulletSpeed;
  bool bonusVsArmored, bonusVsLight;
  float bonusMult;
  const char *label, *name, *key;
};
const UnitDef& unitDef(Unit u);

// Meshes drawn as instances of one batch: an SC2 frame, a static model's parts
// or a baked procedural shape. `local` places the template (the model's wrapper).
struct BakedPart {
  Mesh* mesh;
  const Material* mat;
  bool cast;
};
using Baked = std::vector<BakedPart>;
struct Pool {
  std::vector<BakedPart> parts;
  glm::mat4 local{1};
  std::vector<Instance> inst;   // filled every frame
};

struct Enemy {
  char type = 'z';   // 'z' Zergling · 'r' Roach
  glm::vec3 pos{0};
  float rotY = 0, rotZ = 0;
  bool visible = true;
  // look: an SC2 skin's walk / attack frames, else its static model, else procedural
  Pool* pool = nullptr;
  bool model = false;
  std::vector<Pool*> walk, attack;
  float stride = 1, step = 0, atkT = 0;
  glm::vec3 mouth{0};
  float hp = 0, maxHp = 0;
  bool dead = false;
  float speed = 0, dmg = 0, leg = 0, flash = 0;
  bool biteSet = false;
  float biteT = 0;
  bool light = false, armored = false;
  int rScore = 0, rMin = 0, rGas = 0;
  float slowUntil = -1, slowFactor = 1;
  bool slowed = false;
};
using EnemyP = std::shared_ptr<Enemy>;

struct Corpse {
  glm::vec3 pos;
  float rotY, rotZ;
  Pool* pool;
  float t;
  float spin;
  // flung by a nuke's shock front: frozen white in the flash until the front reaches it (delay),
  // then thrown outward, charred, tumbling (spin: rad/s), bouncing once, sinking
  bool flung = false;
  glm::vec3 vel{0};
  float delay = 0, tint = 1;
};
// How killEnemy throws a corpse (a nuke victim)
struct Fling {
  glm::vec3 vel{0};
  float delay = 0, spin = 0;
};

struct Bullet {
  glm::vec3 pos;
  float vx, vy, vz;
  uint32_t color;
  bool alive = true;
  float dist = 0, speed;
  Unit unit;
  float len = 1.8f;   // streak length × the tracer mesh, alike for every round (bunker and Shrike: the same brightness)
  float sinY = 0, cosY = 1, sinP = 0, cosP = 1;
  float range = 18;               // flies this far; a Marauder's grenade bursts there (its target point)
  glm::vec2 aim{0};               // a Marauder grenade's target point (on the ground)
  std::vector<glm::vec4> trail;   // a Marauder grenade's light streak: recent nozzle positions, w = game time
  const Enemy* passed = nullptr;  // in the rain: the Zerg this round missed and flies past (not rolled for again)
};

// In the rain the bunker's rounds (and the Shrike's) miss now and then: in a drizzle, in a downpour
// (between the two as the rain grows). Grenades still burst, snipes and missiles still strike
inline constexpr float RAIN_MISS[2] = {0.05f, 0.10f};

// A Marauder grenade's blast radius: base, then per Concussive Shell AOE tier
inline constexpr float MG_RADIUS[4] = {1.5f, 2.0f, 2.6f, 3.2f};

// A punisher grenade's blast: a soft glow on the ground over its radius (the burst itself: sprites)
struct MBlast {
  glm::vec3 p;
  float r, t = 0;     // radius; seconds since the burst (game time)
  bool blue = false;  // a concussive shell's (icy blue), else a plain grenade's (warm)
};

struct Acid {
  glm::vec3 p, v;
  float t, dmg;
};

// A Viking air-to-ground missile: leaves a wing, runs ahead, then dives on its target
// along a quadratic curve (p0 → p1 → the target), homing while the target lives
struct VMissile {
  glm::vec3 p0, p1, p{0}, dir{1, 0, 0};
  glm::vec2 tgt;
  EnemyP e;
  float t = 0;                    // 0..1 along the flight
  std::vector<glm::vec4> trail;   // its light streak: recent nozzle positions, w = game time
};

// A nuke's blast (nuke_fx.h draws it; the mushroom cloud is sprites): its shock front reaches
// the bunker at tHit (the shock booms)
struct NukeBlast {
  glm::vec3 p;
  float t = 0;   // seconds since the blast (game time)
  float tHit = 0, seed = 0;
  float embers = 0;   // embers due but not yet emitted (a fraction of one)
  bool hitDone = false;
};
// The scar a nuke leaves on the ground: a burn with glowing cracks that cool, then fade
struct Scar {
  glm::vec2 p;
  float age = 0, seed = 0;
};

struct FloatText {
  glm::vec3 p;
  std::string txt;
  float life, maxLife;
  uint32_t color;
};

// A unit that leaves the bunker to fetch a crate (hidden while inside)
struct UnitFig {
  glm::vec3 pos{0};
  float rotY = 0;
  bool visible = false;
  Unit type = Unit::Marine;
  bool typeSet = false, fallback = false;
  bool running = false;
  float tx = 0, tz = 0;
  bool returning = false;
  std::vector<glm::vec2> queue;
  std::function<void()> cb;
  float homeX = 0, homeZ = 0, leg = 0;
  int idx = 0;
  const Model* modelP = nullptr;   // SC2 model (static or walk frames), else procedural
  float stride = 1, step = 0;
  int frame = 0;
};

// A transient scene object (beams, rings, lasers, the falling warhead)
struct Prop {
  Mesh* mesh = nullptr;
  Material mat;
  const Model* model = nullptr;   // draws the model's parts instead
  glm::mat4 m{1};
  bool visible = true, cast = false;
  int order = 0;
};
using PropP = std::shared_ptr<Prop>;

struct Beam {
  PropP prop;
  float life, startLife;
  int kind;   // 0 fade (strike beams, snipe), 1 nuke shockwave
  glm::vec3 pos;
};

class Game {
 public:
  bool init(GLFWwindow* win);
  void frame(float dt);                 // update + render + UI (inside an ImGui frame)
  void onAssetLoaded(const char* key);
  void shutdown();
  // input (from GLFW callbacks)
  void onKey(int key, int action, int mods);
  void onChar(unsigned c);
  void onMouseButton(int button, int action);
  void onScroll(double dy);
  bool wantsMinimizedPause() const { return true; }
  void setUiScale(float s) { uiScale_ = s; }
  // test hooks (command line)
  void autoStart(const std::string& mode) { start(mode == "endless" ? Mode::Endless : Mode::Campaign); }
  void runCheat(const std::string& code) {   // also the test actions hire:<unit>, research:<key>[:<tier>] (paid for: mng first)
    if (code == "autocast:stim") toggleStimAuto();   // and autocast:stim (the STIM button's right-click),
    else if (code.rfind("runner:", 0) == 0) electRunner(std::atoi(code.c_str() + 7));   // runner:<roster index>
    else if (code.rfind("hire:", 0) == 0) {
      for (int k = 0; k < 3; k++) if (code.substr(5) == unitDef((Unit)k).key) hireUnit((Unit)k);
    } else if (code.rfind("research:", 0) == 0) {
      std::string r = code.substr(9);
      size_t c = r.find(':');
      researchUpgrade(r.substr(0, c), c == std::string::npos ? -1 : std::atoi(r.c_str() + c + 1));
    } else submitCheat(code);
  }
  void openPanel(const std::string& p) {
    if (p == "roster") openRoster();
    else if (p.rfind("menu", 0) == 0) {   // "menu" or "menu:<tab>"
      openMenu();
      for (int i = 0; i < MENU_TAB_COUNT; i++) if (p == std::string("menu:") + menuTabKey(i)) menuTab_ = i;
    }
    else if (p == "cheat") openCheat();
    else if (p.rfind("tour", 0) == 0) {   // "tour" or "tour:<page>"
      startTour();
      if (p.size() > 5) tourPage(std::atoi(p.c_str() + 5));   // (clamped to the pages)
    }
    else if (p == "howto") showHowTo();
  }
  void setDayNightTime(float t);                    // test: the cycle on, t seconds into it
  void weatherForRun(const std::string& s);         // test: clear | overcast | rain | drizzle | after (a shower just over), held
  void tutorialForRun(bool on) { tutRun_ = on; }    // test: no tour / hints this run (the stored setting is kept)
  void postForRun(bool on) { postRun_ = on; }       // HDR post-processing (main sets the window's samples to match)
  void testPress(bool down) { onMouseButton(0, down ? 1 : 0); }   // test: the left button (GLFW_MOUSE_BUTTON_LEFT, PRESS / RELEASE)
  int ammo() const { return ammo_; }
  void testKey(int key) { onKey(key, 1, 0); }
  void showPerf(bool on) { perfOverlay_ = on; }
  void benchFire(bool on) { benchFire_ = on; }   // test: fire every volley at the nearest enemy, magazine refilled
  void setGpuInfo(std::vector<display::Gpu> gpus, const std::string& running, bool dedicatedPref);
  bool wantsRelaunch() const { return relaunch_; }   // the graphics card changed: main restarts the game
  void gpuParticlesForRun(bool on) { gpuFxOn_ = on; R.setGpuParticles(gpuFx() ? &gpuFx_ : nullptr); }   // test: not stored
  bool assetsSettled() const;

 private:
  friend struct UiCtx;
  GLFWwindow* win_ = nullptr;
  Renderer R;
  std::mt19937 rng_{std::random_device{}()};
  float rnd() { return std::uniform_real_distribution<float>(0, 1)(rng_); }

  // window layout (pixels): canvas between the top bar and the console
  float uiScale_ = 1;
  int winW_ = 1, winH_ = 1, fbW_ = 1, fbH_ = 1;
  float canvasX_ = 0, canvasY_ = 0, canvasW_ = 1, canvasH_ = 1;   // window coordinates
  double mx_ = 0.5, my_ = 0.5;                                       // cursor in the canvas, 0..1
  void layout();

  // graphics
  bool gfxHigh_ = true;
  // day / night
  bool dnEnabled_ = false;
  float dnTime_ = 0, dnFactor_ = 0;
  float dnLightF_ = 0;   // the factor the lights / fog were last applied at (a flare lifts it below dnFactor_)
  float dnLit() const;   // that factor now: dnFactor_ lifted by a burning Viking flare
  // Viking flare (night airstrikes): lights the field for FLARE_DUR s of game time
  float flareT_ = -1;    // seconds since it lit, -1 = none burning
  glm::vec2 flareDir_{0, -1};   // bunker → far side of the field as the camera saw it at the light-up
  PropP flareCore_, flareGlow_;
  float flareK() const;  // strength 0..1
  void startFlare();
  void updateFlare(float dt);
  void endFlare();
  // weather (weather.h): clear, overcast, now and then rain; the ground wets, puddles fill and dry
  Weather weather_;
  WeatherRenderer weatherFx_;
  bool weatherOn_ = true;               // Menu → Settings (stored)
  glm::vec3 skyZen_{0}, skyHor_{0};     // the sky the puddles mirror (applyDNLights)
  void setWeatherOn(bool on);
  void rainCheat();                     // the "rain" code, in turn: a downpour now, a drizzle, the sky clears
  float rainMiss() const;               // the chance a round misses in the rain now (RAIN_MISS)
  // nuke fireball light: lights the field (and lifts a night) for NUKE_LIGHT_DUR s of game time
  float nukeLightT_ = -1;   // seconds since the blast, -1 = none
  glm::vec3 nukeLightPos_{0};
  float nukeLightK() const;  // strength 0..1
  // camera rig
  float zoom_ = 0, zoomT_ = 0, camYaw_ = 0, camYawT_ = 0;
  glm::mat4 view_{1}, proj_{1};
  glm::vec3 camPos_{0, 22, 8};
  void updateCamera(float dt);
  void zoomView(float d) { zoomT_ = glm::clamp(zoomT_ + d, 0.f, 1.f); }
  void rotateView(int dir);
  void resetView();
  bool project(const glm::vec3& p, float& sx, float& sy) const;   // world → canvas pixels (window coords)
  bool groundPoint(double cx, double cy, glm::vec3& out) const;

  // lights (three.js parameters)
  glm::vec3 ambCol_{0}, sunCol_{0}, fillCol_{0}, fillPos_{-8, 10, -6};   // fill: the day's sky fill, the moon at night
  float ambI_ = 0.7f, sunI_ = 1.8f, fillI_ = 0.5f, bunkerLightI_ = 0, muzzleI_ = 0, fireI_ = 0;
  glm::vec3 muzzlePos_{0}, firePos_{0, 1.8f, 0};
  glm::vec3 fogCol_{0};
  float fogNear_ = 45, fogFar_ = 90;
  void applyDNLights(float f);
  void updateDayNight(float dt);
  void toggleDayNight();
  std::string dayNightLabel() const;
  std::string dayNightNext() const;   // countdown to the next phase, e.g. "DUSK IN 0:42"

  // scheduler (game time): one-shots and per-frame tickers
  struct Timer { float t; std::function<void()> fn; };
  struct Ticker { std::function<bool(float)> fn; std::function<void()> clean; };
  std::vector<Timer> timers_;
  std::vector<Ticker> tickers_;
  void after(float sec, std::function<void()> fn) { timers_.push_back({sec, std::move(fn)}); }
  void addTicker(std::function<bool(float)> fn, std::function<void()> clean = nullptr) { tickers_.push_back({std::move(fn), std::move(clean)}); }
  void tickScheduler(float dt);
  void clearScheduler();

  // state
  Phase phase_ = Phase::Menu;
  Mode mode_ = Mode::Campaign;
  WavePhase wavePhase_ = WavePhase::Idle;
  int score_ = 0, wave_ = 1, kills_ = 0;
  bool victoryAchieved_ = false;
  float gameT_ = 0;
  float minerals_ = 150, gas_ = 50;
  int ammo_ = 30, maxAmmo_ = 30;
  float bunkerHp_ = 750, maxHp_ = 750;
  int dmgLvl_ = 0, armorLvl_ = 0, hpLvl_ = 0, ammoLvl_ = 0;
  float airCD_ = 0, nukeCD_ = 0, shootCD_ = 0;
  // fire button held: after HOLD_DELAY s a volley every shootCD_ where the cursor is (full auto)
  static constexpr float HOLD_DELAY = 0.25f;
  bool holding_ = false;
  float holdT_ = 0;
  void holdFire(float dt);
  std::vector<Unit> roster_;
  std::vector<float> snipeReady_;
  bool concussiveUnlocked_ = false, snipeUnlocked_ = false, superStimUnlocked_ = false;
  int superStimLvl_ = 0;   // Super Stimpack tiers: each lowers the Marines Stimpack needs (cfg::stim_marines)
  int snipeLvl_ = 0, concussiveLvl_ = 0;
  int sensorLvl_ = 0;   // Sensor Tower research: 0 none, 1-3 (radar blips at night out to cfg::sensor_range)
  int armory_[4] = {0, 0, 0, 0};   // hp, dmg, ammo, cd
  int endlessSteps_ = 0;
  float stimT_ = 0;
  int stimVolley_ = 0;
  int waveKills_ = 0, waveNukes_ = 0, waveAirs_ = 0, waveCrates_ = 0;   // waveCrates_: crates carried in this wave
  std::vector<char> spawnQueue_;
  float spawnTimer_ = 0, spawnInterval_ = 1.2f;
  bool nukeInFlight_ = false;
  bool rosterPaused_ = false, menuOpen_ = false, cheatOpen_ = false;
  bool isPaused() const { return rosterPaused_ || menuOpen_ || cheatOpen_ || tutOpen_; }   // a tutorial box pauses too
  float hudTick_ = 0, dmgFlashCD_ = 0;

  std::vector<EnemyP> zerglings_, roaches_;
  std::vector<Corpse> corpses_;
  std::vector<Bullet> bullets_;
  std::vector<Acid> acids_;
  std::vector<UnitFig> marines_;
  std::vector<PropP> crates_;   // crate props (pos in m)
  std::vector<glm::vec2> crateXZ_;
  std::vector<Beam> beams_;
  std::vector<PropP> props_;    // transient visuals owned by tickers
  std::vector<FloatText> floatTexts_;

  // scene
  Mesh *groundMesh_ = nullptr, *rockMesh_[3] = {};
  Material groundMat_, rockMat_;
  std::vector<Instance> rockInst_[3];   // the boulders, by shape
  const Texture* terrainTex_ = nullptr; // the ground's noise (buildTerrainTex)
  void buildTerrainTex();
  Baked bunkerProc_, marineProc_[6], zerglingProc_[3], roachProc_, vikingProc_;
  std::vector<ModelPart> bunkerParts_, shrikeParts_;   // own copies: their materials are re-tinted
  const Model* bunkerModel_ = nullptr;
  const Model* shrikeModel_ = nullptr;
  bool shrikeMounted_ = false;
  std::vector<glm::vec3> bunkerTris_;   // visible hull, world space (firing ports, fire spots)
  void rebuildBunkerTris();
  bool rayBunker(const glm::vec3& o, const glm::vec3& d, glm::vec3& hit) const;
  void buildScene();
  void buildBunker();
  void mountShrike();
  void updateBunkerVisual();
  std::map<std::string, std::unique_ptr<Pool>> pools_;
  Pool* poolFor(const std::string& key, const Baked& parts, const glm::mat4& local);
  Pool* framePool(const std::string& skin, const char* anim, int k);
  Pool* modelPool(const std::string& skin);
  Material bulletMat_, acidMat_, crateMat_;
  Mesh *bulletMesh_ = nullptr, *acidMesh_ = nullptr, *crateMesh_ = nullptr, *beamMesh_ = nullptr,
       *ringMesh_ = nullptr, *laserMesh_ = nullptr, *trailMesh_ = nullptr, *fireballMesh_ = nullptr, *snipeMesh_ = nullptr,
       *dotRingMesh_ = nullptr, *decalMesh_ = nullptr, *unitBoxMesh_ = nullptr;
  const Texture* nukeTargetTex_ = nullptr;
  // night visuals: tracer halos and their light on the ground, Sensor Tower blips lying on the ground
  const Texture *glowTex_ = nullptr, *blipTex_ = nullptr, *puffTex_ = nullptr;   // puffTex_: the nuke smoke's atlas
  Mesh *glowQuadMesh_ = nullptr, *blipMesh_ = nullptr, *vmMesh_ = nullptr;   // vmMesh_: a Viking missile, unit length on +z
  Material vmBodyMat_;
  ParticleSystem mushSmoke_, mushFire_;   // the mushroom clouds' sprites, rebuilt every frame (not simulated)
  ParticleSystem mushGlow_;               // the blasts' glow cards (nukeGlowCards), likewise
  Material tracerGlowMat_, tracerPoolMat_, blipMat_, sensorRingMat_, sensorRingMats_[4];   // sensorRingMats_: per level (their fades differ)
  float sensorRingA_[4] = {};   // each level's ring opacity (updateSensorRing)
  void updateSensorRing(float dt);
  Mesh* sensorRingMesh_[4] = {};   // per Sensor Tower level: the range ring with its 12 triangles
  Mesh* glowRingMesh_ = nullptr;   // a soft band along a unit circle (xz plane)
  Material blastWashMat_;              // grenade blasts' glow on the ground (additive, tinted per instance)
  const Texture* blastTex_ = nullptr;  // its shape: a soft disk with no edge
  Material blobMat_;                   // contact shadows under the Zerg and the units (alpha blended)
  const Texture* blobTex_ = nullptr;
  std::vector<Instance> blobInst_;     // filled every frame
  Material mgBlueGlowMat_, mgBlueCoreMat_;   // concussive shells: their flames and light streaks burn blue
  std::vector<MBlast> mBlasts_;
  GlowLight glows_[MAX_GLOWS];   // small lights this frame: Marauder grenades in flight and their bursts
  int glowCount_ = 0;
  void updateGlows();
  bool glowLit(float x, float z) const;   // near enough a small light to show in the dark
  float nightDark(float x, float z) const;   // how much the night hides a Zerg there (0 in full light, 1 black)
  void marauderBlast(const glm::vec3& at, const EnemyP& struck, float dmg0, const std::vector<EnemyP>& enemies);
  // a crate runner's chest lamps at night (white LED pod) and their beams on the ground
  const Texture* beamTex_ = nullptr;
  Mesh* lampBeamMesh_ = nullptr;
  Material lampBeamMat_;
  ParticleSystem lampFx_;   // the lamps themselves, soft sprites rebuilt every frame
  void drawUnitLamps();
  SpotLight lampSpot_;   // their light (drawUnitLamps sets it, the scene uses it)
  void buildNightFx();
  bool sensed(const Enemy& e) const;   // a Zerg out in the dark within the Sensor Tower's range
  std::vector<NukeBlast> nukes_;   // at most 3: the oldest goes first
  std::vector<Scar> scars_;        // at most 3
  NukeRenderer nukeFx_;
  bool hdrLast_ = false;           // the last frame went through the HDR post passes (the nuke flash is its exposure)
  void updateNukes(float dt);
  void nukeEmberFx(const glm::vec3& p, int count, float ms);   // ms: the blast's SC2 sequence time
  ParticleSystem glow_, smoke_;
  void emit(ParticleSystem& s, float x, float y, float z, float vx, float vy, float vz, float life, float s0, float s1,
            float a0, uint32_t c0, uint32_t c1, float grav, float drag, int fade) {
    s.emit(x, y, z, vx, vy, vz, life, s0, s1, a0, c0, c1, grav, drag, fade);
  }
  void spawnParts(const glm::vec3& p, uint32_t col, int n = 7);
  // GPU particles (compute shaders): the sparks above ×6, plus fireballs, shockwave
  // dust and smoke columns the CPU system has no budget for. Off: the web version's look.
  GpuParticles gpuFx_;
  bool gpuFxOn_ = true;
  bool gpuFx() const { return gpuFxOn_ && gpuFx_.ok(); }
  void setGpuFx(bool on);
  void explosionFx(const glm::vec3& p, uint32_t col, float scale);   // Viking missile strikes (the nuke has its mushroom cloud)
  void deathFx(const Enemy& e);
  void bloodFx(const Enemy& e, const glm::vec3& p, float dx, float dz);   // a round hits it: a little blood along (dx, dz)
  void acidFx(const glm::vec3& p);
  void muzzleFlash(float x, float y, float z, float dx, float dz, uint32_t col, bool big);
  void render();

  // shrike turret
  float shrikeY_ = 1.0f, shrikeYaw_ = 0, shrikeVel_ = 0, shrikeCD_ = 0, shrikeRecoil_ = 0;
  EnemyP shrikeTgt_;
  bool shrikeOD_ = false;
  glm::vec3 shrikeTips_[2] = {{-0.517f, 0.681f, 0.70f}, {0.517f, 0.681f, 0.70f}};
  float shrikeScore(const Enemy& e) const;
  EnemyP shrikePick();
  bool shrikeOverdrive() const;
  void updateShrike(float dt);
  float shrikeMountY(const std::vector<ModelPart>& parts, const glm::mat4& m) const;

  // stim
  bool stimUnlocked() const;
  int stimNeed() const;   // Marines in the bunker Stimpack needs (the last tier: a Marine or a Marauder)
  int stimCount() const;  // those counting toward it
  float stimCost() const;
  void doStim(bool autocast = false);   // autocast: quietly, only if it can
  bool stimAuto_ = false;               // Super Stimpack autocast (right-click STIM): stims as the bunker fires
  void toggleStimAuto();

  // entities
  void syncMarines();
  void setUnitModel(UnitFig& m, Unit type, int idx);
  int slotsUsed() const;
  int slotsLeft() const { return 6 - slotsUsed(); }
  void collectCrateAt(float x, float z);
  // resupply, one crate per wave: callbacks waiting for the crate on its way (empty = none coming)
  std::vector<std::function<void()>> crateCbs_;
  void awaitCrate(std::function<void()> cb);
  void deliverCrate(float x, float z);
  void crateIn();
  void sendMarine(float tx, float tz, std::function<void()> cb);
  void updateMarines(float dt);
  void makeCrate(float x, float z);
  void makeBeam(float x, float z, uint32_t col);
  EnemyP makeZergling(float x, float z);
  EnemyP makeRoach(float x, float z);
  void enemyLook(Enemy& e, const char* skin, const std::string& procKey, const Baked& proc);
  void walkStep(Enemy& e, float dist);
  float attackReach(const Enemy& e) const;
  glm::vec3 roachMouth(const char* skin);
  void roachAttack(Enemy& e, float dt, float ex, float ez);
  void spitAcid(Enemy& e);
  void updateAcids(float dt);
  void updateCorpses(float dt);
  void killEnemy(const EnemyP& e, bool reward, bool splash = true, const Fling* fling = nullptr);   // splash: its blood (not for a nuke: burnt)
  std::vector<EnemyP> allEnemies() const;
  Bullet makeBullet(float tx, float tz, float spread, Unit u, float ox, float oy, float oz, float ty);
  static void orientBullet(Bullet& b);
  glm::vec3 bunkerPort(float bearing);
  std::map<int, glm::vec3> portCache_;
  void shoot(float tx, float tz);
  void strikeAt(float tx, float tz, uint32_t col, float r);
  void randPos(float& x, float& z);
  void doAirstrike();
  void fireVikingMissile(glm::vec2 tgt, EnemyP e);
  void updateVikingMissiles(float dt);
  void launchViking(std::vector<std::pair<glm::vec2, EnemyP>> targets, glm::vec2 cratePos);
  void removeViking();
  void updateViking(float dt);
  glm::mat4 vikingMatrix() const;   // its model's placement (flying along +x, banked a little)
  bool vikingActive_ = false;
  glm::vec3 vikingPos_{0};
  std::vector<VMissile> vMissiles_;
  Material vmFlameGlowMat_, vmFlameCoreMat_;   // their exhaust flames and light streaks
  Mesh* streakMesh_ = nullptr;                 // a streak segment: unit cylinder (r 1, height 1 on y)
  float vMissileSide_ = 1;   // wings take turns
  void doNuke(bool force = false);
  void ghostSnipeTick(float dt);
  int runnerIdx_ = -1;                 // roster index of the elected crate runner (-1: a Marine, else any free unit)
  void electRunner(int idx);           // the roster: click a unit (again: back to any)
  std::vector<float> marauderReady_;   // per Marauder, in roster order: game time it may fire again
  int marauderShots_ = 0;              // grenades fired: every third costs a round (the first of each three)
  void marauderFireTick();             // Marauders fire on their own (not with the volleys)
  bool marauderTarget(glm::vec2& at) const;

  // bunker fire
  std::vector<glm::vec3> fireSpots_;
  bool fireSpotsValid_ = false;
  float fireAccum_[6][3] = {};
  void updateBunkerFire(float dt);

  // waves and economy
  void startWave();
  void waveCleared();
  int endlessWaves() const { return mode_ == Mode::Endless ? std::max(0, wave_ - 15) : 0; }
  void reinforce();
  float repairMult() const;
  float repairRate() const;
  void armoryCost(int k, int& m, int& g) const;
  float armoryCdMult(int n = -1) const;
  float strikeCooldown() const;
  float nukeCooldown() const;
  bool armoryMaxed(int k) const;
  bool armoryAffordable() const;
  void buyArmory(int k);
  void buy(const std::string& type);
  std::string hireUnit(Unit u);
  void dismissUnit(int idx);
  void researchUpgrade(const std::string& key, int tier = -1);
  void resetRoster();
  std::string needText(float m, float g) const;

  // game flow
  void update(float dt);
  void clearEntities();
  void newGameState();   // a new game's bunker, squad, resources and cooldowns (start(), and the HUD tour on the title)
  void start(Mode m);
  void restart() { start(mode_); }
  void gameOver();
  void campaignVictory();
  void continueEndless();
  void showTitle();
  void showResults(bool victory, bool newlyUnlocked, bool newRecord);
  bool endlessUnlocked() const;
  bool bestEndless(int& wave, int& score) const;
  void skipAssetWait() { skipWait_ = true; }
  bool skipWait_ = false;

  // saving
  storage::Record snapshot() const;
  bool writeSave(const std::string& slot, const storage::Record* override = nullptr);
  bool readSave(const std::string& slot, storage::Record& r) const;
  bool validSave(const storage::Record& r) const;
  std::string saveLabel(const storage::Record& r) const;
  void saveGame();
  void loadGame(bool autoSlot);
  void autoSave();
  std::string saveMsg_;
  bool saveMsgOk_ = false, saveMsgSet_ = false;

  // cheats
  void openCheat();
  void closeCheat();
  void submitCheat(const std::string& code);
  std::string cheatBuf_;
  char cheatInput_[64] = {0};
  std::string cheatMsg_;
  uint32_t cheatMsgCol_ = 0xff4444;
  float cheatCloseT_ = -1;
  bool cheatFocus_ = false;

  // menus
  void openMenu();
  void closeMenu();
  void openRoster();
  void closeRoster();
  void toggleRoster() { rosterPaused_ ? closeRoster() : openRoster(); }
  void setGfx(bool high);
  void setDisplay(bool fullscreen, display::Size sz);
  std::vector<display::Gpu> gpus_;
  std::string gpuRunning_;
  bool gpuChoiceDedicated_ = true, gpuRunningDedicated_ = true, relaunch_ = false;
  void toggleMute();

  // HUD state
  std::string status_ = "READY";
  float statusFlash_ = 0;
  void setStatus(const std::string& s) { status_ = s; statusFlash_ = 0.6f; }
  void flash(int which);   // 0 nuke · 1 dmg · 2 repair
  float flashT_[3] = {-1, -1, -1};   // seconds since the flash (real time), -1 = off
  float slotOverlayOpacity_ = 0, slotOverlayTarget_ = 0, bunkerHoverTimer_ = 0;
  void checkBunkerHover(float dt);
  int lastMin_ = 0, lastGas_ = 0;
  float popMin_ = 0, popGas_ = 0;
  std::string tipCmd_;
  // menu tabs (the last one chosen opens again)
  static constexpr int MENU_TAB_COUNT = 6;
  static const char* menuTabKey(int i) {
    static const char* k[MENU_TAB_COUNT] = {"game", "settings", "display", "controls", "tutorial", "cheats"};
    return k[i];
  }
  int menuTab_ = 0;
  // overlay (title / results / HOW TO PLAY)
  enum class Overlay { None, Title, Results, HowTo } overlay_ = Overlay::Title;
  void showHowTo();
  void drawHowTo();

  // tutorial: the HUD tour, then hints the first time a mechanic matters (game_ui.cpp)
  struct TutStep {
    std::string id, title, text;        // text: <b> and <kbd> markup, as the web version's
    std::vector<std::string> focus;     // HUD controls to spotlight (hudRect ids)
    std::string keys;                   // keys the hint names: they close it and do their job
    std::string titleKey;               // a tour page's hotkey, shown after the title
  };
  bool tutOn_ = true, tutRun_ = true;   // hints on (stored) · this run allows them (--no-tutorial)
  bool postRun_ = true;                 // HDR post-processing this run (--post off: straight to the window)
  std::vector<std::string> tutSeen_;    // ids seen (stored)
  bool tutOpen_ = false;                // a hint or a tour page is shown: the game pauses
  TutStep tutStep_;
  int tourI_ = -1;                      // tour page shown (-1 = none)
  float tutNext_ = 0;                   // no new hint before this gameT_
  float tutShownAt_ = 0;                // uiTime_ when the box opened
  bool tourOverlay_ = false;            // the tour hid the title / results card: it comes back after
  Overlay tourSavedOverlay_ = Overlay::None;
  void tutLoad();
  void tutSave();
  bool tutSeen(const std::string& id) const;
  bool tutWhen(const std::string& id);
  bool tutLeft() const;
  void tutCounts(int& seen, int& total) const;
  void tutorialTick();
  void tutOpenStep(const TutStep& st);
  void tourPage(int i);
  bool tourShown(int i) const;
  void startTour();
  void tutNextPage();
  void tutBack();
  void tutDismiss();
  void tutSkip();
  void setTutorial(bool on);
  void replayTutorial();
  bool tutorialPress();                 // a mouse press while a box is open: false = swallowed / the box's own
  bool tutorialKey(int key);            // true = the key was the box's
  void drawTutorial();
  // HUD control rects (window px) recorded as they are drawn: the tutorial's spotlight
  struct HudRect { float x0, y0, x1, y1; int frame; };
  std::map<std::string, HudRect> hudRects_;
  int uiFrame_ = 0;
  void hudRect(const std::string& id, float x0, float y0, float x1, float y1) { hudRects_[id] = {x0, y0, x1, y1, uiFrame_}; }
  bool focusRect(const std::vector<std::string>& ids, float& x0, float& y0, float& x1, float& y1) const;
  float tutBoxX0_ = 0, tutBoxY0_ = 0, tutBoxX1_ = 0, tutBoxY1_ = 0;   // the box as last drawn
  bool resVictory_ = false, resNewly_ = false, resRecord_ = false;

  // hologram portrait
  std::vector<glm::vec3> holoBase_, holoUpg_, holoTur_;
  glm::vec2 holoTurPivot_{0};
  bool holoBuilt_ = false;
  std::shared_ptr<struct HoloJob> holoJob_;   // edges extracted on a worker, picked up when ready
  float holoAngle_ = 0, holoUpgT_ = 0, holoUpgA_ = 0, holoTurT_ = 0, holoTurA_ = 0;
  void buildPortrait();
  void updatePortraitBunker(bool instant = false);

  // UI
  void drawUI(float dt);
  void drawOverlayCanvas();   // HP bars, Sensor Tower blips, floating numbers, crosshair
  void drawTopBar();
  void drawConsole(float dt);
  void drawPortrait(float x, float y, float w, float h);
  void drawCommandCard(float x, float y, float w, float h);
  void drawTooltip();
  void drawSlotOverlay();
  void drawRoster();
  void drawMenu();
  void drawCheat();
  void drawTitle();
  void drawResults();
  void drawFlashes();
  float uiTime_ = 0;
  // performance overlay (F3): CPU time per frame stage, averaged over ~0.5 s
  bool perfOverlay_ = false, benchFire_ = false;
  struct Perf { double update = 0, render = 0, ui = 0, frame = 0, worst = 0; int frames = 0; } perfAcc_, perfShown_, perfRun_;
  double perfT_ = 0;
  void drawPerf();
};
