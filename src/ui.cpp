#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>

#include "assets.h"
#include "imgui_internal.h"
#include "log.h"

namespace ui {
namespace {
float g_scale = 1;
const float UI_SIZES[] = {9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 22, 26, 38};
const float MONO_SIZES[] = {6, 7, 10, 11, 12, 13, 14, 15, 16, 20};
std::map<float, ImFont*> g_ui, g_mono;
std::map<std::string, ImTextureID> g_icons;
const float PI = 3.14159265f;

ImFont* nearest(std::map<float, ImFont*>& m, float px) {
  ImFont* best = nullptr;
  float bd = 1e9f;
  for (auto& [s, f] : m)
    if (std::abs(s - px) < bd) { bd = std::abs(s - px); best = f; }
  return best ? best : ImGui::GetFont();
}

#ifndef _WIN32
// Linux: the first of these font files found in the usual font folders (each distribution lays
// /usr/share/fonts out its own way, so they are found by file name; one scan per run)
std::string systemFont(std::initializer_list<const char*> names) {
  namespace fs = std::filesystem;
  static std::map<std::string, std::string> files;   // file name → path, the first found wins
  static bool scanned = false;
  if (!scanned) {
    scanned = true;
    std::vector<fs::path> roots;
    if (const char* home = std::getenv("HOME")) roots.push_back(fs::path(home) / ".local/share/fonts");
    roots.push_back("/usr/local/share/fonts");
    roots.push_back("/usr/share/fonts");
    for (const fs::path& r : roots) {
      std::error_code ec;
      for (fs::recursive_directory_iterator it(r, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.emplace(it->path().filename().string(), it->path().string());
    }
  }
  for (const char* n : names)
    if (auto f = files.find(n); f != files.end()) return f->second;
  return "";
}
#endif
}  // namespace

float S() { return g_scale; }

bool init(float scale) {
  g_scale = scale;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  // Latin-1 and the punctuation the HUD uses; symbols only where they appear
  // (merging whole Unicode blocks made the atlas ~40× larger and the first frame >1 s)
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0x2191, 0x2193, 0x2212, 0x2212,
                                   0x221E, 0x221E, 0x237C, 0x237C, 0x25B2, 0x25B2, 0x25B8, 0x25B8, 0x25BE, 0x25BE,
                                   0x25C0, 0x25C0, 0x25C6, 0x25C6, 0x25C8, 0x25C8, 0x2600, 0x2600, 0x2605, 0x2605, 0x263D, 0x263D,
                                   0x2713, 0x2713, 0x2715, 0x2715, 0x2717, 0x2717, 0x2759, 0x2759, 0};
  namespace fs = std::filesystem;
#ifdef _WIN32
  const std::string uiPath = "C:/Windows/Fonts/bahnschrift.ttf";
  const std::string monoPath = "C:/Windows/Fonts/consola.ttf";
  const std::string symPath = "C:/Windows/Fonts/seguisym.ttf";
  bool haveUi = fs::exists(uiPath), haveMono = fs::exists(monoPath), haveSym = fs::exists(symPath);
  if (!haveUi) logWarn("font %s not found — using ImGui's default font", uiPath.c_str());
#else
  // The Windows fonts if they were installed, else stand-ins: a condensed sans for Bahnschrift, a mono for
  // Consolas, and DejaVu Sans for the symbols of Segoe UI Symbol (fonts-dejavu-core / -extra on Debian / Ubuntu)
  const std::string uiPath = systemFont({"bahnschrift.ttf", "DejaVuSansCondensed.ttf", "Ubuntu-C.ttf", "LiberationSansNarrow-Regular.ttf",
                                         "NotoSans-Regular.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf"});
  const std::string monoPath = systemFont({"consola.ttf", "DejaVuSansMono.ttf", "UbuntuMono-R.ttf", "LiberationMono-Regular.ttf", "NotoSansMono-Regular.ttf"});
  const std::string symPath = systemFont({"seguisym.ttf", "DejaVuSans.ttf", "NotoSansSymbols2-Regular.ttf"});
  bool haveUi = !uiPath.empty(), haveMono = !monoPath.empty(), haveSym = !symPath.empty();
  static bool logged = false;   // init runs again on every HUD rescale
  if (!logged) {
    logged = true;
    if (haveUi) logInfo("fonts: %s, %s%s%s", uiPath.c_str(), haveMono ? monoPath.c_str() : "(no mono font)", haveSym ? ", symbols from " : "", symPath.c_str());
    else logWarn("fonts: no DejaVu, Ubuntu, Liberation or Noto font found — using ImGui's default font (install fonts-dejavu)");
  }
#endif
  auto add = [&](const std::string& path, bool have, float px) -> ImFont* {
    ImFontConfig c;
    c.OversampleH = 2;
    c.OversampleV = 2;
    c.PixelSnapH = false;
    ImFont* f = nullptr;
    if (have) f = io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale, &c, ranges);
    if (!f) {
      ImFontConfig d;
      d.SizePixels = px * scale;
      f = io.Fonts->AddFontDefault(&d);
    }
    if (haveSym) {   // ◆ ▲ ✕ ★ ✓ ☀ ☽ ∞ … from Segoe UI Symbol
      ImFontConfig m;
      m.MergeMode = true;
      m.OversampleH = 2;
      m.OversampleV = 2;
      io.Fonts->AddFontFromFileTTF(symPath.c_str(), px * scale, &m, ranges);
    }
    return f;
  };
  for (float s : UI_SIZES) g_ui[s] = add(uiPath, haveUi, s);
  for (float s : MONO_SIZES) g_mono[s] = add(monoPath, haveMono, s);
  io.FontDefault = g_ui[13];
  // HUD icons: decoded on the workers now, uploaded on first use
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(assetPath("assets/ui")), ec))
    if (e.path().extension() == ".png") prefetchTexture("assets/ui/" + e.path().filename().string());
  return true;
}

