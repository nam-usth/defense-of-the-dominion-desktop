// Saving (two slots: manual and the autosave after each cleared wave) and the
// cheat console — game.js's Save / Load and CHEATS.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <sstream>

#include "audio.h"
#include "config.h"
#include "game.h"
#include "log.h"

static std::string num(double v) {
  std::ostringstream o;
  o.precision(10);
  o << v;
  return o.str();
}
static long long nowSec() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
static const char* phaseName(WavePhase p) {
  switch (p) {
    case WavePhase::Combat: return "combat";
    case WavePhase::Clearing: return "clearing";
    case WavePhase::Cleared: return "cleared";
    default: return "idle";
  }
}
static bool parseNum(const storage::Record& r, const char* k, double& out) {
  auto it = r.find(k);
  if (it == r.end() || it->second.empty()) return false;
  char* end = nullptr;
  out = std::strtod(it->second.c_str(), &end);
  return end && *end == 0 && std::isfinite(out);
}
static double getNum(const storage::Record& r, const char* k, double def = 0) {
  double v;
  return parseNum(r, k, v) ? v : def;
}
static std::string getStr(const storage::Record& r, const char* k) {
  auto it = r.find(k);
  return it == r.end() ? "" : it->second;
}
static bool unitFromKey(const std::string& k, Unit& u) {
  for (int i = 0; i < 3; i++)
    if (k == unitDef((Unit)i).key) { u = (Unit)i; return true; }
  return false;
}

storage::Record Game::snapshot() const {
  storage::Record r;
  r["wave"] = num(wave_);
  r["score"] = num(score_);
  r["kills"] = num(kills_);
  r["minerals"] = num(minerals_);
  r["gas"] = num(gas_);
  r["bunkerHp"] = num(bunkerHp_);
  r["maxHp"] = num(maxHp_);
  r["ammo"] = num(ammo_);
  r["maxAmmo"] = num(maxAmmo_);
  r["dmgLvl"] = num(dmgLvl_);
  r["armorLvl"] = num(armorLvl_);
  r["hpLvl"] = num(hpLvl_);
  r["ammoLvl"] = num(ammoLvl_);
  std::string ro;
  for (size_t i = 0; i < roster_.size(); i++) ro += (i ? "," : "") + std::string(unitDef(roster_[i]).key);
  r["roster"] = ro;
  r["concussiveUnlocked"] = concussiveUnlocked_ ? "1" : "0";
  r["snipeUnlocked"] = snipeUnlocked_ ? "1" : "0";
  r["superStimUnlocked"] = superStimUnlocked_ ? "1" : "0";
  r["concussiveLvl"] = num(concussiveLvl_);
  r["snipeLvl"] = num(snipeLvl_);
  r["sensorLvl"] = num(sensorLvl_);
  r["armoryHp"] = num(armory_[0]);
  r["armoryDmg"] = num(armory_[1]);
  r["armoryAmmo"] = num(armory_[2]);
  r["armoryCd"] = num(armory_[3]);
  r["endlessSteps"] = num(endlessSteps_);
  r["gfxQuality"] = gfxHigh_ ? "HIGH" : "LOW";
  r["wavePhase"] = phaseName(wavePhase_);
  r["gameT"] = num(gameT_);
  r["victoryAchieved"] = victoryAchieved_ ? "1" : "0";
  r["gameMode"] = mode_ == Mode::Endless ? "endless" : "campaign";
  // An airstrike's crate run / a nuke countdown can't be saved: loading would keep
  // the cooldown but lose the effect (e.g. 0 ammo with R blocked for 20 s)
  bool crateRun = vikingActive_ || std::any_of(marines_.begin(), marines_.end(), [](const UnitFig& m) { return (bool)m.cb; });
  r["airCD"] = num(crateRun ? 0 : airCD_);
  r["nukeCD"] = num(nukeInFlight_ ? 0 : nukeCD_);
  r["stimT"] = num(stimT_);   // the bunker HP already paid for this stim
  r["stimVolley"] = num(stimVolley_);
  r["ts"] = num((double)nowSec());
  return r;
}

bool Game::writeSave(const std::string& slot, const storage::Record* override) {
  storage::Record r = snapshot();
  if (override) for (auto& [k, v] : *override) r[k] = v;
  return storage::writeRecord(slot, r);
}
bool Game::readSave(const std::string& slot, storage::Record& r) const { return storage::readRecord(slot, r); }

