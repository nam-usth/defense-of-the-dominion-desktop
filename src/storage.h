// Persistent storage in %APPDATA%\DefenseOfTheDominion (Linux: $XDG_DATA_HOME, else
// ~/.local/share/DefenseOfTheDominion) — the desktop stand-in for the web version's
// localStorage: preferences (volume, unlocks, records) and the two save slots, as
// "key=value" text files.
#pragma once
#include <map>
#include <string>

namespace storage {
using Record = std::map<std::string, std::string>;
// creates the folder (dir, else the one above); returns its path ("" if unavailable: values then last for this session only)
std::string init(const std::string& dir = "");
std::string get(const std::string& key, const std::string& fallback = "");
void set(const std::string& key, const std::string& value);
bool writeRecord(const std::string& name, const Record& r);   // whole file, written atomically
bool readRecord(const std::string& name, Record& r);          // false: missing or unreadable
}
