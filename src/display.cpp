#include "display.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

#include "log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#endif

namespace display {

std::vector<Gpu> adapters() {
  std::vector<Gpu> out;
#ifdef _WIN32
  IDXGIFactory1* f = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) { logWarn("display: DXGI unavailable — graphics cards not listed"); return out; }
  IDXGIAdapter1* a = nullptr;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
    DXGI_ADAPTER_DESC1 d;
    if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      // NVIDIA / AMD with their own memory are dedicated; Intel and virtual adapters are not listed as such
      bool known = d.VendorId == 0x10DE || d.VendorId == 0x1002 || d.VendorId == 0x8086;
      if (known) {
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        bool dedicated = d.VendorId != 0x8086 && d.DedicatedVideoMemory >= (512ull << 20);
        out.push_back({name, dedicated});
      }
    }
    a->Release();
  }
  f->Release();
#endif
  return out;
}

const char* noGpuChoiceNote() {
#ifdef _WIN32
  return "Only one graphics card found \xE2\x80\x94 nothing to switch.";
#else
  // Linux: the driver stack picks the card from the environment the game starts in
  return "On Linux the card is chosen when the game starts: DRI_PRIME=1 (Mesa) or prime-run (NVIDIA) for the dedicated one.";
#endif
}

static GLFWmonitor* monitorOf(GLFWwindow* win) {
  if (GLFWmonitor* m = glfwGetWindowMonitor(win)) return m;   // fullscreen
  if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) return glfwGetPrimaryMonitor();   // a Wayland window is not told where it is
  int x, y, w, h, n;
  glfwGetWindowPos(win, &x, &y);
  glfwGetWindowSize(win, &w, &h);
  GLFWmonitor** ms = glfwGetMonitors(&n);
  for (int i = 0; i < n; i++) {   // the monitor under the window's centre
    int mx, my, mw, mh;
    glfwGetMonitorWorkarea(ms[i], &mx, &my, &mw, &mh);
    int cx = x + w / 2, cy = y + h / 2;
    if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) return ms[i];
  }
  return glfwGetPrimaryMonitor();
}

std::vector<Size> windowSizes(GLFWwindow* win) {
  static const Size presets[] = {{1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2048, 1152}, {2560, 1440}, {2800, 1575}, {3200, 1800}, {3840, 2160}};
  int x = 0, y = 0, w = 1920, h = 1080;
  if (GLFWmonitor* m = monitorOf(win)) glfwGetMonitorWorkarea(m, &x, &y, &w, &h);
  std::vector<Size> out;
  for (const Size& s : presets)
    if (s.w <= w && s.h <= h - 40) out.push_back(s);   // room for the title bar
  if (out.empty()) out.push_back({std::min(w, 1280), std::min(h - 40, 720)});
  return out;
}

std::vector<Size> fullscreenSizes(GLFWwindow* win) {
  std::vector<Size> out;
  int n = 0;
  GLFWmonitor* m = monitorOf(win);
  const GLFWvidmode* modes = m ? glfwGetVideoModes(m, &n) : nullptr;
  for (int i = 0; i < n; i++) {
    Size s{modes[i].width, modes[i].height};
    if (s.w < 1024 || s.h < 576) continue;
    if (std::none_of(out.begin(), out.end(), [&](const Size& o) { return o.w == s.w && o.h == s.h; })) out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const Size& a, const Size& b) { return a.w * a.h > b.w * b.h; });
  return out;
}

void apply(GLFWwindow* win, bool fullscreen, Size s) {
  GLFWmonitor* m = monitorOf(win);
  if (fullscreen && m) {
    int n = 0, refresh = GLFW_DONT_CARE;
    const GLFWvidmode* modes = glfwGetVideoModes(m, &n);
    for (int i = 0; i < n; i++)
      if (modes[i].width == s.w && modes[i].height == s.h) refresh = std::max(refresh, modes[i].refreshRate);
    glfwSetWindowMonitor(win, m, 0, 0, s.w, s.h, refresh);
    logInfo("display: fullscreen %dx%d @ %d Hz", s.w, s.h, refresh);
  } else {
    int x = 0, y = 0, w = s.w, h = s.h;
    if (m) glfwGetMonitorWorkarea(m, &x, &y, &w, &h);
    glfwSetWindowMonitor(win, nullptr, x + std::max(0, (w - s.w) / 2), y + std::max(30, (h - s.h) / 2), s.w, s.h, 0);
    logInfo("display: window %dx%d", s.w, s.h);
  }
}

float uiScaleFor(GLFWwindow* win) {
  float xs = 1, ys = 1;
  glfwGetWindowContentScale(win, &xs, &ys);
  int w, h;
  glfwGetWindowSize(win, &w, &h);
  float s = std::min(xs, h / 900.f);
  return std::round(std::clamp(s, 0.75f, 3.f) * 20) / 20;   // steps of 0.05: no font rebuild for a pixel
}

bool relaunch() {
#ifdef _WIN32
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi = {};
  wchar_t cmd[MAX_PATH + 2];
  swprintf(cmd, MAX_PATH + 2, L"\"%ls\"", exe);
  if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    logError("display: could not restart the game (error %lu)", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  return false;
#endif
}
}  // namespace display
