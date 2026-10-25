// Defense of the Dominion — desktop edition (C++ / OpenGL 3.3).
// Window, GL context, Dear ImGui, the frame loop and the asset loader pacing.
//
// Test options (for checking a build without playing):
//   --size 1600x900        window size
//   --start campaign       start a game once every asset has loaded (or "endless")
//   --cheat CODE[@S]       run a cheat code right after --start, or S seconds after it (repeatable);
//                          also hire:marine|marauder|ghost and research:KEY[:TIER] (paid for: --cheat mng first, or
//                          grant:M,G — test money, not a cheat under --data; without --data it turns achievements off)
//   --key K@S              press key K (a letter, a digit, enter, space, esc, left, right, up, down) S seconds after --start (repeatable)
//   --open roster|menu|cheat   open a panel after --start
//   --zoom-in N            press "+" N times after --start
//   --shot FILE.png        save a screenshot --after seconds later, then exit
//   --after SECONDS        default 3
//   --perf                 show the performance overlay (F3) and log its last reading on exit
//   --no-vsync             uncapped frame rate (to measure)
//   --threads N            worker threads (default: min(4, hardware threads − 1); 0 = single-threaded)
//   --bench-fire           test load: fire every volley at the nearest enemy (magazine refilled; without --data: no achievements)
//   --cpu-particles        this run without the GPU particles (the stored setting is kept)
//   --ui-scale S           override the monitor's UI scale
//   --gpu dedicated|integrated   graphics card for this run (the menu stores the choice; Windows only)
//   --platform x11|wayland  Linux: the windowing system (default: X11 when $DISPLAY is set, else Wayland)
//   --resize WxH           change the window size after --start (tests the HUD rescale)
//   --open menu:TAB|tour|howto|achievements[:N]   a menu tab (game, settings, display, controls, tutorial, achievements, cheats),
//                          the HUD tour, HOW TO PLAY, the title's achievement list (N: its category tab, 0-4)
//   --data DIR             settings and saves in DIR instead of %APPDATA% / ~/.local/share (a new player; the real ones untouched)
//   --no-tutorial          this run without the HUD tour and hints (the stored setting is kept)
//   --dn SECONDS           day/night cycle on, SECONDS into it (78 dusk, 102 night, 156 dawn)
//   --hold X,Y,S           1 s after --start, hold the left button at (X, Y) (fractions of the window) for S s;
//                          logs the volleys fired (S = 0: a single click)
//   --post on|off          HDR post-processing (bloom, tone curve); off: the scene draws straight to a 4x MSAA window
//   --weather STATE        clear | overcast | rain (a downpour, puddles full) | drizzle | after (a shower just over:
//                          puddles full, the sun out), at once and held for the run (after --start)
#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "assets.h"
#include "audio.h"
#include "display.h"
#include "game.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "jobs.h"
#include "log.h"
#include "render.h"
#include "storage.h"
#include "ui.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Hybrid-graphics laptops: ask the NVIDIA (Optimus) / AMD (PowerXpress) driver for
// the discrete GPU — without these exports Windows runs the game on the integrated one
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

static Game* g_game = nullptr;

static void fatal(const char* msg) {
  logError("%s", msg);
#ifdef _WIN32
  MessageBoxA(nullptr, msg, "Defense of the Dominion", MB_ICONERROR | MB_OK);
#endif
}

static void keyCb(GLFWwindow*, int key, int, int action, int mods) { if (g_game) g_game->onKey(key, action, mods); }
static void charCb(GLFWwindow*, unsigned c) { if (g_game) g_game->onChar(c); }
static void mouseCb(GLFWwindow*, int b, int action, int) { if (g_game) g_game->onMouseButton(b, action); }
static void scrollCb(GLFWwindow*, double, double dy) { if (g_game) g_game->onScroll(dy); }
static void iconifyCb(GLFWwindow*, int iconified) { audio::setSuspended(iconified != 0); }

