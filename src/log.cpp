#include "log.h"

#include <cstdarg>
#include <cstdio>

static FILE* g_log = nullptr;

void logOpen(const std::string& path) {
  if (g_log) fclose(g_log);
  g_log = fopen(path.c_str(), "w");
}

static void vlog(const char* level, const char* fmt, va_list ap) {
  char buf[8192];
  vsnprintf(buf, sizeof buf, fmt, ap);
  fprintf(stderr, "[%s] %s\n", level, buf);
  if (g_log) {
    fprintf(g_log, "[%s] %s\n", level, buf);
    fflush(g_log);
  }
}
void logInfo(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog("info", fmt, ap); va_end(ap); }
void logWarn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog("warn", fmt, ap); va_end(ap); }
void logError(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog("error", fmt, ap); va_end(ap); }
