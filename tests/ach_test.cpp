// Unit test of the achievement rules and their storage (no window, no GL).
//   powershell -ExecutionPolicy Bypass -File build.ps1 -Test   (or: ninja build/ach_test.exe && build/ach_test.exe)
// Uses a fresh folder of its own under the system temp directory; prints one line per check.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "achievements.h"
#include "storage.h"

static int g_pass = 0, g_fail = 0;
static void check(const char* what, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  (ok ? g_pass : g_fail)++;
}

int main() {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / ("dotd_ach_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));   // two runs never share it
  fs::remove_all(dir);
  check("storage folder created", !storage::init(dir.string()).empty());

  ach::Book b;
  b.load();
  check("a new player has nothing", b.earnedCount() == 0 && b.points() == 0);
  check("27 achievements, 600 points, unique ids", [] {
    int n = 0;
    for (const ach::Def& d : ach::defs()) { n++; if (ach::find(d.id) != &d) return false; }
    return n == 27 && ach::Book::totalPoints() == 600;
  }());

  ach::Run cheat;  cheat.cheated = true;
  ach::Run clean;
  // a cheated game earns and counts nothing
  for (int i = 1; i <= 300; i++) b.kill(cheat, i);
  b.waveCleared(cheat, true, 30, false, 0.05f, true);
  b.victory(cheat);
  b.multiKill(cheat, "nuke", 40);
  b.state(cheat, ach::State{3, 3, 3, 3, 1, 1, 1, true, 3, 3});
  for (int i = 0; i < 60; i++) b.snipe(cheat);
  b.tourDone();   // (Game does not call it in a cheated game: here it shows the book itself has no gate on it)
  check("cheated game: nothing earned, no stats (BOOT CAMP aside, called directly)", b.earnedCount() == 1 && b.has("boot_camp") && b.stat("kills") == 0 && b.stat("snipes") == 0 && !b.has("snipe50"));
  b.takeNew();

  // kills
  b.kill(clean, 1);
  check("first kill: FIRST BLOOD", b.has("first_blood") && b.stat("kills") == 1);
  for (int i = 2; i <= 99; i++) b.kill(clean, i);
  check("99 kills in a game: no EXTERMINATOR yet", !b.has("kills100"));
  b.kill(clean, 100);
  check("100 kills in a game: EXTERMINATOR", b.has("kills100"));
  for (int i = 101; i <= 279; i++) b.kill(clean, i);
  check("279 kills: not GENERAL", !b.has("general"));
  b.kill(clean, 280);
  check("280 kills: FOUR STARS (GENERAL)", b.has("general"));
  for (int i = 281; i <= 1000; i++) b.kill(clean, i);
  check("1,000 lifetime kills: VETERAN", b.has("kills1000") && b.stat("kills") == 1000 && !b.has("kills5000"));

  // multi-kills: thresholds per weapon, not mixed up
  b.multiKill(clean, "nuke", 19);  b.multiKill(clean, "strike", 9);  b.multiKill(clean, "grenade", 4);
  check("19 / 9 / 4 in one blast: none", !b.has("nuke20") && !b.has("strike10") && !b.has("grenade5"));
  b.multiKill(clean, "grenade", 20);
  check("20 by a grenade earns SHRAPNEL only", b.has("grenade5") && !b.has("nuke20") && !b.has("strike10"));
  b.multiKill(clean, "nuke", 20);  b.multiKill(clean, "strike", 10);
  check("20 by a nuke, 10 by an airstrike", b.has("nuke20") && b.has("strike10"));

  // snipes count across games
  for (int i = 0; i < 49; i++) b.snipe(clean);
  check("49 snipes: not yet", !b.has("snipe50"));
  b.snipe(clean);
  check("50 snipes: ONE SHOT, ONE KILL", b.has("snipe50"));

  // waves
  b.waveCleared(clean, false, 4, true, 0.5f, false);
  check("wave 4 cleared while hurt: nothing", !b.has("wave5") && !b.has("untouchable"));
  b.waveCleared(clean, false, 4, false, 0.5f, false);
  check("wave 4 unhurt: no UNTOUCHABLE (from wave 5)", !b.has("untouchable"));
  b.waveCleared(clean, false, 5, true, 0.5f, false);
  check("wave 5 hurt: HOLD THE LINE, no UNTOUCHABLE", b.has("wave5") && !b.has("untouchable"));
  b.waveCleared(clean, false, 6, false, 0.5f, false);
  check("wave 6 unhurt: UNTOUCHABLE", b.has("untouchable"));
  b.waveCleared(clean, false, 15, true, 0.08f, false);
  check("campaign wave 15: DIG IN, LAST STAND under 10%, no Endless ones", b.has("wave10") && b.has("last_stand") && !b.has("endless20"));
  b.waveCleared(clean, true, 21, true, 0.5f, true);
  check("Endless wave 21 at night: NO END IN SIGHT, NIGHT WATCH", b.has("endless20") && b.has("night_watch") && !b.has("endless25"));
  b.waveCleared(clean, true, 30, true, 0.5f, false);
  check("Endless wave 30: 25 and 30", b.has("endless25") && b.has("endless30"));

  // victory conditions
  ach::Run used;  used.usedSupport = true;  used.hiredOther = true;
  b.victory(used);
  check("win with support and other units: only THE DOMINION STANDS", b.has("victory") && !b.has("marines_only") && !b.has("no_support"));
  b.victory(clean);
  check("clean win: BAND OF BROTHERS and NO AIR SUPPORT", b.has("marines_only") && b.has("no_support"));

  // state
  b.state(clean, ach::State{3, 3, 2, 3, 2, 0, 0, false, 2, 2});
  check("hull 2: no FULLY UPGRADED / SHRIKE", !b.has("maxed") && !b.has("shrike"));
  b.state(clean, ach::State{3, 3, 3, 3, 6, 0, 0, true, 3, 3});
  check("all maxed + overdrive + research", b.has("maxed") && b.has("shrike") && b.has("overdrive") && b.has("superstim") && b.has("sensor3"));
  check("no COMBINED ARMS without all three units", !b.has("combined"));
  b.state(clean, ach::State{0, 0, 0, 0, 1, 1, 1, false, 0, 0});
  check("Marine + Marauder + Ghost: COMBINED ARMS", b.has("combined"));


  // toasts: each new one once, in order
  auto n = b.takeNew();
  check("takeNew returns every unlock since the last call once", (int)n.size() == b.earnedCount() - 1 && n.front() == "first_blood" && b.takeNew().empty());
  check("re-earning does not toast again", (b.kill(clean, 5), b.takeNew().empty()));

  // persistence round trip
  b.flush();
  ach::Book c;
  c.load();
  check("reload: same achievements, points and stats", c.earnedCount() == b.earnedCount() && c.points() == b.points() && c.stat("kills") == b.stat("kills")
                                                         && c.stat("snipes") == 50 && c.earnedAt("first_blood") > 0);
  check("reload: nothing new to toast", c.takeNew().empty());
  check("every achievement but HIVE BREAKER earned", c.earnedCount() == 26 && !c.has("kills5000"));

  // an unknown id in the file is dropped, not counted
  {
    storage::Record r;
    storage::readRecord("achievements", r);
    r["earned.removed_one"] = "123";
    storage::writeRecord("achievements", r);
    ach::Book d;
    d.load();
    check("unknown id in the file is dropped", d.earnedCount() == 26 && !d.has("removed_one"));
  }
  {   // an earned id with no time still counts as earned
    storage::Record r;
    storage::readRecord("achievements", r);
    r["earned.first_blood"] = "";
    storage::writeRecord("achievements", r);
    ach::Book e;
    e.load();
    check("earned id without a time: earned, at 1", e.has("first_blood") && e.earnedAt("first_blood") == 1);
  }

  fs::remove_all(dir);
  std::printf("\n%d/%d passed\n", g_pass, g_pass + g_fail);
  return g_fail ? 1 : 0;
}