int main(int argc, char** argv) {
  int winW = 0, winH = 0;
  float uiScaleArg = 0;
  bool perf = false, vsync = true, benchFire = false, cpuParticles = false;
  std::string gpuArg, platformArg;
  int resizeW = 0, resizeH = 0;
  int threads = -1;
  std::string shotPath, startMode, openPanel, dataDir, weatherArg;
  std::vector<std::string> cheats;
  double shotAfter = 3;
  int zoomIn = 0;
  bool noTutorial = false, postOn = true;
  float dnAt = -1, holdX = 0, holdY = 0, holdFor = -1;
  struct Later { double at; std::string cheat; int key; bool done; };   // --cheat CODE@S, --key K@S
  std::vector<Later> later;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--size") { std::string v = next(); sscanf(v.c_str(), "%dx%d", &winW, &winH); }
    else if (a == "--ui-scale") uiScaleArg = std::strtof(next().c_str(), nullptr);
    else if (a == "--perf") perf = true;
    else if (a == "--bench-fire") benchFire = true;
    else if (a == "--cpu-particles") cpuParticles = true;
    else if (a == "--gpu") gpuArg = next();
    else if (a == "--platform") platformArg = next();
    else if (a == "--resize") { std::string v = next(); sscanf(v.c_str(), "%dx%d", &resizeW, &resizeH); }
    else if (a == "--threads") threads = std::atoi(next().c_str());
    else if (a == "--no-vsync") vsync = false;
    else if (a == "--start") startMode = next();
    else if (a == "--cheat" || a == "--key") {
      std::string v = next();
      size_t at = v.find('@');
      if (at == std::string::npos && a == "--cheat") cheats.push_back(v);
      else if (at == std::string::npos) { fprintf(stderr, "%s wants %s\n", a.c_str(), a == "--key" ? "K@S" : "CODE@S"); return 1; }
      else {
        std::string n = v.substr(0, at);
        char k = n.size() == 1 ? (char)std::toupper((unsigned char)n[0]) : 0;
        int key = a != "--key" ? 0 : k >= 'A' && k <= 'Z' ? GLFW_KEY_A + (k - 'A') : k >= '0' && k <= '9' ? GLFW_KEY_0 + (k - '0')
                : n == "enter" ? GLFW_KEY_ENTER : n == "space" ? GLFW_KEY_SPACE : n == "esc" ? GLFW_KEY_ESCAPE : n == "left" ? GLFW_KEY_LEFT
                : n == "right" ? GLFW_KEY_RIGHT : n == "up" ? GLFW_KEY_UP : n == "down" ? GLFW_KEY_DOWN : -1;
        if (key < 0) { fprintf(stderr, "--key: a letter, a digit, enter, space, esc, left, right, up or down\n"); return 1; }
        later.push_back({std::atof(v.c_str() + at + 1), a == "--cheat" ? v.substr(0, at) : "", key, false});
      }
    }
    else if (a == "--open") openPanel = next();
    else if (a == "--zoom-in") zoomIn = std::atoi(next().c_str());
    else if (a == "--shot") shotPath = next();
    else if (a == "--after") shotAfter = std::atof(next().c_str());
    else if (a == "--data") dataDir = next();
    else if (a == "--no-tutorial") noTutorial = true;
    else if (a == "--post") postOn = next() != "off";
    else if (a == "--dn") dnAt = std::strtof(next().c_str(), nullptr);
    else if (a == "--weather") {
      weatherArg = next();
      if (weatherArg != "clear" && weatherArg != "overcast" && weatherArg != "rain" && weatherArg != "drizzle" && weatherArg != "after") {
        fprintf(stderr, "--weather: clear, overcast, rain, drizzle or after\n");
        return 1;
      }
    }
    else if (a == "--hold") {
      std::string v = next();
      if (sscanf(v.c_str(), "%f,%f,%f", &holdX, &holdY, &holdFor) != 3) { fprintf(stderr, "--hold wants X,Y,S\n"); return 1; }
    }
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
  }

  const auto appStart = std::chrono::steady_clock::now();
  auto sinceStart = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - appStart).count(); };
  std::string dir = storage::init(dataDir);
  logOpen(dir.empty() ? "dotd_desktop.log" : (std::filesystem::path(dir) / "dotd_desktop.log").string());
  if (dir.empty()) logWarn("no writable save folder: settings and saves last for this session only");
  if (!findAssetRoot()) { fatal("The assets folder was not found next to the game (assets/models/terran/bunker.obj)."); return 1; }
  // Graphics card: the driver reads these exports when the GL context is created,
  // so the menu's choice (or --gpu) takes effect at startup — checked: Intel with 0, the RTX with 1
  std::string gpu = gpuArg.empty() ? storage::get("gpu", "dedicated") : gpuArg;