ImFont* font(float px, bool mono) { return nearest(mono ? g_mono : g_ui, px); }

ImTextureID icon(const std::string& rel) {
  auto it = g_icons.find(rel);
  if (it != g_icons.end()) return it->second;
  const Texture* t = loadTexture(rel);
  ImTextureID id = t ? (ImTextureID)(intptr_t)t->id : (ImTextureID)0;
  g_icons[rel] = id;
  return id;
}

// ─── text ────────────────────────────────────────────────────────────────────
static void forChars(const char* s, const std::function<void(const char*, const char*, unsigned)>& f) {
  const char* p = s;
  while (*p) {
    unsigned c = 0;
    int n = ImTextCharFromUtf8(&c, p, nullptr);
    if (n <= 0) break;
    f(p, p + n, c);
    p += n;
  }
}
float textH(const Txt& t) { return t.px * g_scale; }
float textW(const char* s, const Txt& t) {
  ImFont* f = font(t.px, t.mono);
  float size = t.px * g_scale;
  if (t.spacing == 0) return f->CalcTextSizeA(size, FLT_MAX, 0, s).x + (t.bold ? 0.6f * g_scale : 0);
  float w = 0;
  int n = 0;
  forChars(s, [&](const char* a, const char* b, unsigned) { w += f->CalcTextSizeA(size, FLT_MAX, 0, a, b).x + t.spacing * g_scale; n++; });
  return w;
}
static void rawText(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 col, const char* s, float spacing) {
  if (spacing == 0) { dl->AddText(f, size, p, col, s); return; }
  forChars(s, [&](const char* a, const char* b, unsigned) {
    dl->AddText(f, size, p, col, a, b);
    p.x += f->CalcTextSizeA(size, FLT_MAX, 0, a, b).x + spacing * g_scale;
  });
}
void text(ImDrawList* dl, ImVec2 p, const char* s, const Txt& t) {
  ImFont* f = font(t.px, t.mono);
  float size = t.px * g_scale;
  p.x = std::round(p.x);
  p.y = std::round(p.y);
  if (t.glow) {
    float r = 1.3f * g_scale;
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4;
      rawText(dl, f, size, {p.x + std::cos(a) * r, p.y + std::sin(a) * r}, alpha(t.glow, 0.28f), s, t.spacing);
    }
  }
  if (t.shadow) rawText(dl, f, size, {p.x, p.y + g_scale}, IM_COL32(0, 0, 0, 200), s, t.spacing);
  rawText(dl, f, size, p, t.col, s, t.spacing);
  if (t.bold) rawText(dl, f, size, {p.x + 0.6f * g_scale, p.y}, t.col, s, t.spacing);
}

