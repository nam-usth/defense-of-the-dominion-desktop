// --sim: whole games played by a bot on the real game code, no frames drawn, thousands of times faster than a player.
// The balance is tuned against what comes out: how far each kind of bunker gets, what it spends, what does the killing.
// The bot clicks at a player's pace at the nearest Zerg it can see, spends everything as soon as it can, in a fixed order,
// nukes a crowd and calls the Viking when out of ammo. It is no expert: it compares bunkers, it does not predict scores.
#include <algorithm>
#include <cstdio>
#include <string>

#include "audio.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "storage.h"

bool Game::simSet(const std::string& assignments) {
  static const struct { const char* name; float* v; } NUM[] = {
    {"zcA", &bal::zcA}, {"zcB", &bal::zcB}, {"zcC", &bal::zcC}, {"zcLate", &bal::zcLate}, {"rcK", &bal::rcK}, {"swarmCount", &bal::swarmCount}, {"swarmHp", &bal::swarmHp},
    {"lingHp0", &bal::lingHp0}, {"lingHpW", &bal::lingHpW}, {"lingHpLate", &bal::lingHpLate}, {"lingDmg0", &bal::lingDmg0}, {"lingDmgW", &bal::lingDmgW}, {"lingSpeedMax", &bal::lingSpeedMax},
    {"lingMin0", &bal::lingMin0}, {"lingMinW", &bal::lingMinW}, {"lingGas0", &bal::lingGas0}, {"lingGasW", &bal::lingGasW},
    {"roachHp0", &bal::roachHp0}, {"roachHpW", &bal::roachHpW}, {"roachDmg0", &bal::roachDmg0}, {"roachDmgW", &bal::roachDmgW}, {"roachSpeedMax", &bal::roachSpeedMax},
    {"roachMin0", &bal::roachMin0}, {"roachMinW", &bal::roachMinW}, {"roachGas0", &bal::roachGas0}, {"roachGasW", &bal::roachGasW},
    {"clearM0", &bal::clearM0}, {"clearMW", &bal::clearMW}, {"clearG0", &bal::clearG0}, {"clearGW", &bal::clearGW}, {"nightPay", &bal::nightPay},
    {"marineMult", &bal::marineMult}, {"marauderMult", &bal::marauderMult}, {"marauderBonus", &bal::marauderBonus}, {"ghostMult", &bal::ghostMult}, {"ghostBonus", &bal::ghostBonus},
    {"mgSplash", &bal::mgSplash}, {"mgInterval", &bal::mgInterval}, {"snipeMult", &bal::snipeMult}, {"snipeAmmo", &bal::snipeAmmo},
    {"mgSlow0", &bal::mgSlow0}, {"mgSlow1", &bal::mgSlow1}, {"mgSlowDur0", &bal::mgSlowDur0}, {"mgSlowDur1", &bal::mgSlowDur1}, {"lateGrow", &bal::lateGrow},
    {"baseAmmo", &bal::baseAmmo}, {"nukeM0", &bal::nukeM0}, {"nukeMW", &bal::nukeMW}, {"nukeG", &bal::nukeG}, {"nukeGrow", &bal::nukeGrow},
  };
  size_t at = 0;
  while (at < assignments.size()) {
    const size_t comma = std::min(assignments.find(',', at), assignments.size()), eq = assignments.find('=', at);
    if (eq == std::string::npos || eq > comma) { fprintf(stderr, "--sim-set: NAME=VALUE,...\n"); return false; }
    const std::string name = assignments.substr(at, eq - at);
    float* v = nullptr;
    for (const auto& n : NUM) if (name == n.name) v = n.v;
    if (!v) { fprintf(stderr, "--sim-set: unknown name %s\n", name.c_str()); return false; }
    *v = std::strtof(assignments.c_str() + eq + 1, nullptr);
    at = comma + 1;
  }
  return true;
}