#ifdef _WIN32
  if (gpu == "integrated") { NvOptimusEnablement = 0; AmdPowerXpressRequestHighPerformance = 0; }
#else
  // Linux: the card is picked outside the game (DRI_PRIME=1 for Mesa, prime-run for NVIDIA)
  if (!gpuArg.empty()) logWarn("--gpu has no effect on Linux: start the game with DRI_PRIME=1 (Mesa) or prime-run (NVIDIA)");
#endif

  // The workers parse the assets and the audio device opens while the window is created
  jobs::init(threads);
  models::startLoading();
  audio::init(std::strtof(storage::get("volume", "0.6").c_str(), nullptr), storage::get("muted") == "1", storage::get("music", "1") != "0");

  glfwSetErrorCallback([](int code, const char* desc) { logError("GLFW %d: %s", code, desc); });
#ifdef __linux__
  // X11 (XWayland on a Wayland desktop) wherever there is an X server: GLFW's Wayland backend can
  // neither place the window nor switch video modes, which the Display menu does
  int platform = platformArg == "wayland" ? GLFW_PLATFORM_WAYLAND
               : platformArg == "x11" || (platformArg.empty() && std::getenv("DISPLAY")) ? GLFW_PLATFORM_X11 : GLFW_ANY_PLATFORM;
  if (platform != GLFW_ANY_PLATFORM && glfwPlatformSupported(platform)) glfwInitHint(GLFW_PLATFORM, platform);
  else if (!platformArg.empty()) logWarn("--platform %s: not built into this binary", platformArg.c_str());
#endif
  if (!glfwInit()) { fatal("GLFW could not initialise."); return 1; }
#ifdef __linux__
  logInfo("windowing: %s", glfwGetPlatform() == GLFW_PLATFORM_WAYLAND ? "Wayland" : glfwGetPlatform() == GLFW_PLATFORM_X11 ? "X11" : "other");
