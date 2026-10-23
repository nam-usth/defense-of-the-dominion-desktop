#include "storage.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "log.h"

namespace fs = std::filesystem;

namespace storage {
static fs::path g_dir;
static Record g_prefs;   // in-memory mirror: unlocks and settings still work for this session if the disk refuses

static fs::path pathOf(const std::string& name) { return g_dir / (name + ".txt"); }

static bool writeFile(const fs::path& p, const Record& r) {
  if (g_dir.empty()) return false;
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    for (const auto& [k, v] : r) f << k << '=' << v << '\n';
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);
  return !ec;
}
static bool readFile(const fs::path& p, Record& r) {
  std::ifstream f(p);
  if (!f) return false;
  r.clear();
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    r[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return true;
}

static fs::path baseDir() {
#ifdef _WIN32
  const char* app = std::getenv("APPDATA");
  return app ? fs::path(app) : fs::current_path();
#else
  // XDG base directories: $XDG_DATA_HOME if it is an absolute path, else ~/.local/share
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && xdg[0] == '/') return fs::path(xdg);
  const char* home = std::getenv("HOME");
  return home && home[0] ? fs::path(home) / ".local" / "share" : fs::current_path();
#endif
}

std::string init(const std::string& dir) {
  g_dir = dir.empty() ? baseDir() / "DefenseOfTheDominion" : fs::path(dir);
  std::error_code ec;
  fs::create_directories(g_dir, ec);
  if (ec) { g_dir.clear(); return ""; }
  readFile(pathOf("prefs"), g_prefs);
  return g_dir.string();
}

std::string get(const std::string& key, const std::string& fallback) {
  auto it = g_prefs.find(key);
  return it == g_prefs.end() ? fallback : it->second;
}
void set(const std::string& key, const std::string& value) {
  g_prefs[key] = value;
  if (!writeFile(pathOf("prefs"), g_prefs)) logWarn("preferences could not be written to %s", g_dir.string().c_str());
}
bool writeRecord(const std::string& name, const Record& r) { return writeFile(pathOf(name), r); }
bool readRecord(const std::string& name, Record& r) { return !g_dir.empty() && readFile(pathOf(name), r); }
}  // namespace storage