// Reject saves that cannot be restored BEFORE the running game is torn down
bool Game::validSave(const storage::Record& d) const {
  double v;
  for (const char* k : {"wave", "score", "minerals", "gas", "bunkerHp", "maxHp", "ammo", "maxAmmo"})
    if (!parseNum(d, k, v)) return false;
  if (getNum(d, "wave") < 1) return false;
  std::string ro = getStr(d, "roster");
  if (ro.empty()) return false;
  std::stringstream ss(ro);
  std::string u;
  Unit x;
  while (std::getline(ss, u, ','))
    if (!unitFromKey(u, x)) return false;
  return true;
}

static std::string ago(double ts) {
  long long m = std::llround((nowSec() - ts) / 60.0);
  if (m < 1) return "just now";
  if (m < 60) return std::to_string(m) + " min ago";
  if (m < 1440) return std::to_string(std::llround(m / 60.0)) + " h ago";
  return std::to_string(std::llround(m / 1440.0)) + " d ago";
}
std::string Game::saveLabel(const storage::Record& d) const {
  return std::string(getStr(d, "gameMode") == "endless" ? "Endless" : "Campaign") + " \xC2\xB7 wave " + std::to_string((int)getNum(d, "wave")) +
         (getStr(d, "wavePhase") == "combat" ? "" : " cleared") + " \xC2\xB7 " + ago(getNum(d, "ts"));
}

void Game::saveGame() {
  saveMsgSet_ = true;
  if (phase_ != Phase::Playing) { saveMsg_ = "Nothing to save \xE2\x80\x94 start a game first"; saveMsgOk_ = false; return; }
  bool ok = writeSave("save_v1");
  storage::Record d;
  saveMsg_ = ok && readSave("save_v1", d) ? "Saved \xE2\x80\x94 " + saveLabel(d) : "Save failed \xE2\x80\x94 the save folder is not writable";
  saveMsgOk_ = ok;
  setStatus(ok ? "Game saved \xE2\x9C\x93" : "Save failed \xE2\x80\x94 the save folder is not writable");
  audio::play(ok ? "buy" : "deny");
}

void Game::autoSave() { if (phase_ == Phase::Playing) writeSave("autosave_v1"); }

// autoSlot: the autosave (title CONTINUE), otherwise the manual save
void Game::loadGame(bool autoSlot) {
  storage::Record d;
  bool have = readSave(autoSlot ? "autosave_v1" : "save_v1", d);
  if (!have || !validSave(d)) {
    std::string msg = have ? "This save cannot be loaded (damaged or from an old version)" : "No saved game found";
    saveMsgSet_ = true; saveMsg_ = msg; saveMsgOk_ = false;
    setStatus(msg);
    audio::play("deny");
    return;
  }
  start(mode_);   // restart cleanly, then restore the state
  wave_ = (int)getNum(d, "wave");
  score_ = (int)getNum(d, "score");
  kills_ = (int)getNum(d, "kills");
  minerals_ = (float)getNum(d, "minerals");
  gas_ = (float)getNum(d, "gas");
  bunkerHp_ = (float)getNum(d, "bunkerHp");
  maxHp_ = (float)getNum(d, "maxHp");
  ammo_ = (int)getNum(d, "ammo");
  maxAmmo_ = (int)getNum(d, "maxAmmo");
  dmgLvl_ = (int)getNum(d, "dmgLvl");
  armorLvl_ = (int)getNum(d, "armorLvl");
  hpLvl_ = (int)getNum(d, "hpLvl");
  ammoLvl_ = (int)getNum(d, "ammoLvl");
  roster_.clear();
  std::stringstream ss(getStr(d, "roster"));
  std::string u;
  while (std::getline(ss, u, ',')) { Unit x; if (unitFromKey(u, x)) roster_.push_back(x); }
  concussiveUnlocked_ = getStr(d, "concussiveUnlocked") == "1";
  snipeUnlocked_ = getStr(d, "snipeUnlocked") == "1";
  superStimUnlocked_ = getStr(d, "superStimUnlocked") == "1";
  concussiveLvl_ = (int)getNum(d, "concussiveLvl");
  snipeLvl_ = (int)getNum(d, "snipeLvl");
  sensorLvl_ = std::clamp((int)getNum(d, "sensorLvl"), 0, 3);   // older saves: none (it indexes the range table)
  armory_[0] = (int)getNum(d, "armoryHp");   // HP / ammo purchases are already in maxHp / maxAmmo
  armory_[1] = (int)getNum(d, "armoryDmg");
  armory_[2] = (int)getNum(d, "armoryAmmo");
  armory_[3] = (int)getNum(d, "armoryCd");
  snipeReady_.assign(std::count(roster_.begin(), roster_.end(), Unit::Ghost), 0.f);
  gameT_ = (float)getNum(d, "gameT");
  victoryAchieved_ = getStr(d, "victoryAchieved") == "1";
  std::string gm = getStr(d, "gameMode");
  mode_ = gm == "endless" ? Mode::Endless : gm == "campaign" ? Mode::Campaign : (victoryAchieved_ ? Mode::Endless : Mode::Campaign);
  if (mode_ == Mode::Endless && !endlessUnlocked()) storage::set("endless_unlocked", "1");   // an Endless save means the campaign was won
  airCD_ = (float)getNum(d, "airCD");
  nukeCD_ = (float)getNum(d, "nukeCD");
  stimT_ = (float)getNum(d, "stimT");   // a stim paid for keeps running
  stimVolley_ = (int)getNum(d, "stimVolley");
  syncMarines();
  std::string gq = getStr(d, "gfxQuality");
  if (!gq.empty()) setGfx(gq == "HIGH");
  updateBunkerVisual();
  // start() queued wave 1: rebuild the queue for the restored wave. A save taken
  // between waves already holds that wave's clear bonus: resume at the next one
  std::string wp = getStr(d, "wavePhase");
  if (wp == "cleared" || wp == "clearing") { wave_ = (int)getNum(d, "wave") + 1; ammo_ = maxAmmo_; }
  endlessSteps_ = (int)getNum(d, "endlessSteps");
  reinforce();
  startWave();
  shrikeOD_ = shrikeOverdrive();   // a restored overdrive is not news: keep the "Loaded" message
  setStatus(std::string("Loaded \xE2\x80\x94 ") + (mode_ == Mode::Endless ? "Endless" : "Campaign") + " wave " + std::to_string(wave_) + " (saved " + ago(getNum(d, "ts")) + ")");
  closeMenu();
}