namespace {
struct Plan {
  const char* name;
  int marines, marauders, ghosts;
};
const Plan PLANS[] = {
  {"marine", 6, 0, 0},
  {"marauder", 0, 3, 0},
  {"ghost", 0, 0, 3},
  {"mix", 2, 1, 1},
};
const float CLICKS_PER_S = 6;
}  // namespace

// What the bunker is made of, one hire or swap at a time: Marines go when the plan has no room for them
void Game::simHire(int marines, int marauders, int ghosts) {
  auto count = [&](Unit u) { return (int)std::count(roster_.begin(), roster_.end(), u); };
  const struct { Unit u; int want; } wants[3] = {{Unit::Ghost, ghosts}, {Unit::Marauder, marauders}, {Unit::Marine, marines}};
  for (const auto& w : wants) {
    while (count(w.u) < w.want) {
      const UnitDef& d = unitDef(w.u);
      if (minerals_ < d.cost[0] || gas_ < d.cost[1]) return;
      while (slotsLeft() < d.slots) {   // over the plan's number of something else (the starting Marines)
        int drop = -1;
        for (int i = 0; i < (int)roster_.size() && drop < 0; i++) {
          const int have = count(roster_[i]), want = roster_[i] == Unit::Marine ? marines : roster_[i] == Unit::Marauder ? marauders : ghosts;
          if (have > want) drop = i;
        }
        if (drop < 0) return;
        dismissUnit(drop);
      }
      hireUnit(w.u);
    }
  }
}

void Game::simShop(int planIdx) {
  const Plan& P = PLANS[planIdx];
  if (wave_ < 4) simHire(6, 0, 0);   // cheap Marines first, the plan's bunker once the money comes in
  else simHire(P.marines, P.marauders, P.ghosts);
  auto afford = [&](const int* c) { return minerals_ >= c[0] && gas_ >= c[1]; };
  auto level = [&](int& lvl, const int (*costs)[2], const char* type) {
    if (lvl < 3 && afford(costs[lvl])) { buy(type); return true; }
    return false;
  };
  // the unit's research: first the unlock, then its tiers
  auto research = [&](const char* key, bool unlocked, int lvl, int c0m, int c0g, const int (*tiers)[2]) {
    const bool has = key[0] == 's' && key[1] == 'u' ? P.marines + P.marauders > 0 : key[0] == 'c' ? P.marauders > 0 : P.ghosts > 0;
    if (!has || wave_ < 4) return false;
    if (!unlocked) { if (minerals_ >= c0m && gas_ >= c0g) { researchUpgrade(key, -1); return true; } return false; }
    if (lvl < 3 && afford(tiers[lvl])) { researchUpgrade(key, lvl); return true; }
    return false;
  };
  static const int STIM[3][2] = {{200, 80}, {320, 130}, {500, 200}}, CONC[3][2] = {{250, 80}, {400, 130}, {600, 200}}, SNIPE[3][2] = {{200, 80}, {320, 130}, {500, 200}};
  for (int guard = 0; guard < 40; guard++) {
    bool bought = false;
    if (dmgLvl_ < 1) bought = level(dmgLvl_, cfg::dmg_costs, "dmg");
    if (!bought && ammoLvl_ < 1) bought = level(ammoLvl_, cfg::ammo_costs, "ammo");
    if (!bought && !concussiveUnlocked_) bought = research("concussive", false, 0, 300, 75, CONC);
    if (!bought && !snipeUnlocked_) bought = research("snipe", false, 0, 250, 100, SNIPE);
    if (!bought && hpLvl_ < 1) bought = level(hpLvl_, cfg::hp_costs, "hp");
    if (!bought && dmgLvl_ < 2) bought = level(dmgLvl_, cfg::dmg_costs, "dmg");
    if (!bought && armorLvl_ < 1) bought = level(armorLvl_, cfg::armor_costs, "armor");
    if (!bought && theatre_ == Theatre::DayNight && sensorLvl_ < 1 && wave_ >= 4 && afford(cfg::sensor_costs[0])) { researchUpgrade("sensor", -1); bought = true; }
    if (!bought) bought = level(hpLvl_, cfg::hp_costs, "hp");
    if (!bought) bought = level(dmgLvl_, cfg::dmg_costs, "dmg");
    if (!bought) bought = level(ammoLvl_, cfg::ammo_costs, "ammo");
    if (!bought) bought = research("concussive", concussiveUnlocked_, concussiveLvl_, 300, 75, CONC);
    if (!bought) bought = research("snipe", snipeUnlocked_, snipeLvl_, 250, 100, SNIPE);
    if (!bought && P.marines >= 1) bought = research("superstim", superStimUnlocked_, superStimLvl_, 250, 150, STIM);
    if (!bought) bought = level(armorLvl_, cfg::armor_costs, "armor");
    if (!bought) {   // the Armory: whichever of its lines is cheapest now, keeping a nuke's price back
      int best = -1, bm = 0, bg = 0;
      const bool keep = wave_ + 1 >= cfg::nuke_wave;
      const float reserve = keep ? (float)nukeCostM() : 0.f, reserveG = keep ? (float)nukeCostG() : 0.f;
      for (int k = 0; k < 4; k++) {
        int m, g;
        armoryCost(k, m, g);
        if (!armoryMaxed(k) && minerals_ - reserve >= m && gas_ - reserveG >= g && (best < 0 || m + 2 * g < bm + 2 * bg)) { best = k; bm = m; bg = g; }
      }
      if (best >= 0) { buyArmory(best); bought = true; }
    }
    if (!bought) break;
  }
  if (superStimUnlocked_) stimAuto_ = true;
}