// splits s at ◆ / ▲ (the resource markers)
static void richParts(const char* s, const std::function<void(const std::string&, int)>& f) {
  std::string cur;
  const char* p = s;
  while (*p) {
    if (!strncmp(p, "\xE2\x97\x86", 3) || !strncmp(p, "\xE2\x96\xB2", 3)) {
      if (!cur.empty()) f(cur, 0);
      cur.clear();
      f("", p[2] == '\x86' ? 1 : 2);
      p += 3;
    } else cur += *p++;
  }
  if (!cur.empty()) f(cur, 0);
}
float richW(const char* s, const Txt& t) {
  float w = 0, em = t.px * g_scale;
  richParts(s, [&](const std::string& part, int ic) { w += ic ? em * 1.2f + 2 * g_scale : textW(part.c_str(), t); });
  return w;
}
void rich(ImDrawList* dl, ImVec2 p, const char* s, const Txt& t) {
  float em = t.px * g_scale;
  richParts(s, [&](const std::string& part, int ic) {
    if (ic) {
      ImTextureID tex = icon(ic == 1 ? "assets/ui/icon-mineral-terran.png" : "assets/ui/icon-gas-terran.png");
      float sz = em * 1.2f, y = p.y + em * 0.82f - sz + 0.28f * em;
      if (tex) dl->AddImage(tex, {p.x, y}, {p.x + sz, y + sz});
      p.x += sz + 2 * g_scale;
    } else {
      text(dl, p, part.c_str(), t);
      p.x += textW(part.c_str(), t);
    }
  });
}

float wrapped(ImDrawList* dl, ImVec2 p, float w, const char* s, const Txt& t, float lineH, bool draw) {
  std::string line, word;
  float y = 0, lh = t.px * g_scale * lineH;
  auto flush = [&](bool force) {
    if (line.empty() && !force) return;
    if (draw) rich(dl, {p.x, p.y + y}, line.c_str(), t);
    y += lh;
    line.clear();
  };
  std::string str(s);
  size_t i = 0;
  while (i <= str.size()) {
    size_t j = str.find_first_of(" \n", i);
    if (j == std::string::npos) j = str.size();
    word = str.substr(i, j - i);
    std::string test = line.empty() ? word : line + " " + word;
    if (!line.empty() && richW(test.c_str(), t) > w) { flush(false); line = word; }
    else line = test;
    if (j < str.size() && str[j] == '\n') flush(true);
    i = j + 1;
  }
  if (!line.empty()) flush(false);
  return y;
}

// ─── shapes ──────────────────────────────────────────────────────────────────
ImU32 lerpCol(ImU32 a, ImU32 b, float t) {
  ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t});
}
ImU32 alpha(ImU32 c, float a) {
  int al = (int)(((c >> IM_COL32_A_SHIFT) & 255) * std::clamp(a, 0.f, 1.f) + 0.5f);
  return (c & ~IM_COL32_A_MASK) | ((ImU32)al << IM_COL32_A_SHIFT);
}