// ─── Cheat console (type "dotd" during play) ─────────────────────────────────
struct Cheat { const char* code; const char* desc; };
static const Cheat CHEATS[] = {
  {"mng", "7500 Minerals + 7500 Gas"},
  {"fu", "All bunker upgrades maxed \xE2\x80\x94 self-repair online!"},
  {"endv", "Victory!"},
  {"endd", "Bunker destroyed!"},
  {"korhal", "All cooldowns reset (Airstrike + Nuke ready)"},
  {"medic", "Bunker fully repaired"},
  {"terran", "+2 Marines deployed"},
  {"siege", "Damage maxed (lv3) instantly"},
  {"bunker", "HP maxed + Armor maxed instantly"},
  {"sensor", "Sensor Tower maxed \xE2\x80\x94 blips out to 16"},
  {"zergrush", "Skip +5 waves \xE2\x80\x94 Zerg hive mind surges!"},
  {"swarm", "Spawn 10 Zerglings right now"},
  {"roachwave", "Spawn 5 Roaches right now"},
  {"nuclear", "Triple nuke \xE2\x80\x94 all enemies obliterated"},
  {"airstrike", "5 consecutive airstrikes"},
  {"mineral", "+500 Minerals"},
  {"gas", "+500 Gas"},
  {"wave", "Skip to next wave immediately"},
  {"immortal", "Bunker HP set to 99999 (godmode)"},
  {"kaboom", "Nuke every enemy position individually"},
  {"general", "Instant rank: GENERAL (280 kills added)"},
  {"help", "Lists all cheat codes"},
};

void Game::openCheat() {
  if (phase_ != Phase::Playing) return;
  cheatOpen_ = true;   // pauses gameplay
  cheatInput_[0] = 0;
  cheatMsg_.clear();
  cheatFocus_ = true;
  cheatCloseT_ = -1;
}
void Game::closeCheat() {
  cheatOpen_ = false;
  cheatCloseT_ = -1;
}

