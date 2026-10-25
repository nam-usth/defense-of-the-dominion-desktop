// Achievements: what the player has earned across games, the lifetime stats some of
// them count, and the rules that unlock them. Game reports what happens through the
// events below; the rules live here so they can be tested without a window. Stored
// in achievements.txt beside the settings (storage). A game in which a cheat code
// was used earns nothing and counts toward nothing.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace ach {
enum class Cat { Campaign, Endless, Combat, Command, Feats };
const char* catName(Cat c);

struct Def {
  const char* id;
  const char* name;
  const char* desc;
  Cat cat;
  int points;
  const char* icon;   // "svg:<name>" (ui::svgIcon) or a HUD image under assets/ui
  const char* stat;   // the lifetime stat it counts ("" = none)
  long long goal;     // that stat's target (0 = no progress bar)
  bool hidden;        // shown as ??? until earned
};
const std::vector<Def>& defs();
const Def* find(const std::string& id);

// This game's facts the rules need (Game keeps it, and saves it with the game)
struct Run {
  bool cheated = false;      // a cheat code was used (or legacy): this game earns nothing
  bool legacy = false;       // loaded from a save made before achievements: its past is unknown (cheated is set too)
  bool usedSupport = false;  // an airstrike or a nuke was called
  bool hiredOther = false;   // a Marauder or a Ghost was hired
};

// What the state rules look at (Game fills it)
struct State {
  int dmg = 0, armor = 0, hull = 0, ammo = 0;   // bunker upgrade levels 0-3
  int marines = 0, marauders = 0, ghosts = 0;   // in the bunker
  bool overdrive = false;                       // Shrike overdrive
  int superStim = 0, sensor = 0;                // research tiers 0-3
};

class Book {
 public:
  void load();          // from storage; a missing file is a new player
  void save();          // the disk refusing is logged (it stays in memory for this session)
  void flush();         // saves if a stat changed since the last save
  bool has(const std::string& id) const { return earned_.count(id) > 0; }
  long long earnedAt(const std::string& id) const;   // unix time, 0 = not earned
  long long stat(const std::string& key) const;
  int earnedCount() const { return (int)earned_.size(); }
  int points() const;
  static int totalPoints();
  std::vector<std::string> takeNew();   // ids earned since the last call (the toasts)

  // events
  void kill(const Run& r, int gameKills);   // a Zerg killed for its reward; gameKills: this game's total
  void snipe(const Run& r);                 // a Ghost snipe killed one
  void multiKill(const Run& r, const char* by, int n);   // "nuke" (one blast), "strike" (one airstrike), "grenade" (one burst)
  void waveCleared(const Run& r, bool endless, int wave, bool hurt, float hpFrac, bool night);
  void victory(const Run& r);
  void state(const Run& r, const State& s);
  void tourDone();

 private:
  bool unlock(const char* id);
  void addStat(const char* key, long long n);
  std::map<std::string, long long> earned_;   // id → unix time
  std::map<std::string, long long> stats_;
  std::vector<std::string> new_;
  bool dirty_ = false;
};
}  // namespace ach
