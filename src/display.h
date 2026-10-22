// Display settings: graphics card choice (hybrid laptops), window / fullscreen
// resolution, and the HUD scale that follows the window height.
#pragma once
#include <string>
#include <vector>

struct GLFWwindow;

namespace display {
struct Gpu {
  std::string name;
  bool dedicated;   // its own video memory (NVIDIA / AMD discrete) vs integrated
};
std::vector<Gpu> adapters();   // hardware adapters (DXGI); virtual / software ones are left out

struct Size { int w, h; };
std::vector<Size> windowSizes(GLFWwindow* win);       // 16:9 presets that fit the monitor's work area
std::vector<Size> fullscreenSizes(GLFWwindow* win);   // the monitor's video modes, largest first
void apply(GLFWwindow* win, bool fullscreen, Size s);  // fullscreen: that video mode at its highest refresh rate
// HUD scale: the monitor's UI scale, but never more than the height allows for the
// 1600×900 layout the HUD was designed at (so a small window or mode still fits)
float uiScaleFor(GLFWwindow* win);
bool relaunch();   // starts a fresh copy of the game (the graphics card is picked at startup)
}