static void gradPoly(ImDrawList* dl, const ImVec2* pts, int n, float y0, float y1, ImU32 top, ImU32 bot) {
  if (n < 3) return;
  ImVec2 uv = dl->_Data->TexUvWhitePixel;
  dl->PrimReserve((n - 2) * 3, n);
  ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
  for (int i = 0; i < n; i++) {
    float t = y1 > y0 ? std::clamp((pts[i].y - y0) / (y1 - y0), 0.f, 1.f) : 0;
    dl->PrimWriteVtx(pts[i], uv, lerpCol(top, bot, t));
  }
  for (int i = 1; i < n - 1; i++) {
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx((ImDrawIdx)(base + i));
    dl->PrimWriteIdx((ImDrawIdx)(base + i + 1));
  }
}
void cutPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 border, float bw) {
  float c = std::min({cut, (b.x - a.x) / 2, (b.y - a.y) / 2});
  ImVec2 pts[6] = {{a.x + c, a.y}, {b.x, a.y}, {b.x, b.y - c}, {b.x - c, b.y}, {a.x, b.y}, {a.x, a.y + c}};
  gradPoly(dl, pts, 6, a.y, b.y, top, bottom);
  if (border) {
    ImVec2 q[6] = {{a.x + c, a.y + .5f}, {b.x - .5f, a.y + .5f}, {b.x - .5f, b.y - c}, {b.x - c, b.y - .5f}, {a.x + .5f, b.y - .5f}, {a.x + .5f, a.y + c}};
    dl->AddPolyline(q, 6, border, ImDrawFlags_Closed, bw);
  }
}
void gradRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) { dl->AddRectFilledMultiColor(a, b, top, top, bottom, bottom); }
void hgradRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 l, ImU32 r) { dl->AddRectFilledMultiColor(a, b, l, r, r, l); }
void glowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius) {
  int n = std::max(1, (int)std::round(radius));
  for (int i = 1; i <= n; i++) {
    float k = 1 - (float)(i - 1) / n;
    dl->AddRect({a.x - i, a.y - i}, {b.x + i, b.y + i}, alpha(col, k * k * 0.5f), 0, 0, 1.0f);
  }
}
void pie(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 col) {
  if (frac <= 0) return;
  dl->PushClipRect(a, b, true);
  ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
  float R = std::hypot(b.x - a.x, b.y - a.y);
  int n = std::max(2, (int)(frac * 64));
  for (int i = 0; i < n; i++) {
    float t0 = -PI / 2 + 2 * PI * frac * i / n, t1 = -PI / 2 + 2 * PI * frac * (i + 1) / n;
    dl->AddTriangleFilled(c, {c.x + std::cos(t0) * R, c.y + std::sin(t0) * R}, {c.x + std::cos(t1) * R, c.y + std::sin(t1) * R}, col);
  }
  dl->PopClipRect();
}
void radial(ImDrawList* dl, ImVec2 c, ImVec2 r, ImU32 inner, ImU32 outer, float from) {
  const int N = 48;
  ImVec2 uv = dl->_Data->TexUvWhitePixel;
  // centre disc (inner colour) then a band inner → outer
  dl->PrimReserve(N * 3 + N * 6, 1 + N * 2);
  ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
  dl->PrimWriteVtx(c, uv, inner);
  for (int i = 0; i < N; i++) {
    float a = 2 * PI * i / N;
    dl->PrimWriteVtx({c.x + std::cos(a) * r.x * from, c.y + std::sin(a) * r.y * from}, uv, inner);
  }
  for (int i = 0; i < N; i++) {
    float a = 2 * PI * i / N;
    dl->PrimWriteVtx({c.x + std::cos(a) * r.x, c.y + std::sin(a) * r.y}, uv, outer);
  }
  for (int i = 0; i < N; i++) {
    int j = (i + 1) % N;
    dl->PrimWriteIdx(base);
    dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
    dl->PrimWriteIdx((ImDrawIdx)(base + 1 + j));
  }
  for (int i = 0; i < N; i++) {
    int j = (i + 1) % N;
    ImDrawIdx a0 = base + 1 + i, a1 = base + 1 + j, b0 = base + 1 + N + i, b1 = base + 1 + N + j;
    dl->PrimWriteIdx(a0); dl->PrimWriteIdx(b0); dl->PrimWriteIdx(b1);
    dl->PrimWriteIdx(a0); dl->PrimWriteIdx(b1); dl->PrimWriteIdx(a1);
  }
}