void Game::submitCheat(const std::string& raw) {
  std::string key = raw;
  key.erase(0, key.find_first_not_of(" \t"));
  key.erase(key.find_last_not_of(" \t") + 1);
  std::transform(key.begin(), key.end(), key.begin(), ::tolower);
  const Cheat* c = nullptr;
  for (const Cheat& x : CHEATS) if (key == x.code) c = &x;
  if (!c) {
    cheatMsgCol_ = 0xff4444;
    cheatMsg_ = key.empty() ? "Enter a code!" : "Unknown: \"" + key + "\" \xE2\x80\x94 try 'help'";
    return;
  }
  if (key == "mng") { minerals_ = 7500; gas_ = 7500; }
  else if (key == "fu") {
    maxHp_ += std::max(0, 3 - hpLvl_) * 250;   // keeps Endless / armory HP
    dmgLvl_ = armorLvl_ = hpLvl_ = 3;
    bunkerHp_ = maxHp_;
    updateBunkerVisual();
  } else if (key == "endv") {
    if (mode_ != Mode::Campaign || victoryAchieved_) setStatus("Victory cheat works only in an unfinished campaign");
    else campaignVictory();
  } else if (key == "endd") { bunkerHp_ = 0; gameOver(); }
  else if (key == "korhal") { nukeCD_ = 0; airCD_ = 0; }
  else if (key == "medic") bunkerHp_ = maxHp_;
  else if (key == "terran") { hireUnit(Unit::Marine); hireUnit(Unit::Marine); }
  else if (key == "siege") dmgLvl_ = 3;
  else if (key == "bunker") {
    maxHp_ += std::max(0, 3 - hpLvl_) * 250;
    armorLvl_ = hpLvl_ = 3;
    bunkerHp_ = maxHp_;
    updateBunkerVisual();
  } else if (key == "zergrush") {
    int cap = mode_ == Mode::Campaign ? (wavePhase_ == WavePhase::Combat ? cfg::victory_wave : cfg::victory_wave - 1) : 99;
    wave_ = std::max(wave_, std::min(wave_ + 5, cap));   // campaign: stop at the final wave (it must still be fought)
    if (wavePhase_ == WavePhase::Combat) { reinforce(); startWave(); }   // between waves the pending wave++ catches up
  } else if (key == "swarm") {
    for (int i = 0; i < 10; i++) { float r = 12 + rnd() * 3, a = rnd() * 6.2832f; zerglings_.push_back(makeZergling(std::cos(a) * r, std::sin(a) * r)); }
    setStatus("The swarm has been reinforced!");
  } else if (key == "roachwave") {
    for (int i = 0; i < 5; i++) { float r = 11 + rnd() * 3, a = rnd() * 6.2832f; roaches_.push_back(makeRoach(std::cos(a) * r, std::sin(a) * r)); }
    setStatus("Roach swarm inbound!");
  } else if (key == "nuclear") {   // any wave, no cooldown; spaced so each detonates before the next launches
    doNuke(true);
    after(2.2f, [this]() { doNuke(true); });
    after(4.4f, [this]() { doNuke(true); });
  } else if (key == "airstrike") {
    for (int i = 0; i < 5; i++)
      after(i * 0.4f, [this]() {
        float tx, tz;
        randPos(tx, tz);
        strikeAt(tx, tz, 0xff8800, 5);
        spawnParts({tx, .5f, tz}, 0xff8800, 14);
      });
    after(2.0f, [this]() { ammo_ = maxAmmo_; setStatus("Air support complete!"); });
  } else if (key == "mineral") minerals_ += 500;
  else if (key == "gas") gas_ += 500;
  else if (key == "wave") {
    if (wavePhase_ != WavePhase::Combat) setStatus("Already between waves \xE2\x80\x94 next wave incoming");   // clearing again paid the bonus twice
    else {
      for (auto& e : allEnemies()) killEnemy(e, false);
      zerglings_.clear();
      roaches_.clear();
      spawnQueue_.clear();
    }
  } else if (key == "immortal") { bunkerHp_ = 99999; maxHp_ = 99999; }
  else if (key == "kaboom") {
    auto es = allEnemies();
    if (es.empty()) setStatus("No enemies to kaboom!");
    for (size_t i = 0; i < es.size(); i++) {
      EnemyP e = es[i];
      after(i * 0.18f, [this, e]() { if (!e->dead) strikeAt(e->pos.x, e->pos.z, 0xff3300, 3); });
    }
  } else if (key == "general") { kills_ += 280; score_ += 2800; }
  else if (key == "sensor") { sensorLvl_ = 3; setStatus("Sensor Tower maxed \xE2\x80\x94 blips out to 16"); }
  else if (key == "help") {
    cheatMsgCol_ = 0x44ff88;
    cheatMsg_.clear();
    for (const Cheat& x : CHEATS) {
      std::string code = x.code;
      code.resize(11, ' ');
      cheatMsg_ += code + x.desc + "\n";
    }
    logInfo("cheat codes listed in the console");
    return;   // keep the console open to read the list
  }
  cheatMsgCol_ = 0x44ff88;
  cheatMsg_ = c->desc;
  cheatCloseT_ = 1.4f;
}
