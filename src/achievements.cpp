#include "achievements.h"

#include <cstdlib>
#include <ctime>
#include <iterator>

#include "config.h"
#include "log.h"
#include "storage.h"

namespace ach {
const char* catName(Cat c) {
  switch (c) {
    case Cat::Campaign: return "CAMPAIGN";
    case Cat::Endless: return "ENDLESS";
    case Cat::Combat: return "COMBAT";
    case Cat::Command: return "COMMAND";
    default: return "FEATS";
  }
}

// In display order. Points as StarCraft II's: 10 / 20 / 30 / 50.
const std::vector<Def>& defs() {
  static const std::vector<Def> d = {
    {"boot_camp", "BOOT CAMP", "Finish the HUD tour.", Cat::Campaign, 10, "svg:help", "", 0, false},
    {"first_blood", "FIRST BLOOD", "Kill your first Zerg.", Cat::Campaign, 10, "svg:rank", "", 0, false},
    {"wave5", "HOLD THE LINE", "Clear wave 5.", Cat::Campaign, 10, "svg:campaign", "", 0, false},
    {"wave10", "DIG IN", "Clear wave 10.", Cat::Campaign, 20, "svg:campaign", "", 0, false},
    {"victory", "THE DOMINION STANDS", "Win the campaign: clear wave 15.", Cat::Campaign, 30, "svg:campaign", "", 0, false},
    {"endless20", "NO END IN SIGHT", "Clear wave 20 in Endless.", Cat::Endless, 20, "svg:endless", "", 0, false},
    {"endless25", "THE SWARM GROWS", "Clear wave 25 in Endless.", Cat::Endless, 30, "svg:endless", "", 0, false},
    {"endless30", "LEGEND OF KORHAL", "Clear wave 30 in Endless.", Cat::Endless, 50, "svg:endless", "", 0, false},
    {"kills100", "EXTERMINATOR", "Kill 100 Zerg in one game.", Cat::Combat, 10, "svg:rank", "", 0, false},
    {"general", "FOUR STARS", "Reach the rank of GENERAL in one game (280 kills).", Cat::Combat, 20, "svg:rank", "", 0, false},
    {"kills1000", "VETERAN", "Kill 1,000 Zerg in all.", Cat::Combat, 20, "svg:rank", "kills", 1000, false},
    {"kills5000", "HIVE BREAKER", "Kill 5,000 Zerg in all.", Cat::Combat, 50, "svg:rank", "kills", 5000, false},
    {"nuke20", "NUCLEAR OPTION", "Kill 20 Zerg with one nuke.", Cat::Combat, 20, "nukes_coop.png", "", 0, false},
    {"strike10", "AIR SUPERIORITY", "Kill 10 Zerg with one airstrike.", Cat::Combat, 20, "btn-progression-hornerhan-5-fleetcalldown.png", "", 0, false},
    {"grenade5", "SHRAPNEL", "Kill 5 Zerg with one Marauder grenade.", Cat::Combat, 20, "btn-upgrade-terran-explosiveshrapnelshells.png", "", 0, false},
    {"snipe50", "ONE SHOT, ONE KILL", "Kill 50 Zerg with Ghost snipes, in all.", Cat::Combat, 20, "btn-upgrade-terran-ui-lazertargetingsystem.png", "snipes", 50, false},
    {"maxed", "FULLY UPGRADED", "Max damage, armor, hull and ammo storage in one game.", Cat::Command, 20, "btn-upgrade-terran-infantryweaponslevel3.png", "", 0, false},
    {"shrike", "SHRIKE ONLINE", "Mount the Shrike turret: bunker hull level 3.", Cat::Command, 10, "btn-upgrade-terran-hisecautotracking.png", "", 0, false},
    {"overdrive", "OVERDRIVE", "Six Marines in a level 3 bunker: the Shrike in overdrive.", Cat::Command, 20, "btn-upgrade-terran-hisecautotracking.png", "", 0, false},
    {"combined", "COMBINED ARMS", "A Marine, a Marauder and a Ghost in the bunker at once.", Cat::Command, 10, "svg:roster", "", 0, false},
    {"superstim", "SUPER SOLDIERS", "Research all three Super Stimpack tiers.", Cat::Command, 20, "btn-upgrade-terran-superstimppack.png", "", 0, false},
    {"sensor3", "EYES IN THE DARK", "Research the Sensor Tower to level 3.", Cat::Command, 10, "svg:sensor", "", 0, false},
    {"untouchable", "UNTOUCHABLE", "Clear wave 5 or later with no Zerg damage to the bunker.", Cat::Feats, 20, "btn-upgrade-terran-buildingarmor.png", "", 0, false},
    {"last_stand", "LAST STAND", "Clear a wave with the bunker below 10% HP.", Cat::Feats, 20, "btn-upgrade-terran-neosteelframe.png", "", 0, true},
    {"night_watch", "NIGHT WATCH", "Clear a wave at night (Menu \xE2\x86\x92 Settings \xE2\x86\x92 day/night cycle).", Cat::Feats, 10, "svg:sensor", "", 0, false},
    {"marines_only", "BAND OF BROTHERS", "Win the campaign without hiring a Marauder or a Ghost.", Cat::Feats, 50, "svg:roster", "", 0, false},
    {"no_support", "NO AIR SUPPORT", "Win the campaign without calling an airstrike or a nuke.", Cat::Feats, 50, "svg:lock", "", 0, false},
  };
  return d;
}
const Def* find(const std::string& id) {
  for (const Def& x : defs()) if (id == x.id) return &x;
  return nullptr;
}
int Book::totalPoints() {
  int t = 0;
  for (const Def& x : defs()) t += x.points;
  return t;
}

// ── storage: "earned.<id>=<unix time>", "stat.<key>=<n>" ─────────────────────
void Book::load() {
  earned_.clear(); stats_.clear(); new_.clear(); dirty_ = false;
  storage::Record r;
  if (!storage::readRecord("achievements", r)) return;   // a new player
  for (const auto& [k, v] : r) {
    long long n = std::atoll(v.c_str());
    if (k.rfind("earned.", 0) == 0) {
      std::string id = k.substr(7);
      if (find(id)) earned_[id] = n > 0 ? n : 1;   // ids of removed achievements drop out
      else logWarn("achievements: unknown id \"%s\" in the file, dropped", id.c_str());
    } else if (k.rfind("stat.", 0) == 0) stats_[k.substr(5)] = n;
  }
  logInfo("achievements: %d of %d earned", (int)earned_.size(), (int)defs().size());
}
void Book::save() {
  storage::Record r;
  for (const auto& [id, t] : earned_) r["earned." + id] = std::to_string(t);
  for (const auto& [k, n] : stats_) r["stat." + k] = std::to_string(n);
  dirty_ = false;
  if (!storage::writeRecord("achievements", r)) logWarn("achievements: could not write achievements.txt (kept for this session)");
}
void Book::flush() { if (dirty_) save(); }

long long Book::earnedAt(const std::string& id) const {
  auto it = earned_.find(id);
  return it == earned_.end() ? 0 : it->second;
}
long long Book::stat(const std::string& key) const {
  auto it = stats_.find(key);
  return it == stats_.end() ? 0 : it->second;
}
int Book::points() const {
  int p = 0;
  for (const auto& [id, t] : earned_) if (const Def* d = find(id)) p += d->points;
  return p;
}
std::vector<std::string> Book::takeNew() {
  std::vector<std::string> v;
  v.swap(new_);
  return v;
}

// Unlocks are written at once (the toast must not be lost); stats on flush().
bool Book::unlock(const char* id) {
  if (has(id) || !find(id)) return false;
  earned_[id] = (long long)std::time(nullptr);
  new_.push_back(id);
  logInfo("achievement earned: %s", id);
  save();
  return true;
}
// A lifetime stat; earns the achievements that count it once their goal is reached
void Book::addStat(const char* key, long long n) {
  long long v = stats_[key] += n;
  dirty_ = true;
  for (const Def& d : defs())
    if (d.goal > 0 && std::string(d.stat) == key && v >= d.goal) unlock(d.id);
}

// ── rules ────────────────────────────────────────────────────────────────────
void Book::kill(const Run& r, int gameKills) {
  if (r.cheated) return;
  unlock("first_blood");
  addStat("kills", 1);
  if (gameKills >= 100) unlock("kills100");
  if (gameKills >= RANKS[std::size(RANKS) - 1].kills) unlock("general");
}
void Book::snipe(const Run& r) {
  if (r.cheated) return;
  addStat("snipes", 1);
}
void Book::multiKill(const Run& r, const char* by, int n) {
  if (r.cheated) return;
  std::string b = by;
  if (b == "nuke" && n >= 20) unlock("nuke20");
  else if (b == "strike" && n >= 10) unlock("strike10");
  else if (b == "grenade" && n >= 5) unlock("grenade5");
}
void Book::waveCleared(const Run& r, bool endless, int wave, bool hurt, float hpFrac, bool night) {
  if (r.cheated) return;
  if (wave >= 5) unlock("wave5");
  if (wave >= 10) unlock("wave10");
  if (endless && wave >= 20) unlock("endless20");
  if (endless && wave >= 25) unlock("endless25");
  if (endless && wave >= 30) unlock("endless30");
  if (wave >= 5 && !hurt) unlock("untouchable");
  if (hpFrac > 0 && hpFrac < 0.1f) unlock("last_stand");
  if (night) unlock("night_watch");
  flush();   // a wave is a checkpoint: the lifetime kills and snipes go to disk with it
}
void Book::victory(const Run& r) {
  if (r.cheated) return;
  unlock("victory");
  if (!r.hiredOther) unlock("marines_only");
  if (!r.usedSupport) unlock("no_support");
}
void Book::state(const Run& r, const State& s) {
  if (r.cheated) return;
  if (s.dmg >= 3 && s.armor >= 3 && s.hull >= 3 && s.ammo >= 3) unlock("maxed");
  if (s.hull >= 3) unlock("shrike");
  if (s.overdrive) unlock("overdrive");
  if (s.marines > 0 && s.marauders > 0 && s.ghosts > 0) unlock("combined");
  if (s.superStim >= 3) unlock("superstim");
  if (s.sensor >= 3) unlock("sensor3");
}
void Book::tourDone() { unlock("boot_camp"); }   // no game to cheat in: always counts
}  // namespace ach
