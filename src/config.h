// Game constants, asset paths and rank table — a port of js/config.js.
#pragma once
#include <cstdint>

namespace cfg {
// ─── CONSTANTS ───────────────────────────────────────────────────────────────
constexpr int   base_ammo = 30;
constexpr int   victory_wave = 15;          // campaign: clearing this wave wins and unlocks Endless
constexpr float airstrike_cd = 20, nuke_cd = 15;
constexpr int   nuke_wave = 5, roach_wave = 4;
constexpr float stim_duration = 11, stim_hp = 10, stim_hp_marauder = 20;   // SC2 Stimpack: 11 s, 10 HP per Marine, 20 per Marauder (paid by the bunker)
constexpr int   stim_marines[4] = {6, 4, 2, 1};     // Marines in the bunker Stimpack needs: base, then per Super Stimpack tier
constexpr float superstim_heal = 6;                 // Super Stimpack: no HP cost, repairs the bunker 6 HP/s while active
constexpr int   metabolic_wave = 8;
constexpr float metabolic_boost = 1.6f;             // Metabolic Boost: from wave 8 Zerglings run 60% faster
constexpr int   webby_wave = 12;                    // the boosted Zerglings' "webby" skin
constexpr float repair_rate = 25;                   // Lv3 hull self-repair, HP/s
// Endless: every wave past the campaign's last one reinforces the bunker
constexpr float endless_dmg = 0.05f, endless_hp = 60, endless_repair = 0.10f;
constexpr int   endless_ammo = 2;
// Endless armory: repeatable purchases, each 30% dearer than the last of its kind (ammo: 15%, so more
// can be bought; each adds half a starting magazine)
constexpr float armory_growth = 1.3f, armory_ammo_growth = 1.15f, armory_hp = 150, armory_dmg = 0.10f;
constexpr int   armory_ammo = 15;
constexpr float armory_cd = 0.9f, armory_cd_min = 0.4f;
constexpr int dmg_costs[3][2]   = {{100, 20}, {160, 32}, {256, 51}};
constexpr int armor_costs[3][2] = {{125, 30}, {200, 48}, {320, 77}};
constexpr int hp_costs[3][2]    = {{150, 40}, {255, 68}, {434, 116}};
constexpr int ammo_costs[3][2]  = {{80, 15}, {130, 25}, {200, 40}};
// Sensor Tower research: at night, Zerg in the dark within this range of the bunker show as radar blips.
// Per level (0 = not researched); the night's lit edge is ~9, so the band beyond it doubles: +1.75, +3.5, +7
constexpr float sensor_range[4] = {0, 10.75f, 12.5f, 16};
constexpr int sensor_costs[3][2] = {{100, 25}, {175, 50}, {275, 100}};
}  // namespace cfg

// ─── ASSET PATHS ─────────────────────────────────────────────────────────────
// The same SC2 models and textures as the web version (assets/models). A model
// that fails to load is drawn with procedural shapes instead.
constexpr uint32_t TEAM_TERRAN = 0x2f6dff, TEAM_ZERG = 0x9a1c1c;
constexpr float SC2_SCALE = 3.0f / 2.726f;   // world units per SC2 unit (the bunker: 2.726 wide in SC2, 3.0 here)
constexpr float ZERGLING_SCALE = SC2_SCALE * 0.8f;
constexpr float ANIM_SPEED = 1.25f;           // baked walk / attack cycles play this much faster
constexpr float PI_F = 3.14159265358979f;

struct TexDef {
  const char* diffuse = nullptr;
  const char* emissive = nullptr;
  const char* normal = nullptr;
  float normalY = 0;   // -1: OpenGL-style normal map (green = up)
};
struct AssetDef {
  const char* key = nullptr;
  const char* glb = nullptr;        // SC2 model from tools/m3-to-glb.mjs: bind pose + baked frames
  const char* obj = nullptr;        // OBJ model (the bunker, Shrike and Viking)
  float scale = 0;                  // size of the largest dimension in world units
  float unitScale = 0;              // world units per model unit (keeps SC2 proportions); wins over scale
  float yOff = 0, rotY = 0;
  bool hasTeamColor = false;
  uint32_t teamColor = 0;
  float stride = 1;                 // world units one walk loop covers
  TexDef tex;
  const char* meshTexName = nullptr;   // one mesh with its own texture set (the bunker's upgrade frame)
  TexDef meshTex;
};