// ─── SVG stroke icons ────────────────────────────────────────────────────────
namespace {
struct Poly { std::vector<ImVec2> pts; bool closed = false; };
struct Icon { std::vector<Poly> polys; bool filled = false; };

void arcTo(std::vector<ImVec2>& out, ImVec2 p0, float rx, float ry, float phiDeg, bool large, bool sweep, ImVec2 p1) {
  // SVG endpoint → centre parameterisation (SVG 1.1 F.6.5)
  float phi = phiDeg * PI / 180, cp = std::cos(phi), sp = std::sin(phi);
  float dx = (p0.x - p1.x) / 2, dy = (p0.y - p1.y) / 2;
  float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
  rx = std::abs(rx); ry = std::abs(ry);
  float lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
  if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
  float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
  float co = std::sqrt(std::max(0.f, num / den)) * (large == sweep ? -1 : 1);
  float cx1 = co * rx * y1 / ry, cy1 = -co * ry * x1 / rx;
  float cx = cp * cx1 - sp * cy1 + (p0.x + p1.x) / 2, cy = sp * cx1 + cp * cy1 + (p0.y + p1.y) / 2;
  auto ang = [](float ux, float uy, float vx, float vy) {
    float a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    return a;
  };
  float t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
  float dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
  if (!sweep && dt > 0) dt -= 2 * PI;
  if (sweep && dt < 0) dt += 2 * PI;
  int n = std::max(4, (int)(std::abs(dt) / (PI / 16)));
  for (int i = 1; i <= n; i++) {
    float t = t1 + dt * i / n;
    float x = rx * std::cos(t), y = ry * std::sin(t);
    out.push_back({cp * x - sp * y + cx, sp * x + cp * y + cy});
  }
}

std::vector<Poly> parsePath(const char* d) {
  std::vector<Poly> polys;
  Poly cur;
  ImVec2 pos(0, 0), start(0, 0), lastCtrl(0, 0);
  char cmd = 0, prevCmd = 0;
  const char* p = d;
  auto skip = [&]() { while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n') p++; };
  auto num = [&]() {
    skip();
    const char* s = p;
    if (*p == '-' || *p == '+') p++;
    bool dot = false;
    while ((*p >= '0' && *p <= '9') || (*p == '.' && !dot)) { if (*p == '.') dot = true; p++; }
    return (float)std::atof(std::string(s, p).c_str());
  };
  auto hasNum = [&]() { skip(); return (*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.'; };
  auto endPoly = [&]() { if (cur.pts.size() > 1) polys.push_back(cur); cur = Poly(); };
  auto cubic = [&](ImVec2 c1, ImVec2 c2, ImVec2 e) {
    ImVec2 s = pos;
    for (int i = 1; i <= 12; i++) {
      float t = i / 12.f, u = 1 - t;
      cur.pts.push_back({u * u * u * s.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * e.x,
                         u * u * u * s.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * e.y});
    }
    lastCtrl = c2;
    pos = e;
  };
  while (true) {
    skip();
    if (!*p) break;
    if (std::isalpha((unsigned char)*p)) cmd = *p++;
    else if (cmd == 'M') cmd = 'L';
    else if (cmd == 'm') cmd = 'l';
    bool rel = std::islower((unsigned char)cmd);
    ImVec2 o = rel ? pos : ImVec2(0, 0);
    switch (std::toupper((unsigned char)cmd)) {
      case 'M': { endPoly(); float x = num(), y = num(); pos = {o.x + x, o.y + y}; start = pos; cur.pts.push_back(pos); break; }
      case 'L': { float x = num(), y = num(); pos = {o.x + x, o.y + y}; cur.pts.push_back(pos); break; }
      case 'H': { float x = num(); pos.x = (rel ? pos.x : 0) + x; cur.pts.push_back(pos); break; }
      case 'V': { float y = num(); pos.y = (rel ? pos.y : 0) + y; cur.pts.push_back(pos); break; }
      case 'C': {
        float a = num(), b = num(), c = num(), e = num(), f = num(), g = num();
        cubic({o.x + a, o.y + b}, {o.x + c, o.y + e}, {o.x + f, o.y + g});
        break;
      }
      case 'S': {
        ImVec2 c1 = (std::toupper((unsigned char)prevCmd) == 'C' || std::toupper((unsigned char)prevCmd) == 'S') ? ImVec2(2 * pos.x - lastCtrl.x, 2 * pos.y - lastCtrl.y) : pos;
        float c = num(), e = num(), f = num(), g = num();
        cubic(c1, {o.x + c, o.y + e}, {o.x + f, o.y + g});
        break;
      }
      case 'A': {
        float rx = num(), ry = num(), rot = num(), la = num(), sw = num(), x = num(), y = num();
        ImVec2 e(o.x + x, o.y + y);
        arcTo(cur.pts, pos, rx, ry, rot, la != 0, sw != 0, e);
        pos = e;
        break;
      }
      case 'Z': cur.closed = true; pos = start; endPoly(); cur.pts.push_back(pos); break;
      default: p++; break;
    }
    prevCmd = cmd;
    if (std::toupper((unsigned char)cmd) == 'Z') continue;
    if (!hasNum()) continue;
  }
  endPoly();
  return polys;
}

std::string circle(float cx, float cy, float r) {
  char b[160];
  snprintf(b, sizeof b, "M%g %g A%g %g 0 1 0 %g %g A%g %g 0 1 0 %g %g z", cx - r, cy, r, r, cx + r, cy, r, r, cx - r, cy);
  return b;
}

std::map<std::string, Icon>& icons() {
  static std::map<std::string, Icon> m;
  if (!m.empty()) return m;
  auto def = [&](const char* name, std::vector<std::string> paths, bool filled = false) {
    Icon ic;
    ic.filled = filled;
    for (auto& d : paths) for (auto& q : parsePath(d.c_str())) ic.polys.push_back(q);
    m[name] = ic;
  };
  def("stim", {"M17 3l4 4M19 5l-3 3", "M16 8L7.5 16.5l-3-3L13 5z", "M9 11l1.5 1.5M11.5 8.5L13 10M5.5 18.5L3 21"});
  def("roster", {circle(8.5f, 8, 3), "M2.5 20c0-3.3 2.7-6 6-6s6 2.7 6 6", "M15.5 5.2a3 3 0 0 1 0 5.6M17 14.3c2.6.6 4.5 2.9 4.5 5.7"});
  def("menu", {"M4 7h16M4 12h16M4 17h16"});
  def("snd", {"M4 9h4l5-4v14l-5-4H4z", "M16.5 9a4 4 0 0 1 0 6M19 6.5a7.5 7.5 0 0 1 0 11"});
  def("snd-off", {"M4 9h4l5-4v14l-5-4H4z", "M16.5 9.5l5 5M21.5 9.5l-5 5"});
  def("rank", {"M5 13l7-5 7 5M5 19l7-5 7 5"});
  def("campaign", {"M5 21V3.5M5 4h12l-2.5 4L17 12H5"});
  def("endless", {"M7 8.5c-3 0-4.5 1.6-4.5 3.5S4 15.5 7 15.5c4 0 6-7 10-7 3 0 4.5 1.6 4.5 3.5s-1.5 3.5-4.5 3.5c-4 0-6-7-10-7z"});
  def("lock", {"M5.5 11h13v10h-13z", "M8 11V7.5a4 4 0 0 1 8 0V11"});
  def("load", {"M3 6.5h6l2 2.5h10v10.5H3z"});
  def("save", {"M5 3h11.5L20 6.5V21H5z", "M8 3v5h7V3M8 14h8v7H8z"});
  def("play", {"M7 4.5l12.5 7.5L7 19.5z"}, true);
  def("restart", {"M4 12a8 8 0 1 0 2.4-5.7", "M4 3.5V9h5.5"});
  def("home", {"M3 11.5L12 4l9 7.5", "M6 9.5V20h12V9.5"});
  def("cheat", {"M3.5 5h17v14h-17z", "M7 10l3 2-3 2M12.5 15H17"});
  def("gfx", {"M3 4.5h18v11.5H3z", "M8 20h8M12 16v4"});
  def("keys", {"M2.5 6.5h19v11h-19z", "M6 10h1.5M10 10h1.5M14 10h1.5M17.5 10h.5M7 14h10"});
  def("help", {circle(12, 12, 9), "M9.3 9.4a2.8 2.8 0 1 1 3.9 2.6c-.8.4-1.2 1-1.2 1.9v.4M12 17.2v.3"});
  def("display", {"M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5"});   // the window / fullscreen corners
  // Sensor Tower radar, the web sprite's #i-sensor: two quarter arcs from a base point, a sweep line
  // and a blip (its filled dot drawn as a tiny stroked circle). PLACEHOLDER: replace with
  // btn-building-terran-sensortower.png once it is exported
  def("sensor", {"M4 12A8 8 0 0 1 12 20", "M4 5A15 15 0 0 1 19 20", "M4 20L14.5 9.5", circle(14, 14.3f, 0.8f)});
  return m;
}
}  // namespace

void svgIcon(ImDrawList* dl, const char* name, ImVec2 c, float size, ImU32 col) {
  auto& m = icons();
  auto it = m.find(name);
  if (it == m.end()) return;
  float k = size / 24.f;
  ImVec2 o(c.x - size / 2, c.y - size / 2);
  for (const Poly& q : it->second.polys) {
    std::vector<ImVec2> pts;
    for (auto& p : q.pts) pts.push_back({o.x + p.x * k, o.y + p.y * k});
    if (it->second.filled) dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), col);
    else dl->AddPolyline(pts.data(), (int)pts.size(), col, q.closed ? ImDrawFlags_Closed : 0, std::max(1.f, 2 * k));
  }
}

void image(ImDrawList* dl, ImTextureID tex, ImVec2 a, ImVec2 b, ImU32 tint) {
  if (tex) dl->AddImage(tex, a, b, {0, 0}, {1, 1}, tint);
}

Btn button(const char* id, ImVec2 size, bool enabled) {
  Btn r;
  r.a = ImGui::GetCursorScreenPos();
  r.b = {r.a.x + size.x, r.a.y + size.y};
  bool pressed = ImGui::InvisibleButton(id, {std::max(1.f, size.x), std::max(1.f, size.y)});
  r.hovered = ImGui::IsItemHovered();
  r.held = ImGui::IsItemActive();
  r.clicked = pressed && enabled;
  return r;
}
}  // namespace ui