#endif
  // OpenGL 4.3 (compute shaders for the GPU particles), else 3.3 below
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  // the scene's MSAA lives in the HDR target (post.h); the window only takes the composite and the HUD
  glfwWindowHint(GLFW_SAMPLES, postOn ? 0 : 4);
  // default size: 1600×900 at the monitor's UI scale, fitted into its work area
  GLFWmonitor* mon = glfwGetPrimaryMonitor();
  float monScale = 1, monScaleY = 1;
  if (mon) glfwGetMonitorContentScale(mon, &monScale, &monScaleY);
  bool sizeGiven = winW > 0 && winH > 0;
  int prefW = 0, prefH = 0;
  sscanf(storage::get("resolution").c_str(), "%dx%d", &prefW, &prefH);
  bool prefFullscreen = storage::get("display") == "fullscreen";
  if (!sizeGiven) {
    int wx = 0, wy = 0, ww = 1600, wh = 900;
    if (mon) glfwGetMonitorWorkarea(mon, &wx, &wy, &ww, &wh);
    if (!prefFullscreen && prefW > 0 && prefH > 0) {   // the window size chosen in the menu, if it still fits
      winW = std::min(prefW, ww);
      winH = std::min(prefH, wh - 40);
    } else {
      float w = 1600 * monScale, h = 900 * monScale, fit = std::min({1.f, ww * 0.92f / w, (wh - 40) * 0.92f / h});
      winW = (int)(w * fit);
      winH = (int)(h * fit);
    }
  }
  GLFWwindow* win = glfwCreateWindow(winW, winH, "Defense of the Dominion", nullptr, nullptr);
  if (!win) {
    logWarn("no OpenGL 4.3 context — trying 3.3 (GPU particles off)");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    win = glfwCreateWindow(winW, winH, "Defense of the Dominion", nullptr, nullptr);
  }
  if (!win) { fatal("Could not open an OpenGL 3.3 window — check the graphics driver."); glfwTerminate(); return 1; }
  logInfo("startup: window after %.0f ms", sinceStart() * 1000);
  glfwMakeContextCurrent(win);
  glfwSwapInterval(vsync ? 1 : 0);
  int glv = gladLoadGL((GLADloadfunc)glfwGetProcAddress);
  if (!glv) { fatal("OpenGL functions could not be loaded."); return 1; }
  const std::string renderer = (const char*)glGetString(GL_RENDERER);
  logInfo("OpenGL %d.%d — %s", GLAD_VERSION_MAJOR(glv), GLAD_VERSION_MINOR(glv), renderer.c_str());
  if (!sizeGiven && prefFullscreen) {   // fullscreen at the mode chosen in the menu (else the monitor's largest)
    auto modes = display::fullscreenSizes(win);
    display::Size s = modes.empty() ? display::Size{winW, winH} : modes.front();
    for (auto& m : modes) if (m.w == prefW && m.h == prefH) s = m;
    display::apply(win, true, s);
  }
  glfwGetWindowSize(win, &winW, &winH);

  float scale = uiScaleArg > 0 ? uiScaleArg : display::uiScaleFor(win);
  logInfo("window %dx%d, UI scale %.2f", winW, winH, scale);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0;
  st.PopupBorderSize = 1;
  st.Colors[ImGuiCol_PopupBg] = ImVec4(0.055f, 0.118f, 0.204f, 0.97f);
  st.Colors[ImGuiCol_Border] = ImVec4(0.36f, 0.72f, 1.f, 1.f);
  st.Colors[ImGuiCol_Text] = ImVec4(0.85f, 0.91f, 0.98f, 1.f);
  st.WindowPadding = ImVec2(8 * scale, 6 * scale);

  // our callbacks first: the ImGui backend chains to them
  glfwSetKeyCallback(win, keyCb);
  glfwSetCharCallback(win, charCb);
  glfwSetMouseButtonCallback(win, mouseCb);
  glfwSetScrollCallback(win, scrollCb);
  glfwSetWindowIconifyCallback(win, iconifyCb);
  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 330");
  ui::init(scale);   // also queues the HUD icons for decoding on the workers
  // the glyph atlas (stb_truetype, ~25 font sizes) builds on its own thread while the game initialises
  std::thread fontThread([] { ImGui::GetIO().Fonts->Build(); });

  Game game;
  g_game = &game;
  game.setUiScale(scale);
  bool ok = game.init(win);
  fontThread.join();
  if (!ok) { fatal("The renderer could not start (see dotd_desktop.log)."); return 1; }
  game.showPerf(perf);
  game.benchFire(benchFire);
  game.testData(!dataDir.empty());
  if (noTutorial) game.tutorialForRun(false);
  game.postForRun(postOn);
  if (dnAt >= 0) game.setDayNightTime(dnAt);
  if (!weatherArg.empty()) game.weatherForRun(weatherArg);   // (the title; --start sets it again in the new game)
  int holdStep = 0, holdAmmo0 = 0;   // --hold: 0 wait · 1 cursor placed · 2 pressed · 3 done
  double holdT0 = 0;
  if (cpuParticles) game.gpuParticlesForRun(false);
  game.setGpuInfo(display::adapters(), renderer, gpu != "integrated");
  float pendingScale = scale;
  double pendingSince = 0;
  logInfo("startup: ready for the first frame after %.0f ms", sinceStart() * 1000);
  double last = glfwGetTime(), started = -1, launch = last - sinceStart();
  bool allLoaded = false;
  double firstFrame = -1, loadWorst = 0;
  int exitCode = 0;
  while (!glfwWindowShouldClose(win)) {
    if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) { glfwWaitEvents(); last = glfwGetTime(); continue; }   // minimised: the game freezes
    glfwPollEvents();
    // assets parsed by the workers: upload them here, at most ~4 ms per frame
    if (!allLoaded) {
      double t0 = glfwGetTime();
      while (glfwGetTime() - t0 < 0.004)
        if (const char* k = models::loadNext()) game.onAssetLoaded(k);
        else break;
      allLoaded = models::allSettled();
      if (allLoaded) logInfo("assets: all loaded %.0f ms after launch; first frame after %.0f ms, longest frame while loading %.1f ms",
                             (glfwGetTime() - launch) * 1000, firstFrame * 1000, loadWorst * 1000);
    }
    double now = glfwGetTime();
    if (firstFrame < 0) firstFrame = now - launch;
    if (!allLoaded) loadWorst = std::max(loadWorst, now - last);
    float dt = (float)std::min(now - last, 0.05);
    last = now;
    if ((!startMode.empty() || !openPanel.empty()) && allLoaded && started < 0) {   // --open alone: on the title
      if (!startMode.empty()) game.autoStart(startMode);
      if (!startMode.empty() && !weatherArg.empty()) game.weatherForRun(weatherArg);
      for (auto& c : cheats) game.runCheat(c);
      for (int i = 0; i < zoomIn; i++) game.testKey(GLFW_KEY_EQUAL);
      if (!openPanel.empty()) game.openPanel(openPanel);
      if (resizeW > 0) display::apply(win, false, {resizeW, resizeH});   // test: a resolution change while running
      started = now;
    }
    for (Later& l : later)
      if (!l.done && started >= 0 && now - started >= l.at) {
        if (l.key) game.testKey(l.key);
        else game.runCheat(l.cheat);
        l.done = true;
      }
    if (holdFor >= 0 && started >= 0) {   // test: the fire button, through the game's own mouse handler
      int ww, wh;
      glfwGetWindowSize(win, &ww, &wh);
      if (holdStep == 0 && now - started >= 1) {   // the cursor first: the HUD's hover state follows it a frame later
        glfwSetCursorPos(win, holdX * ww, holdY * wh);
        holdT0 = now;
        holdStep = 1;
      } else if (holdStep == 1 && now - holdT0 >= 0.1) {
        holdAmmo0 = game.ammo();
        game.testPress(true);
        holdT0 = now;
        holdStep = 2;
      } else if (holdStep == 2 && now - holdT0 >= holdFor) {
        game.testPress(false);
        double cx, cy;
        glfwGetCursorPos(win, &cx, &cy);
        logInfo("hold test: held %.2f s at (%.0f, %.0f), ammo %d -> %d: %d volleys", now - holdT0, cx, cy, holdAmmo0, game.ammo(), holdAmmo0 - game.ammo());
        holdStep = 3;
      }
    }
    // The HUD scale follows the window height (resolution change, resize): the glyph
    // atlas is rebuilt once the size has stayed put for 0.25 s
    if (uiScaleArg <= 0) {
      float want = display::uiScaleFor(win);
      if (want != pendingScale) { pendingScale = want; pendingSince = now; }
      if (pendingScale != scale && now - pendingSince >= 0.25) {
        scale = pendingScale;
        ui::init(scale);
        ImGui::GetIO().Fonts->Build();
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        ImGui_ImplOpenGL3_CreateFontsTexture();
        game.setUiScale(scale);
        logInfo("HUD scale %.2f", scale);
      }
    }
    if (game.wantsRelaunch()) glfwSetWindowShouldClose(win, 1);   // the graphics card changed

    int fbw, fbh;
    glfwGetFramebufferSize(win, &fbw, &fbh);
    glViewport(0, 0, fbw, fbh);
    glClearColor(0.012f, 0.027f, 0.051f, 1);   // --bg #03070d
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    game.frame(dt);
    ImGui::Render();
    glViewport(0, 0, fbw, fbh);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (!shotPath.empty()) {
      bool begun = startMode.empty() || started >= 0;   // --start: count from the game's start
      double t0 = startMode.empty() ? launch : started;
      if (begun && now - t0 >= shotAfter) {
        bool ok = Renderer::saveScreenshot(shotPath, fbw, fbh);
        logInfo("screenshot %s: %s", shotPath.c_str(), ok ? "written" : "FAILED");
        exitCode = ok ? 0 : 2;
        glfwSetWindowShouldClose(win, 1);
      }
    }
    glfwSwapBuffers(win);
  }
  const bool relaunch = game.wantsRelaunch();
  g_game = nullptr;
  jobs::shutdown();
  game.shutdown();
  audio::shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  if (relaunch) {   // a fresh process: the driver picks the card when it creates the context
    logInfo("restarting on the %s graphics card", storage::get("gpu", "dedicated").c_str());
    display::relaunch();
  }
  return exitCode;
}