#define TERRAN_DIR "assets/models/terran/"
#define ZERG_DIR "assets/models/zerg/"
inline const AssetDef ASSETS[] = {
  {.key = "bunker", .obj = TERRAN_DIR "bunker.obj", .scale = 3.0f,
   .tex = {TERRAN_DIR "bunker_diffuse.dds", TERRAN_DIR "bunker_emissive.dds", TERRAN_DIR "bunker_normal.dds"},
   .meshTexName = "Mesh.002",   // the Neosteel frame (HP Lv2+), mapped to the upgrade texture
   .meshTex = {TERRAN_DIR "bunker_upgrade_diffuse.dds", TERRAN_DIR "bunker_upgrade_emissive.dds"}},
  // Shrike turret (HP Lv3): 1.75 wide vs the bunker's 2.73 in SC2 units
  {.key = "shrike", .obj = TERRAN_DIR "bunker_shrike_turret.obj", .scale = 1.926f,
   .tex = {TERRAN_DIR "bunkerperditionupgrade_diffuse.dds", TERRAN_DIR "bunkerperditionupgrade_emissive.dds"}},
  {.key = "marine", .glb = TERRAN_DIR "marine.glb", .unitScale = SC2_SCALE, .hasTeamColor = true, .teamColor = TEAM_TERRAN,
   .stride = 2.25f * 0.667f * SC2_SCALE,
   .tex = {TERRAN_DIR "marine_diffuse.dds", nullptr, TERRAN_DIR "marine_normal.dds"}},
  {.key = "marauder", .glb = TERRAN_DIR "marauder.glb", .unitScale = SC2_SCALE, .hasTeamColor = true, .teamColor = TEAM_TERRAN,
   .stride = 2.25f * 0.866f * SC2_SCALE,
   .tex = {TERRAN_DIR "marauder_diffuse.dds", TERRAN_DIR "marauder_emissive.dds", TERRAN_DIR "marauder_normal.dds", -1}},
  // the bind pose holds the rifle below the feet: sink it so the boots touch the ground
  {.key = "ghost", .glb = TERRAN_DIR "ghost.glb", .unitScale = SC2_SCALE, .yOff = -0.38f * SC2_SCALE,
   .hasTeamColor = true, .teamColor = TEAM_TERRAN, .stride = 2.25f * 0.667f * SC2_SCALE,
   .tex = {TERRAN_DIR "ghost_diffuse.dds"}},
  {.key = "nukeMissile", .glb = TERRAN_DIR "nuke_missile.glb", .unitScale = SC2_SCALE * 0.52f,
   .tex = {TERRAN_DIR "nuke_missile.dds"}},
  {.key = "viking", .obj = TERRAN_DIR "viking.obj", .scale = 2.25f, .rotY = -PI_F / 2,
   .tex = {TERRAN_DIR "viking_diffuse.jpg", TERRAN_DIR "viking_emissive.jpg"}},
  // Zerglings by wave tier: base · Metabolic Boost (wave 8+, winged) · its webby skin (wave 12+)
  {.key = "zergling", .glb = ZERG_DIR "zergling.glb", .unitScale = ZERGLING_SCALE, .hasTeamColor = true, .teamColor = TEAM_ZERG,
   .stride = 2.95f * 2.0f * ZERGLING_SCALE,
   .tex = {ZERG_DIR "zergling_diffuse.dds", ZERG_DIR "zergling_emissive.dds", ZERG_DIR "zergling_normal.dds"}},
  {.key = "zergling_upgrade", .glb = ZERG_DIR "zergling_upgrade.glb", .unitScale = ZERGLING_SCALE, .hasTeamColor = true, .teamColor = TEAM_ZERG,
   .stride = 2.95f * 2.0f * ZERGLING_SCALE,
   .tex = {ZERG_DIR "zergling_diffuse.dds", ZERG_DIR "zergling_emissive.dds", ZERG_DIR "zergling_normal.dds"}},
  {.key = "zergling_webby", .glb = ZERG_DIR "zergling_upgrade.glb", .unitScale = ZERGLING_SCALE, .hasTeamColor = true, .teamColor = TEAM_ZERG,
   .stride = 2.95f * 2.0f * ZERGLING_SCALE,
   .tex = {ZERG_DIR "zergling_webby_diffuse.dds", ZERG_DIR "zergling_webby_emissive.dds", ZERG_DIR "zergling_webby_normal.dds"}},
  // Roach skins by wave tier (same mesh): base · ex1a (wave 9+) · ex3 (wave 13+)
  {.key = "roach", .glb = ZERG_DIR "roach.glb", .unitScale = SC2_SCALE, .hasTeamColor = true, .teamColor = TEAM_ZERG,
   .stride = 2.25f * 0.434f * SC2_SCALE, .tex = {ZERG_DIR "roach_diffuse.dds"}},
  {.key = "roach_ex1a", .glb = ZERG_DIR "roach.glb", .unitScale = SC2_SCALE, .hasTeamColor = true, .teamColor = TEAM_ZERG,
   .stride = 2.25f * 0.434f * SC2_SCALE, .tex = {ZERG_DIR "roach_ex1a_diffuse.dds", ZERG_DIR "roach_ex1a_emissive.dds"}},
  {.key = "roach_ex3", .glb = ZERG_DIR "roach.glb", .unitScale = SC2_SCALE, .hasTeamColor = true, .teamColor = TEAM_ZERG,
   .stride = 2.25f * 0.434f * SC2_SCALE, .tex = {ZERG_DIR "roach_ex3_diffuse.dds", ZERG_DIR "roach_ex3_emissive.dds"}},
};
constexpr int ASSET_COUNT = sizeof(ASSETS) / sizeof(ASSETS[0]);