void Game::simBotStep(float dt) {
  simClick_ -= dt;
  const auto all = allEnemies();
  int alive = 0;
  EnemyP nearest;
  float nd = 1e9f;
  for (const EnemyP& e : all) {
    if (e->dead) continue;
    alive++;
    if (!e->visible && !sensed(*e)) continue;
    const float d = std::hypot(e->pos.x, e->pos.z);
    if (d < nd) { nd = d; nearest = e; }
  }
  if (wave_ >= cfg::nuke_wave && nukeCD_ <= 0 && !nukeInFlight_ && minerals_ >= nukeCostM() && gas_ >= nukeCostG() && (alive >= 10 + wave_ / 2 || (alive > 0 && bunkerHp_ < 0.4f * maxHp_))) doNuke(false);
  if (wavePhase_ != WavePhase::Combat) return;
  if (ammo_ <= 0) { if (airCD_ <= 0 && !vikingActive_ && alive > 0) doAirstrike(); return; }
  if (!nearest || simClick_ > 0 || shootCD_ > 0) return;
  simClick_ = 1 / CLICKS_PER_S;
  const float j = 0.25f;   // a player's aim is not perfect
  shoot(nearest->pos.x + (rnd() - 0.5f) * j, nearest->pos.z + (rnd() - 0.5f) * j);
}

