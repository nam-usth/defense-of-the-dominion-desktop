// Log to stderr and to dotd_desktop.log next to the save files (the game runs
// without a console window, so the file is where warnings end up).
#pragma once
#include <string>

void logOpen(const std::string& path);
void logInfo(const char* fmt, ...);
void logWarn(const char* fmt, ...);
void logError(const char* fmt, ...);