// Load order: the bunker first, then what waves 1-3 need — the title screen's
// start buttons wait for those two stages — then the rest.
inline const char* const LOAD_STAGES[][8] = {
  {"bunker"},
  {"zergling", "marine"},
  {"roach", "viking", "nukeMissile"},
  {"marauder", "ghost", "zergling_upgrade", "roach_ex1a", "shrike", "zergling_webby", "roach_ex3"},
};
constexpr int LOAD_STAGE_COUNT = 4, ESSENTIAL_STAGES = 2;

// Roach acid (SC2: range 4, one spit every 1.43 s). The acid leaves the mouth
// ("Ref_Head") at the lunge, 420 ms into the Attack animation.
namespace roach_acid {
constexpr float range = 1.45f + 4 * SC2_SCALE + 0.7f;   // hull radius + SC2 range + the Roach's reach
constexpr float cooldown = 1.43f, anim = 1.0f, spit = 0.42f;
constexpr float mouth[3] = {-0.013f, 0.436f, 0.688f};   // Ref_Head at the spit, model units
constexpr float flight = 0.55f, gravity = 9;            // lobbed: seconds in the air · arc gravity
}
// Zergling bite: the 533 ms SC2 Attack loops at the hull; the jaws close half way
namespace zergling_bite { constexpr float anim = 0.533f, bite = 0.5f; }

constexpr const char* NUKE_TARGET_TEX = TERRAN_DIR "nuke_target.dds";   // SC2 nuke target reticle

#define UI_DIR "assets/ui/"
namespace ui_icons {
constexpr const char* mineral = UI_DIR "icon-mineral-terran.png";
constexpr const char* gas = UI_DIR "icon-gas-terran.png";
constexpr const char* dmg[4] = {UI_DIR "btn-upgrade-terran-infantryweaponslevel0.png", UI_DIR "btn-upgrade-terran-infantryweaponslevel1.png",
                                UI_DIR "btn-upgrade-terran-infantryweaponslevel2.png", UI_DIR "btn-upgrade-terran-infantryweaponslevel3.png"};
constexpr const char* ammo[4] = {UI_DIR "btn-upgrade-terran-magrailmunitions.png", UI_DIR "btn-upgrade-terran-magrailmunitions.png",
                                 UI_DIR "btn-upgrade-terran-magfieldaccelerator.png", UI_DIR "btn-upgrade-terran-enhancedmunitions.png"};
constexpr const char* superstim = UI_DIR "btn-upgrade-terran-superstimppack.png";
constexpr const char* concussive = UI_DIR "btn-upgrade-terran-explosiveshrapnelshells.png";
constexpr const char* snipe = UI_DIR "btn-upgrade-terran-ui-lazertargetingsystem.png";
// Sensor Tower: no PNG yet — the "sensor" stroke glyph (ui.cpp) stands in until
// btn-building-terran-sensortower.png is exported
constexpr const char* armor = UI_DIR "btn-upgrade-terran-buildingarmor.png";
constexpr const char* neosteel = UI_DIR "btn-upgrade-terran-neosteelframe.png";
constexpr const char* fleet = UI_DIR "btn-progression-hornerhan-5-fleetcalldown.png";
constexpr const char* nuke = UI_DIR "nukes_coop.png";
constexpr const char* detector = UI_DIR "btn-status-detector.png";
constexpr const char* shrike = UI_DIR "btn-upgrade-terran-hisecautotracking.png";
constexpr const char* weapons3 = UI_DIR "btn-upgrade-terran-infantryweaponslevel3.png";
constexpr const char* enhanced = UI_DIR "btn-upgrade-terran-enhancedmunitions.png";
}

// Music: the StarCraft soundtrack (Glenn Stafford), AAC. Lists play in turn; title and battle loop.
#define AUDIO_DIR "assets/audio/"
constexpr float MUSIC_LEVEL = 0.4f;   // under the effects, before the master volume

constexpr const char* BLIZZARD_NOTICE =
  "StarCraft\xC2\xAE and its models, textures, icons and music are \xC2\xA9 Blizzard Entertainment, Inc. "
  "Non-commercial fan project \xE2\x80\x94 not affiliated with or endorsed by Blizzard Entertainment.";

struct Rank { const char* name; int kills; uint32_t color; };
inline const Rank RANKS[] = {
  {"PRIVATE", 0, 0xaaaaaa},   {"CORPORAL", 10, 0xcd7f32}, {"SERGEANT", 25, 0xcd7f32}, {"LIEUTENANT", 50, 0xc0c0c0},
  {"CAPTAIN", 90, 0xc0c0c0},  {"MAJOR", 140, 0xffd700},   {"COLONEL", 200, 0xffd700}, {"GENERAL", 280, 0xff4444},
};
inline const Rank& getRank(int k) {
  const Rank* r = &RANKS[0];
  for (const Rank& x : RANKS) if (k >= x.kills) r = &x;
  return *r;
}

constexpr float MARINE_SLOTS[6][2] = {{.3f, 0}, {-.3f, .4f}, {.5f, -.4f}, {-.5f, -.3f}, {.0f, .6f}, {.0f, -.6f}};