// games: per plan, on seeds 1..games; waveCap: a game that gets this far stops there
void Game::simRun(const std::string& onlyPlan, int games, int waveCap, const std::string& out) {
  FILE* f = out.empty() ? nullptr : fopen(out.c_str(), "w");
  if (!out.empty() && !f) { logError("sim: cannot write %s", out.c_str()); return; }
  if (f) fprintf(f, "plan,seed,wave,t,hp,maxHp,ammo,maxAmmo,minerals,gas,spentM,spentG,alive,dmgMarine,dmgMarauder,dmgGhost,dmgSnipe,dmgShrike,killsStrike,killsNuke,nukes,strikes\n");
  audio::setMuted(true);
  gpuFxOn_ = false;
  dmgNumbersOn_ = false;
  tutRun_ = false;
  const float dt = 1.f / 60;
  for (int pi = 0; pi < (int)(sizeof PLANS / sizeof PLANS[0]); pi++) {
    if (!onlyPlan.empty() && onlyPlan != PLANS[pi].name) continue;
    std::vector<int> reached;
    for (int seed = 1; seed <= games; seed++) {
      rng_.seed((unsigned)seed * 7919u + pi);
      storage::set("endless_unlocked", "1");
      start(Mode::Campaign);
      simClick_ = 0;
      for (float& d : simDmg_) d = 0;
      simStrikeKills_ = simNukeKills_ = simNukes_ = simStrikes_ = 0;
      simHurt_[0] = simHurt_[1] = 0;
      int lastWave = 0;
      float waveT0 = gameT_;
      double spentM0 = 0, spentG0 = 0;
      long steps = 0;
      const long maxSteps = (long)(60 * 60 * 90);   // 90 minutes of game: a stuck run must end
      while (steps++ < maxSteps) {
        if (phase_ == Phase::Victory) continueEndless();
        if (phase_ == Phase::GameOver) break;
        if (wave_ > waveCap) break;
        if (wavePhase_ != WavePhase::Combat && lastWave != wave_) {   // the break after a wave: its line, then the shopping
          lastWave = wave_;
          if (f) fprintf(f, "%s,%d,%d,%.1f,%.0f,%.0f,%d,%d,%.0f,%.0f,%.0f,%.0f,%d,%.0f,%.0f,%.0f,%.0f,%.0f,%d,%d,%d,%d\n", PLANS[pi].name, seed, wave_, gameT_ - waveT0, bunkerHp_, maxHp_, ammo_, maxAmmo_,
                         minerals_, gas_, spentM0, spentG0, (int)(zerglings_.size() + roaches_.size()), simDmg_[0], simDmg_[1], simDmg_[2], simDmg_[3], simDmg_[4], simStrikeKills_, simNukeKills_, simNukes_, simStrikes_);
          waveT0 = gameT_;
          simHurt_[0] = simHurt_[1] = 0;
          const float m0 = minerals_, g0 = gas_;
          simShop(pi);
          spentM0 += m0 - minerals_;
          spentG0 += g0 - gas_;
        }
        if (wave_ == 1 && lastWave == 0 && steps == 1) simShop(pi);
        simBotStep(dt);
        update(dt);
        if (steps % 600 == 0) { glow_.clear(); smoke_.clear(); }
      }
      if (steps >= maxSteps) logWarn("sim: %s seed %d hit the step limit at wave %d", PLANS[pi].name, seed, wave_);
      if (phase_ == Phase::GameOver)
        logInfo("sim %s seed %d fell %.1f s into wave %d: max HP %.0f, this wave bites %.0f acid %.0f; %d Zerglings %d Roaches alive, %d to come; ammo %d/%d, nuke cd %.1f, price %d, minerals %.0f",
                PLANS[pi].name, seed, gameT_ - waveT0, wave_, maxHp_, simHurt_[0], simHurt_[1], (int)zerglings_.size(), (int)roaches_.size(), (int)spawnQueue_.size(), ammo_, maxAmmo_, nukeCD_, nukeCostM(), minerals_);
      reached.push_back(wave_);
      logInfo("sim %s seed %d: wave %d, %.0f s, score %d, %.0f+%.0f spent, dmg M %.0f Ma %.0f G %.0f snipe %.0f shrike %.0f, kills strike %d nuke %d", PLANS[pi].name, seed, wave_, gameT_, score_, spentM0, spentG0,
              simDmg_[0], simDmg_[1], simDmg_[2], simDmg_[3], simDmg_[4], simStrikeKills_, simNukeKills_);
    }
    std::sort(reached.begin(), reached.end());
    std::string line = std::string("sim ") + PLANS[pi].name + ": median wave " + std::to_string(reached[reached.size() / 2]) + " (";
    for (int w : reached) line += std::to_string(w) + " ";
    logInfo("%s)", line.c_str());
    printf("%s)\n", line.c_str());
    fflush(stdout);
  }
  if (f) fclose(f);
}
