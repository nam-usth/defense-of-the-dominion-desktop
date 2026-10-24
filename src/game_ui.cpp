// HUD, menus and overlays — the web version's index.html + css/hud.css,
// drawn with Dear ImGui. Layers (bottom → top) follow the CSS z-index order.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <sstream>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "audio.h"
#include "config.h"
#include "game.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "jobs.h"
#include "ui.h"

using namespace ui;

static std::string fmt(const char* f, ...) {
  char b[1024];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}
static float sc() { return ui::S(); }
static float ascent(const Txt& t) {
  ImFont* f = font(t.px, t.mono);
  return f->Ascent * (t.px * sc() / f->FontSize);
}
// SC2's autocast mark: two bright dashes with fading tails running round a button's border (the
// cut-panel outline, corner cut `cut`), one lap every 1.6 s
static void autocastBorder(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, float t, ImU32 col) {
  const float s = sc(), c = std::min({cut, (b.x - a.x) / 2, (b.y - a.y) / 2});
  const ImVec2 q[7] = {{a.x + c, a.y + 1}, {b.x - 1, a.y + 1}, {b.x - 1, b.y - c}, {b.x - c, b.y - 1}, {a.x + 1, b.y - 1}, {a.x + 1, a.y + c}, {a.x + c, a.y + 1}};
  float len[6], total = 0;
  for (int i = 0; i < 6; i++) total += len[i] = std::hypot(q[i + 1].x - q[i].x, q[i + 1].y - q[i].y);
  auto at = [&](float d) {   // the point d along the outline
    d = std::fmod(d, total);
    if (d < 0) d += total;
    for (int i = 0; i < 6; i++) {
      if (d <= len[i]) { float k = d / std::max(1e-3f, len[i]); return ImVec2(q[i].x + (q[i + 1].x - q[i].x) * k, q[i].y + (q[i + 1].y - q[i].y) * k); }
      d -= len[i];
    }
    return q[0];
  };
  const int STEPS = 12;
  const float dash = total * 0.18f;
  for (int n = 0; n < 2; n++) {
    float head = t / 1.6f * total + n * total / 2;
    for (int k = 0; k < STEPS; k++) {
      float w = 1 - (float)k / STEPS;   // brightest at the head
      ImVec2 p0 = at(head - dash * k / STEPS), p1 = at(head - dash * (k + 1) / STEPS);
      dl->AddLine(p0, p1, alpha(col, 0.3f * w), 4.5f * s * w + 1);   // its glow
      dl->AddLine(p0, p1, alpha(col, w), 1.6f * s * w + 0.5f);
    }
  }
}
// text whose baseline sits at y
static void textB(ImDrawList* dl, float x, float base, const char* s, const Txt& t) { text(dl, {x, base - ascent(t)}, s, t); }
static void richB(ImDrawList* dl, float x, float base, const char* s, const Txt& t) { rich(dl, {x, base - ascent(t)}, s, t); }
// text vertically centred on cy
static void textC(ImDrawList* dl, float x, float cy, const char* s, const Txt& t) { text(dl, {x, cy - textH(t) * 0.5f}, s, t); }
static Txt T(float px, unsigned col, bool bold = false, float spacing = 0, bool mono = false) {
  Txt t;
  t.px = px;
  t.col = rgb(col);
  t.bold = bold;
  t.spacing = spacing;
  t.mono = mono;
  return t;
}
static Txt TA(float px, ImU32 col, bool bold = false, float spacing = 0, bool mono = false) {
  Txt t = T(px, 0, bold, spacing, mono);
  t.col = col;
  return t;
}
static std::string ellipsize(const std::string& s, float w, const Txt& t) {
  if (richW(s.c_str(), t) <= w) return s;
  std::string r = s;
  while (!r.empty()) {
    r.pop_back();
    while (!r.empty() && ((unsigned char)r.back() & 0xC0) == 0x80) r.pop_back();   // whole UTF-8 characters
    if (richW((r + "\xE2\x80\xA6").c_str(), t) <= w) return r + "\xE2\x80\xA6";
  }
  return r;
}

// An input-taking (or display-only) full-layer window, raised above the previous ones
static bool layer(const char* name, ImVec2 pos, ImVec2 size, bool inputs) {
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!inputs) f |= ImGuiWindowFlags_NoInputs;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  bool open = ImGui::Begin(name, nullptr, f);
  ImGui::PopStyleVar(2);
  ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  return open;
}

// cut-corner HUD button: gradient, border, hover glow
struct BtnStyle {
  ImU32 top = rgb(0x1d3b5e), bot = rgb(0x10223a), border = rgb(EDGE);
  ImU32 hTop = rgb(0x2a5684), hBot = rgb(0x173253), hBorder = rgb(EDGE_HI);
  float cut = 5;
};
static void drawBtnBg(ImDrawList* dl, const Btn& b, const BtnStyle& st, bool enabled) {
  bool hov = b.hovered && enabled;
  if (hov) glowRect(dl, b.a, b.b, rgb(0x46a0ff, 0.45f), 8 * sc());
  cutPanel(dl, b.a, b.b, st.cut * sc(), hov ? st.hTop : st.top, hov ? st.hBot : st.bot, hov ? st.hBorder : st.border);
}

// ─── Canvas overlay: enemy HP bars, floating damage numbers, crosshair ───────
// (the Sensor Tower's ring and blips lie on the ground in the 3D scene: Game::render)
void Game::drawOverlayCanvas() {
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  dl->PushClipRect({canvasX_, canvasY_}, {canvasX_ + canvasW_, canvasY_ + canvasH_}, true);
  const float s = sc();
  if (phase_ == Phase::Playing) {
    // night: no bars beyond the fog, and fading with it in front of it, as the fog fades the enemy;
    // none on a Zerg hidden under its Sensor Tower blip (its bar and badge would tell which kind)
    float fogRange = dnFactor_ > 0.1f ? fogFar_ : INFINITY;
    for (auto* list : {&zerglings_, &roaches_})
      for (auto& e : *list) {
        if (e->dead || !e->visible) continue;
        float fa = 1;
        if (fogRange < INFINITY) {
          float d = std::hypot(e->pos.x, e->pos.z);
          if (d > fogRange) continue;
          float k = glm::clamp((d - fogNear_) / (fogRange - fogNear_), 0.f, 1.f);
          fa = 1 - k * k * (3 - 2 * k);
        }
        float pct = std::max(0.f, e->hp / e->maxHp);
        if (pct >= 1.0f) continue;   // only damaged enemies (less clutter)
        float sx, sy;
        if (!project(e->pos + glm::vec3(0, e->type == 'r' ? 1.4f : 1.1f, 0), sx, sy)) continue;
        bool r = e->type == 'r';
        float bw = (r ? 36 : 24) * s, bh = (r ? 5 : 4) * s, bx = sx - bw / 2, by = sy - bh / 2;
        dl->AddRectFilled({bx - s, by - s}, {bx + bw + s, by + bh + s}, alpha(IM_COL32(0, 0, 0, 166), fa));
        ImU32 col = pct > 0.5f ? IM_COL32((int)std::round(255 * (1 - pct) * 2), 200, 0, 255) : IM_COL32(255, (int)std::round(255 * pct * 2), 0, 255);
        dl->AddRectFilled({bx, by}, {bx + bw * pct, by + bh}, alpha(col, fa));
        dl->AddRect({bx, by}, {bx + bw, by + bh}, alpha(IM_COL32(255, 255, 255, 64), fa), 0, 0, 0.5f * s);
        // attribute badge: ARM (Roach) / LGT (Zergling)
        Txt tag = TA(6, alpha(r ? IM_COL32(255, 150, 50, 217) : IM_COL32(100, 220, 255, 217), fa), true, 0, true);
        textB(dl, bx + bw + 2 * s, by + bh - s, r ? "ARM" : "LGT", tag);
        if (r) {   // numeric HP for Roaches
          Txt n = TA(7, alpha(IM_COL32(255, 255, 255, 166), fa), false, 0, true);
          std::string hp = std::to_string((int)std::ceil(e->hp));
          textB(dl, sx - textW(hp.c_str(), n) / 2, by - 2 * s, hp.c_str(), n);
        }
      }
  }
  for (const FloatText& f : floatTexts_) {   // floating damage numbers
    float sx, sy;
    if (!project(f.p, sx, sy)) continue;
    float a = std::min(1.f, f.life / f.maxLife * 2);
    Txt t = TA(15, alpha(rgb(f.color), a), false, 0, true);
    float w = textW(f.txt.c_str(), t), h = textH(t);
    ImVec2 p(sx - w / 2, sy - h / 2);
    Txt o = TA(15, IM_COL32(0, 0, 0, (int)(178 * a)), false, 0, true);
    for (int i = 0; i < 8; i++) {
      float an = i * 3.14159f / 4;
      text(dl, {p.x + std::cos(an) * 1.5f * s, p.y + std::sin(an) * 1.5f * s}, f.txt.c_str(), o);
    }
    text(dl, p, f.txt.c_str(), t);
  }
  if (phase_ == Phase::Playing) {   // crosshair
    float px = canvasX_ + (float)mx_ * canvasW_, py = canvasY_ + (float)my_ * canvasH_;
    bool nukeReady = wave_ >= cfg::nuke_wave && nukeCD_ == 0;
    ImU32 col = alpha(rgb(nukeReady ? 0xffff00 : ammo_ > 0 ? 0x00ff44 : 0xff6600), 0.85f);
    float k = 11 * s, w = 1.5f * s;
    dl->AddLine({px - k, py}, {px - 3 * s, py}, col, w);
    dl->AddLine({px + 3 * s, py}, {px + k, py}, col, w);
    dl->AddLine({px, py - k}, {px, py - 3 * s}, col, w);
    dl->AddLine({px, py + 3 * s}, {px, py + k}, col, w);
    dl->AddCircle({px, py}, 3.5f * s, col, 16, w);
  }
  dl->PopClipRect();
}

// ─── Slot bar above the bunker (shown on hover, display only) ────────────────
void Game::checkBunkerHover(float dt) {
  if (rosterPaused_) return;
  float bx, by;
  if (!project({0, 1.8f, 0}, bx, by)) return;
  float px = canvasX_ + (float)mx_ * canvasW_, py = canvasY_ + (float)my_ * canvasH_;
  bool near = std::hypot(px - bx, py - by) < 110 * sc() && zoom_ < 0.3f;   // close-up: the bar would cover the bunker
  if (near) { slotOverlayTarget_ = 1; bunkerHoverTimer_ = 0.5f; }
  else if (bunkerHoverTimer_ > 0) {
    bunkerHoverTimer_ -= dt;
    if (bunkerHoverTimer_ <= 0) slotOverlayTarget_ = 0;
  }
}

void Game::drawSlotOverlay() {
  if (rosterPaused_) slotOverlayTarget_ = zoom_ < 0.3f ? 1.f : 0.f;
  else if (zoom_ >= 0.3f) slotOverlayTarget_ = 0;
  if (phase_ != Phase::Playing && !rosterPaused_) slotOverlayTarget_ = 0;
  float dt = ImGui::GetIO().DeltaTime;
  slotOverlayOpacity_ += glm::clamp(slotOverlayTarget_ - slotOverlayOpacity_, -dt / 0.25f, dt / 0.25f);   // .25 s transition
  if (slotOverlayOpacity_ <= 0.01f) return;
  float bx, by;
  if (!project({0, 1.8f, 0}, bx, by)) return;
  const float s = sc(), a = slotOverlayOpacity_;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  float w = 12 * s, gap = 3 * s;
  int used = 0;
  std::vector<std::pair<float, Unit>> pips;
  for (Unit u : roster_) { float pw = unitDef(u).slots == 2 ? 30 * s : 14 * s; pips.push_back({pw, u}); w += pw + gap; used += unitDef(u).slots; }
  w += (6 - used) * (14 * s + gap) - gap;
  float h = 14 * s + 8 * s + 2 * s;
  ImVec2 a0(std::round(bx - w / 2), std::round(by - 50 * s)), a1(a0.x + w, a0.y + h);
  glowRect(dl, a0, a1, alpha(rgb(0x2878dc), 0.35f * a), 10 * s);
  dl->AddRectFilled(a0, a1, IM_COL32(2, 8, 16, (int)(217 * a)));
  dl->AddRect(a0, a1, alpha(rgb(0x5cb8ff), 0.5f * a));
  float x = a0.x + 7 * s, y = a0.y + 5 * s;
  for (auto& [pw, u] : pips) {
    ImU32 f = u == Unit::Marine ? rgb(0x2255aa) : u == Unit::Marauder ? rgb(0x884400) : rgb(0x442288);
    ImU32 b = u == Unit::Marine ? rgb(0x4a8be0) : u == Unit::Marauder ? rgb(0xd4761e) : rgb(0x9152e0);
    dl->AddRectFilled({x, y}, {x + pw, y + 14 * s}, alpha(f, a));
    dl->AddRect({x, y}, {x + pw, y + 14 * s}, alpha(b, a));
    x += pw + gap;
  }
  for (int i = used; i < 6; i++) {
    dl->AddRectFilled({x, y}, {x + 14 * s, y + 14 * s}, IM_COL32(255, 255, 255, (int)(15 * a)));
    dl->AddRect({x, y}, {x + 14 * s, y + 14 * s}, IM_COL32(255, 255, 255, (int)(51 * a)));
    x += 14 * s + gap;
  }
}

// ─── Top bar ─────────────────────────────────────────────────────────────────
void Game::drawTopBar() {
  const float s = sc(), W = (float)winW_, H = 32 * s, cssW = W / s;
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  gradRect(bg, {0, H}, {W, H + 12 * s}, IM_COL32(0, 0, 0, 140), IM_COL32(0, 0, 0, 0));   // drop shadow on the canvas
  if (!layer("##topbar", {0, 0}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradRect(dl, {0, 0}, {W, H}, rgb(0x112238), rgb(0x08121f));
  dl->AddLine({0, H - 0.5f}, {W, H - 0.5f}, rgb(EDGE));
  const float base = 21.5f * s, gapX = (cssW < 1100 ? 12 : 18) * s;
  bool showLabels = cssW >= 860, wide = cssW >= 1100;
  float x = 14 * s;
  auto stat = [&](const char* label, const std::string& v, unsigned vcol, const char* sub) {
    if (showLabels) {
      Txt l = T(11, MUTED, true, 1.5f);
      textB(dl, x, base, label, l);
      x += textW(label, l) + 6 * s;
    }
    Txt vt = T(16, vcol, false, 0, true);
    textB(dl, x, base, v.c_str(), vt);
    x += textW(v.c_str(), vt);
    if (sub) {
      Txt st = T(12, DIM, false, 0, true);
      x += 6 * s;
      textB(dl, x, base, sub, st);
      x += textW(sub, st);
    }
    x += gapX;
  };
  bool camp = mode_ == Mode::Campaign;
  stat("WAVE", std::to_string(wave_), MINERAL, camp ? fmt("/%d", cfg::victory_wave).c_str() : " \xE2\x88\x9E");
  stat("SCORE", std::to_string(score_), WARN, nullptr);
  stat("KILLS", std::to_string(kills_), TEXT, nullptr);
  int t = (int)gameT_;
  if (wide) stat("TIME", fmt("%d:%02d", t / 60, t % 60), TEXT, nullptr);
  if (wide) {   // mode chip
    Txt m = T(11, camp ? MINERAL : GOLD, true, 2);
    const char* ms = camp ? "CAMPAIGN" : "ENDLESS";
    float w = textW(ms, m) + 16 * s, h = 16 * s, y = (H - h) / 2;
    dl->AddRectFilled({x, y}, {x + w, y + h}, camp ? rgb(0x285a96, 0.18f) : rgb(0xa06e0a, 0.16f));
    dl->AddRect({x, y}, {x + w, y + h}, camp ? rgb(EDGE) : rgb(0x8a6a10));
    textC(dl, x + 8 * s, H / 2, ms, m);
    x += w + gapX;
  }
  // right side, laid out from the right edge
  float rx = W - 10 * s;
  BtnStyle bs;
  auto tbButton = [&](const char* id, float w, const std::function<void(ImVec2, ImVec2, bool)>& draw) {
    rx -= w;
    ImGui::SetCursorScreenPos({rx, (H - 22 * s) / 2});
    Btn b = button(id, {w, 22 * s});
    drawBtnBg(dl, b, bs, true);
    draw(b.a, b.b, b.hovered);
    rx -= gapX;
    return b.clicked;
  };
  Txt bt = T(12, 0xbcd7f5, true, 1.5f);
  float menuW = 20 * s + 14 * s + 6 * s + textW("MENU", bt);
  if (tbButton("##tbmenu", menuW, [&](ImVec2 a, ImVec2 b, bool) {
        svgIcon(dl, "menu", {a.x + 10 * s + 7 * s, (a.y + b.y) / 2}, 14 * s, rgb(0xbcd7f5));
        textC(dl, a.x + 10 * s + 20 * s, (a.y + b.y) / 2, "MENU", bt);
      }))
    openMenu();
  Txt gv = T(12, gfxHigh_ ? OK : 0xffaa44, false, 0, true);
  const char* gq = gfxHigh_ ? "HIGH" : "LOW";
  float gfxW = 20 * s + textW("GFX", bt) + 6 * s + textW(gq, gv);
  if (tbButton("##tbgfx", gfxW, [&](ImVec2 a, ImVec2 b, bool) {
        float cy = (a.y + b.y) / 2;
        textC(dl, a.x + 10 * s, cy, "GFX", bt);
        textC(dl, a.x + 10 * s + textW("GFX", bt) + 6 * s, cy, gq, gv);
      }))
    setGfx(!gfxHigh_);
  bool muted = audio::muted();
  if (tbButton("##tbsnd", 34 * s, [&](ImVec2 a, ImVec2 b, bool) {
        svgIcon(dl, muted ? "snd-off" : "snd", {(a.x + b.x) / 2, (a.y + b.y) / 2}, 14 * s, rgb(muted ? DANGER : 0xbcd7f5));
      }))
    toggleMute();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sound on/off [K]");
  if (wide) {
    const Rank& rk = getRank(kills_);
    Txt r = T(13, rk.color, true, 1.5f);
    rx -= textW(rk.name, r);
    textC(dl, rx, H / 2, rk.name, r);
    rx -= gapX;
  }
  if (dnEnabled_) {   // e.g. "☀ DAY  DUSK IN 0:42": the phase, then the countdown to the next one
    std::string next = dayNightNext();
    Txt nt = T(12, MUTED, false, 0, true);
    rx -= textW(next.c_str(), nt);
    textC(dl, rx, H / 2, next.c_str(), nt);
    rx -= 8 * s;
    std::string lbl = dayNightLabel();
    Txt d = T(12, dnFactor_ > 0.5f ? 0x8899ff : 0xffcc44, true, 1);
    rx -= textW(lbl.c_str(), d);
    textC(dl, rx, H / 2, lbl.c_str(), d);
    rx -= gapX;
  }
  // status line: centred in the space left, white flash on change
  float st = statusFlash_ / 0.6f;
  Txt stt = T(14, WARN, true, 0.6f);
  stt.col = lerpCol(rgb(WARN), IM_COL32(255, 255, 255, 255), st);
  stt.glow = rgb(st > 0 ? 0xffdc8c : 0xffaa3c, st > 0 ? 0.9f : 0.35f);
  float avail = rx - x;
  if (avail > 20 * s) {
    std::string str = ellipsize(status_, avail, stt);
    float w = richW(str.c_str(), stt);
    rich(dl, {x + (avail - w) / 2, H / 2 - textH(stt) / 2}, str.c_str(), stt);
  }
  ImGui::End();
}

// ─── Bottom console ──────────────────────────────────────────────────────────

void Game::drawConsole(float dt) {
  const float s = sc(), W = (float)winW_, H = 148 * s, Y = (float)winH_ - H;
  ImDrawList* bgl = ImGui::GetBackgroundDrawList();
  gradRect(bgl, {0, Y - 24 * s}, {W, Y}, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150));   // shadow on the canvas
  if (!layer("##console", {0, Y}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradRect(dl, {0, Y}, {W, Y + H * 0.3f}, rgb(0x0f1f33), rgb(0x081320));
  gradRect(dl, {0, Y + H * 0.3f}, {W, Y + H}, rgb(0x081320), rgb(0x050c16));
  dl->AddRectFilled({0, Y}, {W, Y + 2 * s}, rgb(EDGE));
  hgradRect(dl, {0, Y}, {W / 2, Y + s}, IM_COL32(120, 200, 255, 0), IM_COL32(120, 200, 255, 153));
  hgradRect(dl, {W / 2, Y}, {W, Y + s}, IM_COL32(120, 200, 255, 153), IM_COL32(120, 200, 255, 0));
  const float top = Y + 2 * s, ch = H - 2 * s;

  // ── left: hologram projector + wave bar ──
  const float lw = 168 * s;
  gradRect(dl, {0, top}, {lw, top + ch}, rgb(0x04101c), rgb(0x02070d));
  float waveH = 26 * s, pBot = top + ch - waveH;
  drawPortrait(0, top, lw, pBot - top);
  dl->AddLine({0, pBot}, {lw, pBot}, rgb(0x1f3a58));
  gradRect(dl, {0, pBot + s}, {lw, top + ch}, rgb(0x071425), rgb(0x030912));
  {
    float cy = pBot + waveH / 2, x = 9 * s, base = cy + 5 * s;
    Txt l = T(11, MUTED, true, 1.5f), v = T(15, MINERAL, false, 0, true), m = T(11, DIM, false, 0, true);
    textB(dl, x, base, "WAVE", l);
    x += textW("WAVE", l) + 6 * s;
    std::string ws = std::to_string(wave_);
    textB(dl, x, base, ws.c_str(), v);
    x += textW(ws.c_str(), v) + 2 * s;
    std::string wm = mode_ == Mode::Campaign ? fmt("/%d", cfg::victory_wave) : "\xE2\x88\x9E";
    textB(dl, x, base, wm.c_str(), m);
    x += textW(wm.c_str(), m) + 6 * s;
    float bx1 = lw - 9 * s, by0 = cy - 2.5f * s;
    if (bx1 > x) {
      dl->AddRectFilled({x, by0}, {bx1, by0 + 5 * s}, rgb(0x06101b));
      float frac = mode_ == Mode::Endless ? 1.f : std::min(1.f, ((wave_ - 1) + (wavePhase_ == WavePhase::Combat ? 0 : 1)) / (float)cfg::victory_wave);
      if (frac > 0) {
        ImU32 c0 = mode_ == Mode::Endless ? rgb(0xb07a10) : rgb(0x2a7fd0), c1 = mode_ == Mode::Endless ? rgb(GOLD) : rgb(0x5cc8ff);
        hgradRect(dl, {x, by0}, {x + (bx1 - x) * frac, by0 + 5 * s}, c0, c1);
      }
      dl->AddRect({x, by0}, {bx1, by0 + 5 * s}, rgb(0x1c3654));
    }
  }
  auto sep = [&](float x) {
    gradRect(dl, {x, top}, {x + 2 * s, top + ch}, rgb(EDGE), rgb(0x2b5684, 0.2f));
  };
  sep(lw);

  // ── right: command card ──
  const float rw = 318 * s, rx0 = W - rw;
  sep(rx0 - 2 * s);
  drawCommandCard(rx0, top, rw, ch);

  // ── centre: bunker status ──
  const float cx0 = lw + 2 * s, cx1 = rx0 - 2 * s;
  gradRect(dl, {cx0, top}, {cx1, top + ch}, rgb(0x102036, 0.5f), rgb(0x060c16, 0.4f));
  // upgrade chips: 3 columns; 4 when all ten show (Endless with all research), as the web
  // console does — a 4th row would overflow the console
  struct Chip { const char* id; int lvl; bool show; const char* ico; const char* tip; };
  int en = endlessWaves();
  bool anyArmory = armory_[0] || armory_[1] || armory_[2] || armory_[3];
  bool od = shrikeOverdrive();
  std::string endTip;
  if (en) endTip = fmt("Endless reinforcements \xC3\x97%d: damage +%d%%, max HP +%d, repair +%d%%, magazine +%d", en, (int)std::round(cfg::endless_dmg * en * 100),
                       (int)(cfg::endless_hp * en), (int)std::round(cfg::endless_repair * en * 100), cfg::endless_ammo * en);
  if (anyArmory) {
    std::vector<std::string> b;
    if (armory_[0]) b.push_back(fmt("+%d HP", armory_[0] * (int)cfg::armory_hp));
    if (armory_[1]) b.push_back(fmt("+%d%% damage", (int)std::round(armory_[1] * cfg::armory_dmg * 100)));
    if (armory_[2]) b.push_back(fmt("+%d rounds", armory_[2] * cfg::armory_ammo));
    if (armory_[3]) b.push_back(fmt("cooldowns \xC3\x97%.2f", armoryCdMult(armory_[3])));
    std::string j;
    for (size_t i = 0; i < b.size(); i++) j += (i ? ", " : "") + b[i];
    endTip += (endTip.empty() ? "" : "\n") + std::string("Armory: ") + j;
  }
  std::string sensorTip = fmt("Sensor Tower \xE2\x80\x94 blips out to %g", (double)cfg::sensor_range[sensorLvl_]);
  Chip chips[] = {
    {"dmg", dmgLvl_, true, ui_icons::dmg[dmgLvl_], "Infantry weapons"},
    {"armor", armorLvl_, true, ui_icons::armor, "Armor"},
    {"hp", hpLvl_, true, ui_icons::neosteel, "Bunker HP"},
    {"ammo", ammoLvl_, true, ui_icons::ammo[ammoLvl_], "Ammo storage"},
    {"conc", concussiveLvl_, concussiveUnlocked_, ui_icons::concussive, "Concussive Shells"},
    {"snipe", snipeLvl_, snipeUnlocked_, ui_icons::snipe, "Snipe"},
    {"sensor", sensorLvl_, sensorLvl_ >= 1, nullptr, sensorTip.c_str()},   // placeholder glyph, no PNG yet
    {"superstim", superStimLvl_, superStimUnlocked_, ui_icons::superstim, "Super Stimpack"},
    {"endless", 3, en > 0 || anyArmory, nullptr, endTip.c_str()},
    {"shrike", 3, hpLvl_ >= 3, ui_icons::shrike, od ? "Shrike overdrive \xE2\x80\x94 six Marines inside: rate of fire \xC3\x97" "2.5" : "Shrike turret \xE2\x80\x94 auto-fire, firepower of 2 marines"},
  };
  int nShow = 0;
  for (auto& c : chips) nShow += c.show;
  const int chipCols = nShow > 9 ? 4 : 3;
  const float chipsW = (chipCols * 34 + (chipCols - 1) * 5) * s;
  float mx0 = cx0 + 14 * s, mx1 = cx1 - 14 * s - chipsW - 12 * s;
  float y = top + 9 * s;
  const Rank& rk = getRank(kills_);
  {   // head
    float cy = y + 9 * s;
    Txt nm = T(15, 0x9fd2ff, true, 2.5f);
    nm.glow = rgb(0x50aaff, 0.45f);
    textC(dl, mx0, cy, "TERRAN BUNKER", nm);
    float x = mx0 + textW("TERRAN BUNKER", nm) + 10 * s;
    Txt rb = T(12, rk.color, true, 1.5f);
    float bw = 7 * s + 11 * s + 4 * s + textW(rk.name, rb) + 7 * s;
    std::string ar = "ARMOR " + std::to_string(armorLvl_);
    Txt at = T(12, MUTED, true);
    float aw = textW(ar.c_str(), at), armorX = mx1 - aw - 17 * s;
    if (x + bw + 8 * s <= armorX) {   // a narrow console drops the rank badge (the top bar shows the rank too)
      dl->AddRect({x, cy - 8 * s}, {x + bw, cy + 8 * s}, alpha(rgb(rk.color), 0.4f));
      svgIcon(dl, "rank", {x + 7 * s + 5.5f * s, cy}, 11 * s, rgb(rk.color));
      textC(dl, x + 7 * s + 15 * s, cy, rk.name, rb);
    }
    if (mx0 + textW("TERRAN BUNKER", nm) + 8 * s <= armorX) {
      textC(dl, mx1 - aw, cy, ar.c_str(), at);
      image(dl, icon(ui_icons::armor), {armorX, cy - 6.5f * s}, {mx1 - aw - 4 * s, cy + 6.5f * s});
    }
    y += 18 * s + 6 * s;
  }
  auto bar = [&](float h, float pct, ImU32 fill, ImU32 glow, float tickFrac, ImU32 tickCol, const char* label, const std::string& val,
                 float lblPx, float valPx, bool pulse, float pulsePeriod, ImU32 valCol) {
    ImVec2 a(mx0, y), b(mx1, y + h);
    gradRect(dl, a, b, rgb(0x05090e), rgb(0x0a1018));
    float fw = (b.x - a.x) * glm::clamp(pct, 0.f, 1.f);
    if (fw > 0) {
      dl->AddRectFilled(a, {a.x + fw, b.y}, fill);
      gradRect(dl, a, {a.x + fw, a.y + h * 0.45f}, IM_COL32(255, 255, 255, 82), IM_COL32(255, 255, 255, 0));
      gradRect(dl, {a.x, a.y + h * 0.45f}, {a.x + fw, b.y}, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 46));
      glowRect(dl, a, {a.x + fw, b.y}, glow, 5 * s);
    }
    if (tickFrac > 0 && tickFrac < 1) {
      float step = (b.x - a.x) * tickFrac;
      if (step >= 2) for (float tx = a.x + step; tx < b.x - 0.5f; tx += step) dl->AddLine({tx - 0.5f, a.y}, {tx - 0.5f, b.y}, tickCol);
    }
    ImU32 border = rgb(0x233a52);
    if (pulse) {
      float k = 0.5f - 0.5f * std::cos(uiTime_ * 2 * 3.14159f / pulsePeriod);
      border = lerpCol(border, rgb(DANGER), k);
      glowRect(dl, a, b, alpha(rgb(0xff503c), 0.6f * k), 8 * s);
    }
    dl->AddRect(a, b, border);
    Txt l = TA(lblPx, IM_COL32(255, 255, 255, 204), true, 1.5f);
    l.shadow = true;
    textC(dl, a.x + 7 * s, (a.y + b.y) / 2, label, l);
    Txt v = TA(valPx, valCol, false, 0, true);
    v.shadow = true;
    textC(dl, b.x - 7 * s - textW(val.c_str(), v), (a.y + b.y) / 2, val.c_str(), v);
    y += h + 6 * s;
  };
  {
    float hpPct = glm::clamp(bunkerHp_ / maxHp_, 0.f, 1.f);
    ImU32 f = hpPct <= .25f ? rgb(0xe2392e) : hpPct <= .5f ? rgb(0xe2a21b) : rgb(0x2fd35a);
    ImU32 g = hpPct <= .25f ? rgb(0xe63c28, 0.5f) : hpPct <= .5f ? rgb(0xe6a01e, 0.4f) : rgb(0x28dc5a, 0.4f);
    float rr = bunkerHp_ < maxHp_ ? repairRate() : 0;   // repairing: show the stacked rate
    std::string v = fmt("%d / %d", (int)std::max(0.f, std::round(bunkerHp_)), (int)std::round(maxHp_)) + (rr > 0 ? fmt("  +%d/s", (int)std::round(rr)) : "");
    hudRect("hp", mx0, y, mx1, y + 16 * s);
    bar(16 * s, hpPct, f, g, 250 / maxHp_, IM_COL32(0, 0, 0, 115), "HP", v, 11, 12, hpPct <= .25f, 1.0f, IM_COL32_WHITE);
    float ap = maxAmmo_ ? std::min(1.f, (float)ammo_ / maxAmmo_) : 0;
    hudRect("ammo", mx0, y, mx1, y + 12 * s);
    bar(12 * s, ap, rgb(0xffae3c), rgb(0xffaa3c, 0.45f), 1.f / std::max(1, maxAmmo_), IM_COL32(0, 0, 0, 140), "AMMO",
        fmt("%d / %d", ammo_, maxAmmo_), 10, 11, ammo_ > 0 && ap <= .2f, 0.8f, ammo_ <= 0 ? rgb(DANGER) : IM_COL32_WHITE);
  }
  {   // slots + resources
    float cy = y + 10 * s, x = mx0;
    Txt l = T(11, MUTED, true, 1.5f);
    textC(dl, x, cy, "SLOTS", l);
    x += textW("SLOTS", l) + 14 * s;
    int used = 0;
    bool od = shrikeOverdrive();
    for (Unit u : roster_) {
      float pw = unitDef(u).slots == 2 ? 27 * s : 12 * s;
      ImU32 f = u == Unit::Marine ? rgb(0x1b4d9c) : u == Unit::Marauder ? rgb(0x7a3a06) : rgb(0x3d1a78);
      ImU32 b = u == Unit::Marine ? rgb(0x4a8be0) : u == Unit::Marauder ? rgb(0xd4761e) : rgb(0x9152e0);
      dl->AddRectFilled({x, cy - 6 * s}, {x + pw, cy + 6 * s}, f);
      dl->AddRect({x, cy - 6 * s}, {x + pw, cy + 6 * s}, b);
      x += pw + 3 * s;
      used += unitDef(u).slots;
    }
    for (int i = used; i < 6; i++) {
      dl->AddRectFilled({x, cy - 6 * s}, {x + 12 * s, cy + 6 * s}, IM_COL32(255, 255, 255, 10));
      dl->AddRect({x, cy - 6 * s}, {x + 12 * s, cy + 6 * s}, IM_COL32(255, 255, 255, 41));
      x += 15 * s;
    }
    if (hpLvl_ >= 3)
      for (int i = 0; i < 2; i++) {   // Shrike turret = 2 extra (automatic) marines
        float k = od ? 0.5f + 0.5f * std::sin(uiTime_ * 3.14159f / 0.5f) : 0;
        if (od) glowRect(dl, {x, cy - 6 * s}, {x + 12 * s, cy + 6 * s}, alpha(rgb(0xff3c28), 0.5f + 0.5f * k), (4 + 6 * k) * s);
        else glowRect(dl, {x, cy - 6 * s}, {x + 12 * s, cy + 6 * s}, rgb(0xffb81f, 0.6f), 5 * s);
        dl->AddRectFilled({x, cy - 6 * s}, {x + 12 * s, cy + 6 * s}, od ? rgb(0x7a1010) : rgb(0x6a4c00));
        dl->AddRect({x, cy - 6 * s}, {x + 12 * s, cy + 6 * s}, od ? rgb(0xff4a3a) : rgb(0xffb81f));
        x += 15 * s;
      }
    std::string cnt = fmt("%d/6", used) + (hpLvl_ >= 3 ? (od ? " +2 \xC3\x97" "2.5" : " +2") : "");
    Txt ct = T(12, MUTED, false, 0, true);
    textC(dl, x + 3 * s, cy, cnt.c_str(), ct);
    // resources (pop when they rise)
    int mi = (int)std::round(minerals_), ga = (int)std::round(gas_);
    if (mi > lastMin_) popMin_ = 0.35f;
    if (ga > lastGas_) popGas_ = 0.35f;
    lastMin_ = mi;
    lastGas_ = ga;
    popMin_ = std::max(0.f, popMin_ - dt);
    popGas_ = std::max(0.f, popGas_ - dt);
    float rxr = mx1;
    auto res = [&](const char* id, int v, unsigned col, const char* ic, float pop, float glowA) {
      Txt t = T(15, col, false, 0, true);
      float k = pop / 0.35f;
      t.col = lerpCol(rgb(col), IM_COL32_WHITE, 0.45f * k);
      std::string vs = std::to_string(v);
      float tw = textW(vs.c_str(), t);
      rxr -= tw;
      textC(dl, rxr, cy, vs.c_str(), t);
      float isz = 20 * s * (1 + 0.25f * k);
      hudRect(id, rxr - 25 * s, cy - 10 * s, rxr + tw, cy + 10 * s);
      rxr -= 5 * s + 20 * s;
      ImVec2 c(rxr + 10 * s, cy);
      dl->AddCircleFilled(c, 9 * s, alpha(rgb(col), glowA * 0.35f));
      image(dl, icon(ic), {c.x - isz / 2, c.y - isz / 2}, {c.x + isz / 2, c.y + isz / 2});
      rxr -= 14 * s;
    };
    res("res-gas", ga, GAS, ui_icons::gas, popGas_, 0.7f);
    res("res-min", mi, MINERAL, ui_icons::mineral, popMin_, 0.7f);
    y += 20 * s + 6 * s;
  }
  {   // status line
    Txt st = T(13, WARN, true, 0.4f);
    std::string sv = ellipsize(status_, mx1 - mx0, st);
    rich(dl, {mx0, y}, sv.c_str(), st);
  }

  // upgrade chips (centred vertically; built above, with the column count)
  int rows = (nShow + chipCols - 1) / chipCols;
  float gx0 = cx1 - 14 * s - chipsW, gy0 = top + (ch - (rows * 34 * s + (rows - 1) * 5 * s)) / 2;
  int k = 0;
  for (auto& c : chips) {
    if (!c.show) continue;
    float x = gx0 + (k % chipCols) * 39 * s, yy = gy0 + (k / chipCols) * 39 * s;
    k++;
    ImVec2 a(x, yy), b(x + 34 * s, yy + 34 * s);
    bool maxed = c.lvl >= 3, active = c.lvl > 0;
    float op = active ? 1 : 0.4f;
    ImU32 border = maxed ? rgb(0xb08a10) : active ? rgb(0x3a6aaa) : rgb(0x1d3048);
    bool isOd = !strcmp(c.id, "shrike") && od;
    if (isOd) border = lerpCol(rgb(0xa02a20), rgb(0xff5a48), 0.5f + 0.5f * std::sin(uiTime_ * 3.14159f / 0.5f));
    cutPanel(dl, a, b, 5 * s, alpha(rgb(0x0b1726), op), alpha(rgb(0x050b14), op), alpha(border, op));
    if (isOd) dl->AddRectFilled({a.x + 2 * s, a.y + 2 * s}, {b.x - 2 * s, b.y - 2 * s}, rgb(0xff3c28, 0.15f + 0.15f * std::sin(uiTime_ * 6.28f)));
    bool hasPips = strcmp(c.id, "shrike") && strcmp(c.id, "endless");
    ImVec2 ctr((a.x + b.x) / 2, (a.y + b.y) / 2 - (hasPips ? 3 * s : 0));
    if (!strcmp(c.id, "endless")) {
      svgIcon(dl, "endless", {ctr.x, ctr.y - 4 * s}, 16 * s, rgb(GOLD));
      if (en) {
        std::string n = "+" + std::to_string(en);
        Txt nt = T(10, GOLD, false, 0, true);
        text(dl, {ctr.x - textW(n.c_str(), nt) / 2, ctr.y + 5 * s}, n.c_str(), nt);
      }
    } else if (!strcmp(c.id, "sensor")) {   // the stroke glyph until the Sensor Tower PNG is exported
      svgIcon(dl, "sensor", ctr, 16 * s, alpha(rgb(maxed ? GOLD : 0x9fd2ff), op));
    } else {
      float isz = (hasPips ? 18 : 24) * s;
      image(dl, icon(c.ico), {ctr.x - isz / 2, ctr.y - isz / 2}, {ctr.x + isz / 2, ctr.y + isz / 2}, IM_COL32(255, 255, 255, (int)(255 * op)));
    }
    if (hasPips)
      for (int i = 0; i < 3; i++) {
        float px = ctr.x - 10 * s + i * 8 * s, py = ctr.y + 9 * s + 3 * s;
        bool on = i < c.lvl;
        ImU32 pc = on ? (maxed ? rgb(GOLD) : rgb(0x5aa2ff)) : rgb(0x1c2c40);
        dl->AddRectFilled({px, py}, {px + 6 * s, py + 3 * s}, alpha(pc, op));
      }
    if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered() && c.tip && *c.tip) ImGui::SetTooltip("%s", c.tip);
  }
  ImGui::End();
}

// Hologram: the bunker's feature edges projected onto the console's portrait.
// Extracting the edges of the dense SC2 meshes takes tens of ms, so it runs on
// a worker from copies of the geometry; drawPortrait swaps the result in.
struct HoloJob {
  std::atomic<bool> ready{false};
  std::vector<glm::vec3> base, upg, tur;
  glm::vec2 pivot{0};
};

void Game::buildPortrait() {
  struct In {
    std::vector<std::pair<bool, Geometry>> bunker;   // (is the Neosteel frame, geometry)
    glm::mat4 bw{1}, tm{1};
    std::vector<Geometry> tur;
  };
  auto in = std::make_shared<In>();
  if (bunkerModel_) {
    in->bw = bunkerModel_->wrapper();
    for (const ModelPart& p : bunkerParts_) in->bunker.push_back({p.name == "Mesh.002", p.cpu});
    if (shrikeModel_) {   // the turret seated on the roof
      in->tm = glm::translate(glm::mat4(1), {0, shrikeMountY(bunkerParts_, in->bw), 0}) * shrikeModel_->wrapper();
      for (const ModelPart& p : shrikeParts_) in->tur.push_back(p.cpu);
    }
  }
  auto job = std::make_shared<HoloJob>();
  holoJob_ = job;   // a newer build replaces a pending one
  jobs::submit([in, job] {
    for (auto& [frame, g] : in->bunker) {
      auto e = featureEdges(g, in->bw, 5);
      auto& dst = frame ? job->upg : job->base;
      dst.insert(dst.end(), e.begin(), e.end());
    }
    for (auto& g : in->tur) {   // dense model: coarser edge angle
      auto e = featureEdges(g, in->tm, 12);
      job->tur.insert(job->tur.end(), e.begin(), e.end());
    }
    if (job->base.empty() && job->upg.empty()) job->base = featureEdges(geo::box(1.4f, 0.9f, 1.4f), glm::mat4(1), 1);
    // centre + scale so every stage (base + frame + turret) fills the view without the framing jumping
    glm::vec3 mn(1e30f), mx(-1e30f);
    for (auto* a : {&job->base, &job->upg, &job->tur}) for (auto& v : *a) { mn = glm::min(mn, v); mx = glm::max(mx, v); }
    glm::vec3 c = (mn + mx) * 0.5f, sz = mx - mn;
    float k = 2.2f / std::max({sz.x, sz.y, sz.z, 1e-6f});
    job->pivot = {-c.x * k, -c.z * k};
    for (auto* a : {&job->base, &job->upg, &job->tur})
      for (auto& v : *a) v = {(v.x - c.x) * k, (v.y - c.y) * k + 0.25f, (v.z - c.z) * k};
    job->ready = true;
  });
}

void Game::updatePortraitBunker(bool instant) {
  holoUpgT_ = hpLvl_ >= 2 ? 1.f : 0.f;   // Lv2: Neosteel frame
  holoTurT_ = hpLvl_ >= 3 ? 1.f : 0.f;   // Lv3: Shrike turret
  if (instant) { holoUpgA_ = holoUpgT_; holoTurA_ = holoTurT_; }
}

void Game::drawPortrait(float x0, float y0, float w, float h) {
  const float s = sc(), dt = ImGui::GetIO().DeltaTime;
  if (holoJob_ && holoJob_->ready) {   // edges from the worker
    holoBase_ = std::move(holoJob_->base);
    holoUpg_ = std::move(holoJob_->upg);
    holoTur_ = std::move(holoJob_->tur);
    holoTurPivot_ = holoJob_->pivot;
    holoJob_.reset();
    holoBuilt_ = true;
    updatePortraitBunker(true);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  radial(dl, {x0 + w / 2, y0 + h * 0.16f}, {w * 1.2f, h * 0.8f}, rgb(0x005a87, 0.22f), rgb(0x005a87, 0.0f), 0.0f);
  float labelH = 15 * s;
  ImVec2 a(x0 + 5 * s, y0 + 5 * s), b(x0 + w - 5 * s, y0 + h - labelH);
  radial(dl, {(a.x + b.x) / 2, a.y + (b.y - a.y) * 0.62f}, {(b.x - a.x) * 0.82f, (b.y - a.y) * 0.72f}, rgb(0x004669, 0.3f), rgb(0x004669, 0), 0);
  dl->PushClipRect(a, b, true);
  // the projector: edges spinning slowly, frame and turret materialising on upgrade
  holoAngle_ += dt * 0.45f;
  float spd = dt * 3.2f;
  auto ease = [&](float v, float t) { return v < t ? std::min(t, v + spd) : v > t ? std::max(t, v - spd * 1.6f) : v; };
  holoUpgA_ = ease(holoUpgA_, holoUpgT_);
  holoTurA_ = ease(holoTurA_, holoTurT_);
  float cw = 120 * s, chh = 80 * s, cx = (a.x + b.x) / 2 - cw / 2, cy = (a.y + b.y) / 2 - chh / 2;
  glm::mat4 vp = glm::perspective(glm::radians(45.f), 1.5f, 0.1f, 50.f) * glm::lookAt(glm::vec3(1.6f, 1.2f, 1.6f), glm::vec3(0), glm::vec3(0, 1, 0));
  auto stroke = [&](const std::vector<glm::vec3>& segs, const glm::mat4& m, ImU32 col, float al) {
    glm::mat4 mvp = vp * m;
    for (int pass = 0; pass < 2; pass++) {
      ImU32 c = pass == 0 ? alpha(rgb(0x2ad6ff), 0.18f * al) : alpha(col, al);
      float th = (pass == 0 ? 3.0f : 0.8f) * s;
      for (size_t i = 0; i + 1 < segs.size(); i += 2) {
        glm::vec4 p0 = mvp * glm::vec4(segs[i], 1), p1 = mvp * glm::vec4(segs[i + 1], 1);
        if (p0.w <= 0 || p1.w <= 0) continue;   // behind the projector
        dl->AddLine({cx + (p0.x / p0.w * 0.5f + 0.5f) * cw, cy + (0.5f - p0.y / p0.w * 0.5f) * chh},
                    {cx + (p1.x / p1.w * 0.5f + 0.5f) * cw, cy + (0.5f - p1.y / p1.w * 0.5f) * chh}, c, th);
      }
    }
  };
  glm::mat4 rot = glm::rotate(glm::mat4(1), holoAngle_, {0, 1, 0});
  if (holoBuilt_) {
    stroke(holoBase_, rot, rgb(0x00ff88), 0.95f);
    float pulse = 0.80f + 0.20f * std::sin(uiTime_ * 5);
    if (!holoUpg_.empty() && holoUpgA_ > 0.001f)   // Neosteel frame: blue (Lv2) / gold (Lv3)
      stroke(holoUpg_, glm::translate(rot, {0, (1 - holoUpgA_) * 0.1f, 0}), rgb(hpLvl_ >= 3 ? 0xffd24a : 0x44aaff), holoUpgA_ * pulse);
    if (!holoTur_.empty() && holoTurA_ > 0.001f) {   // Shrike turret: tracks its target like the real one
      glm::mat4 m = rot * glm::translate(glm::mat4(1), {holoTurPivot_.x, (1 - holoTurA_) * 0.12f, holoTurPivot_.y}) *
                    glm::rotate(glm::mat4(1), shrikeYaw_, {0, 1, 0}) * glm::translate(glm::mat4(1), {-holoTurPivot_.x, 0, -holoTurPivot_.y});
      stroke(holoTur_, m, rgb(0xffd24a), holoTurA_ * pulse);
    }
  }
  // scanlines + moving scan band
  for (float yy = a.y + 3 * s; yy < b.y; yy += 4 * s) dl->AddLine({a.x, yy}, {b.x, yy}, IM_COL32(0, 0, 0, 46), s);
  float band = std::fmod(uiTime_, 3.4f) / 3.4f, by = a.y - 14 * s + band * (b.y - a.y + 14 * s);
  gradRect(dl, {a.x, by}, {b.x, by + 7 * s}, IM_COL32(85, 222, 255, 0), IM_COL32(85, 222, 255, 41));
  gradRect(dl, {a.x, by + 7 * s}, {b.x, by + 14 * s}, IM_COL32(85, 222, 255, 41), IM_COL32(85, 222, 255, 0));
  dl->PopClipRect();
  dl->AddRect(a, b, rgb(0x2e80b4, 0.45f));
  // corners
  ImU32 cc = rgb(0x5acdff, 0.72f);
  float L = 11 * s, t2 = 2 * s, i3 = 3 * s;
  auto corner = [&](float x, float y, float dx, float dy) {
    dl->AddLine({x, y}, {x + dx * L, y}, cc, t2);
    dl->AddLine({x, y}, {x, y + dy * L}, cc, t2);
  };
  corner(a.x + i3, a.y + i3, 1, 1);
  corner(b.x - i3, a.y + i3, -1, 1);
  corner(a.x + i3, b.y - i3, 1, -1);
  corner(b.x - i3, b.y - i3, -1, -1);
  // status + tier dots
  const char* stx = hpLvl_ >= 3 ? "\xE2\x97\x88 FORTIFIED" : hpLvl_ >= 1 ? "\xE2\x97\x88 REINFORCED" : "\xE2\x97\x88 ONLINE";
  unsigned stc = hpLvl_ >= 3 ? 0xffd24a : hpLvl_ >= 1 ? 0x66ddff : 0x44ff88;
  Txt st = T(10, stc, true, 1.5f);
  st.glow = rgb(stc, 0.6f);
  text(dl, {(a.x + b.x) / 2 - textW(stx, st) / 2, a.y + 3 * s}, stx, st);
  for (int i = 0; i < 3; i++) {
    ImVec2 c((a.x + b.x) / 2 + (i - 1) * 10 * s, b.y - 7 * s);
    bool on = i < hpLvl_;
    if (on) dl->AddCircleFilled(c, 6 * s, rgb(0x3aaaff, 0.3f));
    dl->AddCircleFilled(c, 3 * s, on ? rgb(0x66ddff) : rgb(0x14283c, 0.7f));
    dl->AddCircle(c, 3 * s, on ? rgb(0x9be7ff) : rgb(0x5f9bcd, 0.5f));
  }
  Txt lb = T(11, 0x7fd8ff, true, 2.5f);
  lb.glow = rgb(0x3cb4ff, 0.6f);
  text(dl, {x0 + w / 2 - textW("COMMAND BUNKER", lb) / 2, y0 + h - labelH + 2 * s}, "COMMAND BUNKER", lb);
}

// ─── Command card ────────────────────────────────────────────────────────────
struct CmdInfo { const char *t, *k, *d; };
static const std::map<std::string, CmdInfo> CMD_INFO = {
  {"air", {"AIRSTRIKE", "R", "A Viking fires a missile at every enemy cluster and drops an ammo crate by the bunker \xE2\x80\x94 your reload once the magazine is empty."}},
  {"stim", {"STIMPACK", "T", "Unlocked by six Marines in the bunker (fewer with Super Stimpack's tiers: 4, 2, 1). For 11 s the Marines and Marauders fire 50% faster (an extra round every other volley, no extra ammo). Costs 10 HP per Marine and 20 per Marauder, paid by the bunker \xE2\x80\x94 free and repairing with the Super Stimpack research, which also lets you right-click it for autocast: it stims whenever you fire (a gold light runs round the button)."}},
  {"nuke", {"NUKE", "N", "Tactical nuke on the centre of the swarm after a 2-second warning. Wipes out everything in a huge radius."}},
  {"roster", {"BUNKER ROSTER", "B", "Hire Marines, Marauders and Ghosts (6 slots) and research their upgrades. In Endless its armory sells more HP, damage, ammo and shorter strike / nuke cooldowns. The game pauses while it is open."}},
  {"view", {"RESET VIEW", "HOME", "Back to the overview. Mouse wheel or + / \xE2\x88\x92 zooms in on the bunker; Insert / Delete rotate the view 90\xC2\xB0."}},
  {"menu", {"MENU", "M", "Save / load, graphics and sound, the controls, the tutorial, the main menu. The game pauses while it is open."}},
  {"dmg", {"WEAPON DAMAGE", "1", "Every round from every unit hits harder."}},
  {"armor", {"BUNKER ARMOR", "2", "Reduces the damage Zerg deal to the bunker (bites and acid)."}},
  {"hp", {"BUNKER HULL", "3", "+250 max HP per level. Lv2 adds the Neosteel frame; Lv3 mounts the Shrike auto-turret (= 2 Marines, no ammo) and self-repair (25 HP/s, stacks with the Super Stimpack). Six Marines inside overdrive the turret: \xC3\x97" "2.5 rate of fire."}},
  {"ammo", {"AMMO STORAGE", "4", "+10 rounds per level. One round = one volley from every unit in the bunker."}},
};

void Game::drawCommandCard(float x0, float y0, float w, float h) {
  const float s = sc();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradRect(dl, {x0, y0}, {x0 + w, y0 + h}, rgb(0x071222), rgb(0x030810));
  float th = 18 * s;
  hgradRect(dl, {x0, y0}, {x0 + w, y0 + th}, rgb(0x285082, 0.6f), rgb(0x142846, 0.1f));
  dl->AddLine({x0, y0 + th - 0.5f}, {x0 + w, y0 + th - 0.5f}, rgb(EDGE));
  textC(dl, x0 + 9 * s, y0 + th / 2, "COMMAND", T(11, 0x8fc5f5, true, 2.5f));
  Txt hd = T(10, DIM, true, 1);
  textC(dl, x0 + w - 9 * s - textW("hover for details", hd), y0 + th / 2, "hover for details", hd);

  float gx = x0 + 6 * s, gy = y0 + th + 5 * s, gw = w - 12 * s, gh = h - th - 11 * s;
  float cw = (gw - 4 * 4 * s) / 5, chh = (gh - 4 * s) / 2;
  hudRect("cmd-grid", gx, gy, gx + gw, gy + gh);
  bool anyHover = false;
  bool camp = mode_ == Mode::Campaign;
  (void)camp;
  struct C {
    const char* id;
    const char* key;
    const char* label;
    const char* img;     // SC2 art (assets/ui) or null
    const char* svg;     // stroke icon or null
    bool disabled, ready, researched, stimOn;
    float cd;            // 0..1 sweep
    std::string cdText;
    int lvl = -1;        // upgrade pips (-1 = none)
    int cost[2] = {0, 0};
    bool hasCost = false, maxed = false;
  };
  std::vector<C> cs;
  bool airBlocked = airCD_ > 0 || ammo_ > 0 || wavePhase_ != WavePhase::Combat;
  cs.push_back({"air", "R", "STRIKE", ui_icons::fleet, nullptr, airBlocked, !airBlocked, false, false, airCD_ > 0 ? airCD_ / strikeCooldown() : 0,
                airCD_ > 0 ? std::to_string((int)std::ceil(airCD_)) : ""});
  bool nukeLocked = wave_ < cfg::nuke_wave, nukeReady = !nukeLocked && nukeCD_ == 0 && !nukeInFlight_;
  cs.push_back({"nuke", "N", "NUKE", ui_icons::nuke, nullptr, !nukeReady, nukeReady, false, false,
                nukeLocked ? 1 : nukeCD_ > 0 ? nukeCD_ / nukeCooldown() : 0,
                nukeLocked ? "W" + std::to_string(cfg::nuke_wave) : nukeCD_ > 0 ? std::to_string((int)std::ceil(nukeCD_)) : ""});
  bool rosterReady = mode_ == Mode::Endless && wavePhase_ != WavePhase::Combat && phase_ == Phase::Playing && armoryAffordable();
  cs.push_back({"roster", "B", "ROSTER", nullptr, "roster", false, rosterReady, false, false, 0, ""});
  cs.push_back({"view", "HOME", "VIEW", ui_icons::detector, nullptr, false, false, false, false, 0, ""});
  cs.push_back({"menu", "M", "MENU", nullptr, "menu", false, false, false, false, 0, ""});
  struct U { const char *id, *key, *label, *img; int lvl; const int (*costs)[2]; };
  U ups[4] = {{"dmg", "1", "DAMAGE", ui_icons::dmg[dmgLvl_], dmgLvl_, cfg::dmg_costs},
              {"armor", "2", "ARMOR", ui_icons::armor, armorLvl_, cfg::armor_costs},
              {"hp", "3", "BUNKER", ui_icons::neosteel, hpLvl_, cfg::hp_costs},
              {"ammo", "4", "AMMO", ui_icons::ammo[ammoLvl_], ammoLvl_, cfg::ammo_costs}};
  for (U& u : ups) {
    bool maxed = u.lvl >= 3;
    bool shrt = !maxed && (minerals_ < u.costs[u.lvl][0] || gas_ < u.costs[u.lvl][1]);
    C c{u.id, u.key, u.label, u.img, nullptr, maxed || shrt, false, maxed, false, 0, ""};
    c.lvl = u.lvl;
    c.hasCost = true;
    c.maxed = maxed;
    if (!maxed) { c.cost[0] = u.costs[u.lvl][0]; c.cost[1] = u.costs[u.lvl][1]; }
    cs.push_back(c);
  }
  bool stimOn = stimT_ > 0;
  bool stimReady = stimUnlocked() && !stimOn && (!stimCost() || bunkerHp_ > stimCost()) && wavePhase_ == WavePhase::Combat;
  cs.push_back({"stim", "T", "STIM", superStimUnlocked_ ? ui_icons::superstim : nullptr, superStimUnlocked_ ? nullptr : "stim", !stimReady && !stimOn, stimReady,
                false, stimOn, stimOn ? stimT_ / cfg::stim_duration : !stimUnlocked() ? 1.f : 0.f,
                stimOn ? std::to_string((int)std::ceil(stimT_)) : !stimUnlocked() ? fmt("%dM", stimNeed()) : ""});

  float pulse = 0.5f - 0.5f * std::cos(uiTime_ * 2 * 3.14159f / 1.4f);
  for (int i = 0; i < (int)cs.size(); i++) {
    C& c = cs[i];
    float x = gx + (i % 5) * (cw + 4 * s), y = gy + (i / 5) * (chh + 4 * s);
    hudRect(std::string("cmd-") + c.id, x, y, x + cw, y + chh);
    ImGui::SetCursorScreenPos({x, y});
    Btn b = button((std::string("##cmd_") + c.id).c_str(), {cw, chh});
    bool hov = b.hovered && !c.disabled;
    if (b.hovered) { anyHover = true; tipCmd_ = c.id; }
    float press = (b.held && !c.disabled) ? 1 * s : 0;
    ImVec2 a(b.a.x, b.a.y + press), e(b.b.x, b.b.y + press);
    float dim = c.disabled ? 0.62f : 1.f;
    auto D = [&](ImU32 col) {   // filter: saturate(.35) brightness(.62) when disabled
      if (!c.disabled) return col;
      ImVec4 v = ImGui::ColorConvertU32ToFloat4(col);
      float l = 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
      v.x = (l + (v.x - l) * 0.35f) * dim; v.y = (l + (v.y - l) * 0.35f) * dim; v.z = (l + (v.z - l) * 0.35f) * dim;
      return ImGui::ColorConvertFloat4ToU32(v);
    };
    ImU32 border = c.researched ? rgb(0xb08a10) : c.ready ? rgb(WARN) : c.stimOn ? rgb(0xff5a3a) : hov ? rgb(EDGE_HI) : rgb(EDGE);
    if (c.ready) glowRect(dl, a, e, rgb(0xffb43c, 0.25f + 0.5f * pulse), (4 + 10 * pulse) * s);
    else if (c.stimOn) glowRect(dl, a, e, rgb(0xff5a3a, 0.75f), 10 * s);
    else if (hov) glowRect(dl, a, e, rgb(0x46a0ff, 0.45f), 10 * s);
    cutPanel(dl, a, e, 7 * s, D(hov ? rgb(0x25528a) : rgb(0x183558)), D(hov ? rgb(0x122a4a) : rgb(0x0b1a2e)), D(border));
    gradRect(dl, {a.x + 7 * s, a.y + 1}, {e.x - 1, a.y + (e.y - a.y) * 0.42f}, D(IM_COL32(150, 205, 255, hov ? 64 : 33)), IM_COL32(150, 205, 255, 0));
    dl->AddLine({a.x + 7 * s, a.y + 1.5f}, {e.x - 1, a.y + 1.5f}, D(IM_COL32(160, 215, 255, 64)));
    // icon + label (shifted up when a cost line is shown)
    float labelH = 12 * s, iconSz = c.img ? 32 * s : 24 * s, gap = 1 * s;
    float total = iconSz + gap + labelH + (c.hasCost ? 9 * s : 0);
    float iy = (a.y + e.y) / 2 - total / 2, icx = (a.x + e.x) / 2;
    ImU32 iconCol = c.ready ? rgb(0xffd27a) : c.stimOn ? rgb(0xff8a6a) : rgb(0xbfe0ff);
    if (c.img) {
      image(dl, icon(c.img), {icx - iconSz / 2, iy}, {icx + iconSz / 2, iy + iconSz}, D(IM_COL32_WHITE));
      dl->AddRect({icx - iconSz / 2, iy}, {icx + iconSz / 2, iy + iconSz}, D(IM_COL32(140, 200, 255, 89)), 2 * s);
    } else {
      dl->AddCircleFilled({icx, iy + iconSz / 2}, iconSz * 0.55f, D(alpha(c.ready ? rgb(0xffbe50) : rgb(0x5ab4ff), 0.12f)));
      svgIcon(dl, c.svg, {icx, iy + iconSz / 2}, iconSz, D(iconCol));
    }
    Txt lt = TA(11, D(c.researched ? rgb(GOLD) : rgb(0xa8d4ff)), true, 1);
    lt.glow = D(rgb(0x50aaff, 0.4f));
    text(dl, {icx - textW(c.label, lt) / 2, iy + iconSz + gap}, c.label, lt);
    Txt kt = TA(10, D(rgb(0xffd27a)), false, 0, true);
    kt.shadow = true;
    text(dl, {a.x + 4 * s, a.y + 2 * s}, c.key, kt);
    if (c.lvl >= 0)
      for (int p = 0; p < 3; p++) {
        float px = e.x - 5 * s - (3 - p) * 6 * s + 2 * s, py = a.y + 4 * s;
        bool on = p < c.lvl;
        ImU32 fc = on ? (c.researched ? rgb(GOLD) : rgb(0x5aa2ff)) : rgb(0x16283e);
        ImU32 bc = on ? (c.researched ? rgb(0xffe48a) : rgb(0x8cc4ff)) : rgb(0x2b4a6c);
        dl->AddRectFilled({px, py}, {px + 4 * s, py + 7 * s}, D(fc));
        dl->AddRect({px, py}, {px + 4 * s, py + 7 * s}, D(bc));
      }
    if (c.hasCost) {
      Txt mt = TA(10, 0, false, 0, true);
      if (c.maxed) {
        mt.col = D(rgb(GOLD));
        text(dl, {icx - textW("MAX", mt) / 2, e.y - 2 * s - 11 * s}, "MAX", mt);
      } else {
        std::string ms = std::to_string(c.cost[0]), gs = std::to_string(c.cost[1]);
        Txt m1 = mt, g1 = mt;
        m1.col = D(minerals_ < c.cost[0] ? rgb(DANGER) : rgb(MINERAL));
        g1.col = D(gas_ < c.cost[1] ? rgb(DANGER) : rgb(GAS));
        float tw = textW(ms.c_str(), m1) + 5 * s + textW(gs.c_str(), g1), tx = icx - tw / 2, ty = e.y - 2 * s - 11 * s;
        text(dl, {tx, ty}, ms.c_str(), m1);
        text(dl, {tx + textW(ms.c_str(), m1) + 5 * s, ty}, gs.c_str(), g1);
      }
    }
    if (c.cd > 0 || !c.cdText.empty()) {   // radial cooldown sweep
      dl->PushClipRect(a, e, true);
      dl->AddRectFilled(a, e, IM_COL32(0, 0, 0, 38));
      pie(dl, a, e, glm::clamp(c.cd, 0.f, 1.f), IM_COL32(0, 0, 0, 135));
      dl->PopClipRect();
      if (!c.cdText.empty()) {
        Txt ct = T(16, 0xffffff, false, 0, true);
        ct.shadow = true;
        text(dl, {icx - textW(c.cdText.c_str(), ct) / 2, (a.y + e.y) / 2 - textH(ct) / 2}, c.cdText.c_str(), ct);
      }
    }
    bool stimBtn = !strcmp(c.id, "stim");
    if (stimBtn && stimAuto_) autocastBorder(dl, a, e, 7 * s, uiTime_, rgb(0xffd040));
    if (stimBtn && b.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) toggleStimAuto();   // SC2: right-click = autocast
    if (b.clicked) {
      std::string id = c.id;
      if (id == "air") doAirstrike();
      else if (id == "nuke") doNuke();
      else if (id == "roster") toggleRoster();
      else if (id == "view") resetView();
      else if (id == "menu") openMenu();
      else if (id == "stim") doStim();
      else buy(id);
    }
  }
  if (!anyHover) tipCmd_.clear();
}

// Tooltip above the command card: cost, level, why it is disabled
void Game::drawTooltip() {
  static float vis = 0;
  float dt = ImGui::GetIO().DeltaTime;
  if (tutOpen_) vis = 0;   // it covered the tutorial box on narrow windows
  vis = glm::clamp(vis + (tipCmd_.empty() || tutOpen_ ? -dt : dt) / 0.12f, 0.f, 1.f);
  static std::string shown;
  if (!tipCmd_.empty()) shown = tipCmd_;
  if (vis <= 0 || shown.empty()) return;
  auto it = CMD_INFO.find(shown);
  if (it == CMD_INFO.end()) return;
  const CmdInfo& info = it->second;
  const float s = sc(), w = 280 * s, pad = 12 * s;
  std::string lv, fx, warn;
  bool ok = false, hasCost = false;
  int cm = 0, cg = 0;
  int lvl = -1;
  const int (*costs)[2] = nullptr;
  if (shown == "dmg") { lvl = dmgLvl_; costs = cfg::dmg_costs; }
  if (shown == "armor") { lvl = armorLvl_; costs = cfg::armor_costs; }
  if (shown == "hp") { lvl = hpLvl_; costs = cfg::hp_costs; }
  if (shown == "ammo") { lvl = ammoLvl_; costs = cfg::ammo_costs; }
  if (lvl >= 0) {
    int l = lvl;
    lv = fmt("Lv %d/3", l);
    if (shown == "dmg") fx = fmt("Damage \xC3\x97%.1f", 1 + l * .6f) + (l < 3 ? fmt(" \xE2\x86\x92 \xC3\x97%.1f", 1 + (l + 1) * .6f) : "");
    if (shown == "armor") fx = fmt("Damage taken \xC3\x97%.2f", 1 / (1 + l * .35f)) + (l < 3 ? fmt(" \xE2\x86\x92 \xC3\x97%.2f", 1 / (1 + (l + 1) * .35f)) : "");
    if (shown == "hp") { const char* n[4] = {"Next: +250 HP", "Next: +250 HP \xC2\xB7 Neosteel frame", "Next: +250 HP \xC2\xB7 Shrike turret \xC2\xB7 self-repair", ""}; fx = n[l]; }
    if (shown == "ammo") fx = l < 3 ? fmt("%d \xE2\x86\x92 %d rounds", maxAmmo_, maxAmmo_ + 10) : "";
    int en = endlessWaves();
    std::string extra;
    if (en) {
      if (shown == "dmg") extra = fmt("+%d%% damage", (int)std::round(cfg::endless_dmg * en * 100));
      if (shown == "hp") extra = fmt("+%d max HP, repair +%d%%", (int)(cfg::endless_hp * en), (int)std::round(cfg::endless_repair * en * 100));
      if (shown == "ammo") extra = fmt("+%d rounds", cfg::endless_ammo * en);
    }
    if (!extra.empty()) fx += (fx.empty() ? "" : "\n") + std::string("Endless reinforcements: ") + extra;
    int ak = shown == "hp" ? 0 : shown == "dmg" ? 1 : shown == "ammo" ? 2 : -1;
    if (ak >= 0 && armory_[ak]) {
      int n = armory_[ak];
      std::string tot = ak == 0 ? fmt("+%d HP", n * (int)cfg::armory_hp) : ak == 1 ? fmt("+%d%% damage", (int)std::round(n * cfg::armory_dmg * 100)) : fmt("+%d rounds", n * cfg::armory_ammo);
      fx += (fx.empty() ? "" : "\n") + std::string("Armory: ") + tot;
    }
    if (l >= 3) warn = "FULLY UPGRADED";
    else {
      cm = costs[l][0];
      cg = costs[l][1];
      hasCost = true;
      if (minerals_ < cm || gas_ < cg) {
        int nm = std::max(0, (int)std::ceil(cm - minerals_)), ng = std::max(0, (int)std::ceil(cg - gas_));
        warn = "Need " + (nm ? "\xE2\x97\x86" + std::to_string(nm) : std::string()) + (nm && ng ? " " : "") + (ng ? "\xE2\x96\xB2" + std::to_string(ng) : std::string()) + " more";
      }
    }
  }
  if (shown == "air") {
    if (airCD_ > 0) warn = fmt("Recharging \xE2\x80\x94 %d s", (int)std::ceil(airCD_));
    else if (wavePhase_ != WavePhase::Combat) warn = "Only during a wave";
    else if (ammo_ > 0) warn = fmt("Only when out of ammo (%d left)", ammo_);
    else { warn = "READY"; ok = true; }
  }
  if (shown == "stim") {
    if (!stimUnlocked()) warn = fmt("Needs %d Marines in the bunker (%d/%d)", stimNeed(), (int)std::count(roster_.begin(), roster_.end(), Unit::Marine), stimNeed());
    else if (stimT_ > 0) warn = fmt("Stimmed \xE2\x80\x94 %d s left", (int)std::ceil(stimT_));
    else if (stimCost() && bunkerHp_ <= stimCost()) warn = fmt("Not enough HP (%d needed)", (int)stimCost());
    else if (superStimUnlocked_ && wavePhase_ == WavePhase::Combat) { warn = std::string("READY \xE2\x80\x94 Super Stimpack: no HP cost, repairs the bunker") + (hpLvl_ >= 3 ? " (stacks with self-repair)" : ""); ok = true; }
    else if (wavePhase_ != WavePhase::Combat) warn = "Only during a wave";
    else { warn = "READY"; ok = true; }
    if (superStimUnlocked_) warn += stimAuto_ ? "\nAutocast ON \xE2\x80\x94 stims as you fire (right-click: off)" : "\nRight-click: autocast";
  }
  if (shown == "nuke") {
    if (wave_ < cfg::nuke_wave) warn = fmt("Unlocks at wave %d", cfg::nuke_wave);
    else if (nukeInFlight_) warn = "Launch in progress";
    else if (nukeCD_ > 0) warn = fmt("Recharging \xE2\x80\x94 %d s", (int)std::ceil(nukeCD_));
    else { warn = "READY"; ok = true; }
  }
  // measure
  Txt tt = T(16, 0xe6f3ff, true, 1.5f), dt2 = T(13, 0xb9cfe6), ft = T(13, 0x8fe0a8, true), wt = T(12, ok ? OK : WARN, true, 0.5f);
  float iw = w - 2 * pad, hh = 9 * s + 20 * s + 4 * s;
  hh += wrapped(nullptr, {0, 0}, iw, info.d, dt2, 1.35f, false);
  if (!fx.empty()) hh += 4 * s + wrapped(nullptr, {0, 0}, iw, fx.c_str(), ft, 1.3f, false);
  if (hasCost) hh += 6 * s + 17 * s;
  if (!warn.empty()) hh += 5 * s + wrapped(nullptr, {0, 0}, iw, warn.c_str(), wt, 1.3f, false);
  hh += 10 * s;
  float x0 = winW_ - 8 * s - w, y1 = winH_ - 156 * s - (1 - vis) * 4 * s, y0 = y1 - hh;
  if (!layer("##tip", {x0, y0}, {w, hh}, false)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float a = vis;
  glowRect(dl, {x0, y0}, {x0 + w, y1}, rgb(0x2878dc, 0.35f * a), 14 * s);
  cutPanel(dl, {x0, y0}, {x0 + w, y1}, 8 * s, rgb(0x0e1e34, 0.97f * a), rgb(0x060e1a, 0.98f * a), rgb(EDGE_HI, a));
  auto A = [&](Txt t) { t.col = alpha(t.col, a); return t; };
  float x = x0 + pad, y = y0 + 9 * s;
  {
    float cy = y + 10 * s;
    textC(dl, x, cy, info.t, A(tt));
    float kx = x + textW(info.t, tt) + 8 * s;
    Txt kt = A(T(11, 0xffd27a, false, 0, true));
    float kw = textW(info.k, kt) + 8 * s;
    dl->AddRect({kx, cy - 7 * s}, {kx + kw, cy + 7 * s}, rgb(0x6b5422, a));
    textC(dl, kx + 4 * s, cy, info.k, kt);
    if (!lv.empty()) { Txt lt = A(T(11, MUTED, false, 0, true)); textC(dl, x0 + w - pad - textW(lv.c_str(), lt), cy, lv.c_str(), lt); }
    y += 20 * s + 4 * s;
  }
  y += wrapped(dl, {x, y}, iw, info.d, A(dt2), 1.35f);
  if (!fx.empty()) { y += 4 * s; y += wrapped(dl, {x, y}, iw, fx.c_str(), A(ft), 1.3f); }
  if (hasCost) {
    y += 6 * s;
    Txt m = A(T(13, minerals_ < cm ? DANGER : MINERAL, false, 0, true)), g = A(T(13, gas_ < cg ? DANGER : GAS, false, 0, true));
    std::string ms = "\xE2\x97\x86" + std::to_string(cm), gs = "\xE2\x96\xB2" + std::to_string(cg);
    rich(dl, {x, y}, ms.c_str(), m);
    rich(dl, {x + richW(ms.c_str(), m) + 12 * s, y}, gs.c_str(), g);
    y += 17 * s;
  }
  if (!warn.empty()) { y += 5 * s; wrapped(dl, {x, y}, iw, warn.c_str(), A(wt), 1.3f); }
  ImGui::End();
}

// ─── Screen flashes (nuke white-out, damage vignette, repair glow) ───────────
void Game::flash(int which) { flashT_[which] = 0; }

void Game::drawFlashes() {
  float dt = ImGui::GetIO().DeltaTime;
  float op[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    if (flashT_[i] < 0) continue;
    flashT_[i] += dt;
    float t = flashT_[i], hold = i == 0 ? 0.08f : 0.1f, fade = i == 0 ? 0.6f : 0.3f;
    if (t < 0.05f) op[i] = t / 0.05f;
    else if (t < hold) op[i] = 1;
    else if (t < hold + fade) op[i] = 1 - (t - hold) / fade;
    else flashT_[i] = -1;
  }
  if (op[0] <= 0 && op[1] <= 0 && op[2] <= 0) return;
  if (!layer("##flashes", {0, 0}, {(float)winW_, (float)winH_}, false)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float W = (float)winW_, H = (float)winH_;
  if (op[1] > 0) {   // radial vignette: clear inside 55%, red at the corners
    ImVec2 c(W / 2, H / 2), r(W / 2 * 1.4142f, H / 2 * 1.4142f);
    dl->PushClipRect({0, 0}, {W, H});
    radial(dl, c, r, IM_COL32(255, 30, 30, 0), IM_COL32(255, 30, 30, (int)(97 * op[1])), 0.55f);
    dl->PopClipRect();
  }
  if (op[2] > 0) dl->AddRectFilled({0, 0}, {W, H}, IM_COL32(50, 255, 80, (int)(46 * op[2])));
  if (op[0] > 0) dl->AddRectFilled({0, 0}, {W, H}, IM_COL32(255, 240, 200, (int)(242 * op[0])));
  ImGui::End();
}

// ─── Panels (roster, menu, cheat console, title, results) ────────────────────
static void panelBg(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut) {
  glowRect(dl, a, b, rgb(0x2878dc, 0.3f), 22 * sc());
  cutPanel(dl, a, b, cut * sc(), rgb(0x0e1e34, 0.97f), rgb(0x050c16, 0.98f), rgb(EDGE_HI));
}

// Wide HUD button with a centred label (optionally an icon); returns clicked
struct WideBtn { ImU32 top, bot, border, text; };
static const WideBtn WB_BLUE = {rgb(0x1d3b5e), rgb(0x10223a), rgb(EDGE), rgb(0xcfe6ff)};
static const WideBtn WB_SHOP = {rgb(0x244a78), rgb(0x12274a), rgb(EDGE), rgb(0xcfe6ff)};
static bool wideButton(ImDrawList* dl, const char* id, ImVec2 pos, ImVec2 size, const std::string& label, const WideBtn& st, bool enabled,
                       float px = 13, const char* svg = nullptr, bool active = false, float cut = 5, float spacing = 1.5f) {
  const float s = sc();
  ImGui::SetCursorScreenPos(pos);
  Btn b = button(id, size, enabled);
  bool hov = b.hovered && enabled;
  float op = enabled ? 1 : 0.45f;
  ImU32 top = active ? rgb(0x2e6fc0) : hov ? rgb(0x2a5684) : st.top, bot = active ? rgb(0x18427a) : hov ? rgb(0x173253) : st.bot;
  if (st.top == WB_SHOP.top && hov) { top = rgb(0x3266a3); bot = rgb(0x1a3863); }
  if (hov) glowRect(dl, b.a, b.b, rgb(0x46a0ff, 0.45f), 10 * s);
  cutPanel(dl, b.a, b.b, cut * s, alpha(top, op), alpha(bot, op), alpha(hov || active ? rgb(EDGE_HI) : st.border, op));
  Txt t = TA(px, alpha(active ? IM_COL32_WHITE : st.text, op), true, spacing);
  float tw = richW(label.c_str(), t) + (svg ? 15 * s + 8 * s : 0);
  float x = (b.a.x + b.b.x) / 2 - tw / 2, cy = (b.a.y + b.b.y) / 2;
  if (svg) { svgIcon(dl, svg, {x + 7.5f * s, cy}, 15 * s, t.col); x += 23 * s; }
  rich(dl, {x, cy - textH(t) / 2}, label.c_str(), t);
  return b.clicked;
}

static std::string resText(int m, int g) { return "\xE2\x97\x86" + std::to_string(m) + " \xE2\x96\xB2" + std::to_string(g); }
std::string shortText(float m, float g, float have_m, float have_g) {
  int nm = std::max(0, (int)std::ceil(m - have_m)), ng = std::max(0, (int)std::ceil(g - have_g));
  std::string r;
  if (nm) r += "\xE2\x97\x86" + std::to_string(nm);
  if (nm && ng) r += " ";
  if (ng) r += "\xE2\x96\xB2" + std::to_string(ng);
  return r;
}

void Game::drawRoster() {
  if (!rosterPaused_) return;
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  // pause dim (display only: the command card stays clickable under it)
  if (layer("##dim", {0, 0}, {W, H}, false)) ImGui::GetWindowDrawList()->AddRectFilled({0, 0}, {W, H}, IM_COL32(0, 4, 10, 140));
  ImGui::End();
  float pw = std::min(760 * s, W - 24 * s), px0 = (W - pw) / 2, py0 = 44 * s, maxH = H - 56 * s - py0 + 44 * s - 44 * s;
  maxH = std::max(100.f, H - 56 * s);
  static float contentH = 400;
  float ph = std::min(contentH, maxH - py0 + 12 * s);
  ph = std::min(contentH, H - 56 * s);
  if (!layer("##roster", {px0, py0}, {pw, ph}, true)) { ImGui::End(); return; }
  ImDrawList* bgdl = ImGui::GetWindowDrawList();
  panelBg(bgdl, {px0, py0}, {px0 + pw, py0 + ph}, 12);
  ImGui::SetCursorScreenPos({px0, py0});
  ImGui::BeginChild("##rosterScroll", {pw, ph}, false, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float x0 = o.x + 16 * s, x1 = o.x + pw - 16 * s, y = o.y + 14 * s, iw = x1 - x0;
  // title row
  {
    float cy = y + 12 * s;
    Txt t = T(18, 0x9fd2ff, true, 3);
    t.glow = rgb(0x50aaff, 0.4f);
    textC(dl, x0, cy, "BUNKER ROSTER", t);
    float x = x0 + textW("BUNKER ROSTER", t) + 12 * s;
    std::string lbl = fmt("%d/6 slots \xE2\x80\x94 %d free", slotsUsed(), slotsLeft()) + (hpLvl_ >= 3 ? " \xC2\xB7 Shrike turret: +2 marines (auto)" : "");
    textC(dl, x, cy, lbl.c_str(), T(13, MUTED, true));
    Txt rt = T(13, 0xffd9d4, true, 1.5f);
    float rbw = 14 * s + 15 * s + 6 * s + textW("RESUME", rt) + 14 * s, rbx = x1 - rbw;
    ImGui::SetCursorScreenPos({rbx, cy - 12 * s});
    Btn rb = button("##resume", {rbw, 24 * s});
    if (rb.hovered) glowRect(dl, rb.a, rb.b, rgb(0xff5a46, 0.5f), 10 * s);
    cutPanel(dl, rb.a, rb.b, 6 * s, rgb(0x6a1f1f), rgb(0x3a0e0e), rb.hovered ? rgb(0xff7a66) : rgb(0xb2453a));
    svgIcon(dl, "play", {rbx + 14 * s + 7 * s, cy}, 15 * s, rgb(0xffd9d4));
    textC(dl, rbx + 14 * s + 21 * s, cy, "RESUME", rt);
    Txt pt = T(12, WARN, true, 2);
    const char* ps = "\xE2\x9D\x99\xE2\x9D\x99 PAUSED";
    textC(dl, rbx - 12 * s - textW(ps, pt), cy, ps, pt);
    if (rb.clicked) closeRoster();
    y += 24 * s + 9 * s;
    dl->AddLine({x0, y}, {x1, y}, rgb(0x1d3a5a));
    y += 12 * s;
  }
  // slots
  {
    float x = x0;
    int ghostOrd = 0;
    for (int i = 0; i < (int)roster_.size(); i++) {
      Unit u = roster_[i];
      const UnitDef& d = unitDef(u);
      float tw = (d.slots == 2 ? 85 : 40) * s, th = 40 * s;
      unsigned bc = u == Unit::Marine ? 0x4a8be0 : u == Unit::Marauder ? 0xd4761e : 0x9152e0;
      unsigned fc = u == Unit::Marine ? 0x224488 : u == Unit::Marauder ? 0x823c0a : 0x46148c;
      unsigned tc = u == Unit::Marine ? 0xa8c8ff : u == Unit::Marauder ? 0xffbd8a : 0xd6b0ff;
      ImGui::SetCursorScreenPos({x, y});
      Btn tile = button(fmt("##tile%d", i).c_str(), {tw, th});
      float br = tile.hovered ? 1.25f : 1;
      dl->AddRectFilled(tile.a, tile.b, rgb(fc, 0.35f * br));
      dl->AddRect(tile.a, tile.b, rgb(bc));
      std::string tag;
      unsigned tagCol = tc;
      if (u == Unit::Ghost && snipeUnlocked_) {
        float cd = std::max(0.f, (ghostOrd < (int)snipeReady_.size() ? snipeReady_[ghostOrd] : 0) - gameT_);
        tag = cd > 0 ? fmt("SNIPE %ds", (int)std::ceil(cd)) : "SNIPE RDY";
        tagCol = cd > 0 ? 0xd6b0ff : 0xe08aff;
      }
      if (u == Unit::Marauder && concussiveUnlocked_) { tag = "SLOW" + (concussiveLvl_ > 0 ? fmt(" A%d", concussiveLvl_) : ""); tagCol = 0xffbd8a; }
      if (u == Unit::Ghost) ghostOrd++;
      Txt lt = T(14, tc, true, 0.5f), st = TA(9, tag.empty() ? alpha(rgb(tc), 0.75f) : rgb(tagCol), true, 1);
      const char* sub = tag.empty() ? d.name : tag.c_str();
      float cxm = (tile.a.x + tile.b.x) / 2;
      text(dl, {cxm - textW(d.label, lt) / 2, tile.a.y + 6 * s}, d.label, lt);
      text(dl, {cxm - textW(sub, st) / 2, tile.a.y + 24 * s}, sub, st);
      if (tile.hovered) {   // ✕ dismisses (75% refund)
        ImVec2 ra(tile.b.x - 16 * s, tile.a.y), rb2(tile.b.x, tile.a.y + 16 * s);
        bool overX = ImGui::IsMouseHoveringRect(ra, rb2);
        if (overX) dl->AddRectFilled(ra, rb2, IM_COL32(255, 60, 60, 89));
        Txt xt = T(12, overX ? 0xffffff : 0xff8a80);
        text(dl, {ra.x + 4 * s, ra.y + 1 * s}, "\xE2\x9C\x95", xt);
        if (overX) ImGui::SetTooltip("Dismiss (75%% refund)");
        if (tile.clicked && overX) dismissUnit(i);
      }
      x += tw + 5 * s;
    }
    for (int i = 0; i < slotsLeft(); i++) {
      float tw = 40 * s, th = 40 * s;
      dl->AddRectFilled({x, y}, {x + tw, y + th}, IM_COL32(255, 255, 255, 8));
      for (float d = 0; d < tw; d += 6 * s) {   // dashed border
        dl->AddLine({x + d, y}, {x + std::min(tw, d + 3 * s), y}, IM_COL32(255, 255, 255, 51));
        dl->AddLine({x + d, y + th}, {x + std::min(tw, d + 3 * s), y + th}, IM_COL32(255, 255, 255, 51));
        dl->AddLine({x, y + d}, {x, y + std::min(th, d + 3 * s)}, IM_COL32(255, 255, 255, 51));
        dl->AddLine({x + tw, y + d}, {x + tw, y + std::min(th, d + 3 * s)}, IM_COL32(255, 255, 255, 51));
      }
      Txt pl = TA(14, IM_COL32(255, 255, 255, 77));
      text(dl, {x + tw / 2 - textW("+", pl) / 2, y + th / 2 - textH(pl) / 2}, "+", pl);
      x += tw + 5 * s;
    }
    y += 40 * s + 12 * s;
  }
  // unit shop (3 columns)
  {
    float cw = (iw - 16 * s) / 3, ch = 9 * s + 20 * s + 16 * s + 3 * s + 34 * s + 6 * s + 17 * s + 6 * s + 24 * s + 9 * s;
    for (int k = 0; k < 3; k++) {
      Unit u = (Unit)k;
      const UnitDef& d = unitDef(u);
      float x = x0 + k * (cw + 8 * s);
      unsigned bc = u == Unit::Marine ? 0x4a8be0 : u == Unit::Marauder ? 0xd4761e : 0x9152e0;
      unsigned g0 = u == Unit::Marine ? 0x2850a0 : u == Unit::Marauder ? 0x96460a : 0x5a1eaa;
      unsigned g1 = u == Unit::Marine ? 0x142850 : u == Unit::Marauder ? 0x461e05 : 0x280f50;
      unsigned nc = u == Unit::Marine ? 0xa8c8ff : u == Unit::Marauder ? 0xffbd8a : 0xd6b0ff;
      cutPanel(dl, {x, y}, {x + cw, y + ch}, 8 * s, rgb(g0, 0.28f), rgb(g1, 0.12f), rgb(bc));
      float ix = x + 11 * s, yy = y + 9 * s;
      text(dl, {ix, yy}, d.name, T(16, nc, true, 1.5f));
      yy += 20 * s;
      text(dl, {ix, yy}, fmt("%d slot%s", d.slots, d.slots > 1 ? "s" : "").c_str(), T(12, MUTED, true));
      yy += 16 * s + 3 * s;
      std::string note = (d.dmgMult != 1 ? fmt("\xC3\x97%g dmg", d.dmgMult) : std::string("balanced")) +
                         (d.bonusVsArmored ? fmt(", +%d%% vs armored", (int)std::round((d.bonusMult - 1) * 100)) : d.bonusVsLight ? fmt(", +%d%% vs light", (int)std::round((d.bonusMult - 1) * 100)) : std::string(", no bonus")) +
                         (u == Unit::Marauder ? ", blast slows the area" : "");
      wrapped(dl, {ix, yy}, cw - 22 * s, note.c_str(), T(13, 0xb9cfe6), 1.3f);
      yy += 34 * s + 6 * s;
      Txt cmt = T(13, MINERAL, false, 0, true), cgt = T(13, GAS, false, 0, true);
      std::string ms = "\xE2\x97\x86" + std::to_string(d.cost[0]), gs = " \xE2\x96\xB2" + std::to_string(d.cost[1]);
      rich(dl, {ix, yy}, ms.c_str(), cmt);
      rich(dl, {ix + richW(ms.c_str(), cmt), yy}, gs.c_str(), cgt);
      yy += 17 * s + 6 * s;
      bool afford = minerals_ >= d.cost[0] && gas_ >= d.cost[1], fit = slotsLeft() >= d.slots;
      std::string lbl = !fit ? "NO SLOTS" : !afford ? "NEED " + shortText(d.cost[0], d.cost[1], minerals_, gas_) : "HIRE";
      if (wideButton(dl, fmt("##hire%d", k).c_str(), {ix, yy}, {cw - 22 * s, 24 * s}, lbl, WB_SHOP, afford && fit)) setStatus(hireUnit(u));
    }
    y += ch + 10 * s;
  }
  // research (2 columns)
  {
    struct R { const char* key; Unit unit; const char* name; const char* icon; int cost[2]; bool unlocked; int lvl; int tiers; int tc[3][2]; const char* desc; };
    // Sensor Tower: levels 1-3 bought in turn (the first is the research), no unit needed
    const auto& sk = cfg::sensor_costs;
    R rs[4] = {
      {"superstim", Unit::Marine, "Super Stimpack", ui_icons::superstim, {250, 150}, superStimUnlocked_, superStimLvl_, 3, {{200, 80}, {320, 130}, {500, 200}},
       "Nova's Marines: Stimpack costs no HP and repairs the bunker while it lasts (stacks with the hull's self-repair). Its tiers lower the Marines Stimpack needs: 6 \xE2\x86\x92 4 \xE2\x86\x92 2 \xE2\x86\x92 1"},
      {"concussive", Unit::Marauder, "Concussive Shell", ui_icons::concussive, {300, 75}, concussiveUnlocked_, concussiveLvl_, 3, {{250, 80}, {400, 130}, {600, 200}}, "Marauder blasts slow harder: to 40% speed (from 70%) for 2.5s (from 1.5s)"},
      {"snipe", Unit::Ghost, "Snipe", ui_icons::snipe, {250, 100}, snipeUnlocked_, snipeLvl_, 3, {{200, 80}, {320, 130}, {500, 200}}, "Ghost auto-snipes highest-threat enemy (8s CD)"},
      {"sensor", Unit::Marine, "Sensor Tower", nullptr, {sk[0][0], sk[0][1]}, sensorLvl_ >= 1, sensorLvl_, 3, {{sk[0][0], sk[0][1]}, {sk[1][0], sk[1][1]}, {sk[2][0], sk[2][1]}},
       "At night, Zerg in the dark within range show as red ! blips (not which kind). Range 10.75 / 12.5 / 16; the lit area reaches about 9."},
    };
    float cw = (iw - 8 * s) / 2;
    float rowY = y, rowH = 0;
    for (int k = 0; k < 4; k++) {
      R& r = rs[k];
      bool sensor = !strcmp(r.key, "sensor");
      float x = x0 + (k % 2) * (cw + 8 * s);
      if (k > 0 && k % 2 == 0) { rowY += rowH + 8 * s; rowH = 0; }
      bool hasUnit = sensor || std::find(roster_.begin(), roster_.end(), r.unit) != roster_.end();
      bool locked = !hasUnit && !r.unlocked;
      std::string status = r.desc;
      if (r.unlocked) {
        if (!strcmp(r.key, "snipe")) status = fmt("CD: %.1fs | tier %d/3", (double)(r.lvl == 0 ? 8 : r.lvl == 1 ? 6 : r.lvl == 2 ? 4 : 2.5), r.lvl);
        if (!strcmp(r.key, "concussive")) status = fmt("Slow 40%% for 2.5s | blast radius %.1f | tier %d/3", (double)MG_RADIUS[r.lvl], r.lvl);
        if (!strcmp(r.key, "superstim")) status = fmt("Stim: no HP cost, +%d HP/s to the bunker | needs %d Marine%s | tier %d/3", (int)cfg::superstim_heal, cfg::stim_marines[r.lvl], cfg::stim_marines[r.lvl] > 1 ? "s" : "", r.lvl);
        if (sensor) status = fmt("Blips out to %g | tier %d/3", (double)cfg::sensor_range[r.lvl], r.lvl);
      }
      if (locked) status += std::string(" \xE2\x80\x94 need ") + unitDef(r.unit).key;
      std::string lbl;
      bool dis;
      int tier = -1;
      if (!r.unlocked) {
        bool afford = minerals_ >= r.cost[0] && gas_ >= r.cost[1];
        lbl = !hasUnit ? "LOCKED" : !afford ? resText(r.cost[0], r.cost[1]) + " \xE2\x80\x94 NEED " + shortText(r.cost[0], r.cost[1], minerals_, gas_) : "RESEARCH  " + resText(r.cost[0], r.cost[1]);
        dis = !hasUnit || !afford;
      } else if (r.lvl < r.tiers) {
        int m = r.tc[r.lvl][0], g = r.tc[r.lvl][1];
        bool afford = minerals_ >= m && gas_ >= g;
        lbl = !afford ? fmt("LV.%d: ", r.lvl + 1) + resText(m, g) + " \xE2\x80\x94 NEED " + shortText(m, g, minerals_, gas_) : fmt("UPGRADE LV.%d  ", r.lvl + 1) + resText(m, g);
        dis = !afford;
        tier = r.lvl;
      } else { lbl = "\xE2\x98\x85 MAXED"; dis = true; }
      unsigned bc = sensor ? 0xc0453a : r.unit == Unit::Marine ? 0x3a78d4 : r.unit == Unit::Marauder ? 0xd4761e : 0x9152e0;   // Sensor Tower: radar red
      unsigned bgc = sensor ? 0x821e14 : r.unit == Unit::Marine ? 0x143c82 : r.unit == Unit::Marauder ? 0x823c0a : 0x46148c;
      unsigned nc = sensor ? 0xffa294 : r.unit == Unit::Marine ? 0x9fc8ff : r.unit == Unit::Marauder ? 0xffbd8a : 0xd6b0ff;
      float op = locked ? 0.6f : 1;
      Txt dt3 = T(13, 0xb9cfe6);
      dt3.col = alpha(dt3.col, op);
      float textW2 = cw - 22 * s - 49 * s;
      float descH = wrapped(nullptr, {0, 0}, textW2, status.c_str(), dt3, 1.3f, false);
      float headH = std::max(40 * s, 19 * s + descH);
      float hgt = 8 * s + headH + 6 * s + 24 * s + 8 * s;
      rowH = std::max(rowH, hgt);
      float yy = rowY;
      cutPanel(dl, {x, yy}, {x + cw, yy + hgt}, 8 * s, rgb(bgc, 0.16f * op), rgb(bgc, 0.16f * op), rgb(bc, op));
      ImVec2 ia(x + 11 * s, yy + 8 * s);
      if (sensor) {   // PLACEHOLDER: the radar glyph in a dark square until btn-building-terran-sensortower.png is exported
        gradRect(dl, ia, {ia.x + 40 * s, ia.y + 40 * s}, rgb(0x0b1726), rgb(0x050b14));
        svgIcon(dl, "sensor", {ia.x + 20 * s, ia.y + 20 * s}, 28 * s, rgb(0xff7a6a));
      } else image(dl, icon(r.icon), ia, {ia.x + 40 * s, ia.y + 40 * s}, locked ? IM_COL32(150, 150, 150, 160) : IM_COL32_WHITE);
      dl->AddRect(ia, {ia.x + 40 * s, ia.y + 40 * s}, IM_COL32(140, 200, 255, (int)(89 * op)), 2 * s);
      float tx = ia.x + 49 * s;
      text(dl, {tx, yy + 8 * s}, (std::string(r.name) + (r.unlocked ? " \xE2\x9C\x93" : "")).c_str(), TA(15, alpha(rgb(nc), op), true, 1));
      wrapped(dl, {tx, yy + 8 * s + 19 * s}, textW2, status.c_str(), dt3, 1.3f);
      if (wideButton(dl, fmt("##res%d", k).c_str(), {x + 11 * s, yy + 8 * s + headH + 6 * s}, {cw - 22 * s, 24 * s}, lbl, WB_SHOP, !dis))
        researchUpgrade(r.key, tier);
    }
    y = rowY + rowH + 8 * s;
  }
  // Endless armory
  if (mode_ == Mode::Endless) {
    y += 6 * s - 8 * s;
    Txt ah = T(13, GOLD, true, 2.5f);
    text(dl, {x0, y}, "ENDLESS ARMORY", ah);
    text(dl, {x0 + textW("ENDLESS ARMORY", ah) + 8 * s, y + 2 * s}, "spend the surplus \xE2\x80\x94 each purchase costs 30% more (ammo 15%)", T(11, DIM, true, 0.5f));
    y += 21 * s;
    static const char* NAMES[4] = {"Hull plating", "Weapon tuning", "Magazine", "Fleet logistics"};
    const char* ICONS[4] = {ui_icons::neosteel, ui_icons::weapons3, ui_icons::enhanced, ui_icons::fleet};
    float cw = (iw - 24 * s) / 4, chh = 0;
    std::vector<float> hs(4);
    for (int k = 0; k < 4; k++) {
      int n = armory_[k];
      std::string fx = k == 0 ? fmt("+%d max HP", (int)cfg::armory_hp) : k == 1 ? fmt("+%d%% damage", (int)std::round(cfg::armory_dmg * 100))
                     : k == 2 ? fmt("+%d rounds", cfg::armory_ammo) : fmt("strike & nuke cooldowns \xE2\x88\x92%d%%", (int)std::round((1 - cfg::armory_cd) * 100));
      std::string tot = k == 0 ? fmt("+%d HP", n * (int)cfg::armory_hp) : k == 1 ? fmt("+%d%% damage", (int)std::round(n * cfg::armory_dmg * 100))
                      : k == 2 ? fmt("+%d rounds", n * cfg::armory_ammo) : fmt("cooldowns \xC3\x97%.2f", armoryCdMult(n));
      std::string desc = fx + (n ? "\nnow " + tot : "");
      hs[k] = wrapped(nullptr, {0, 0}, cw - 18 * s - 41 * s, desc.c_str(), T(12, 0xb9cfe6), 1.3f, false);
      chh = std::max(chh, std::max(32 * s, 18 * s + hs[k]));
    }
    float cardH = 7 * s + chh + 3 * s + 14 * s + 6 * s + 24 * s + 7 * s;
    for (int k = 0; k < 4; k++) {
      int n = armory_[k];
      float x = x0 + k * (cw + 8 * s);
      bool maxed = armoryMaxed(k);
      int m, g;
      armoryCost(k, m, g);
      bool afford = minerals_ >= m && gas_ >= g;
      cutPanel(dl, {x, y}, {x + cw, y + cardH}, 8 * s, rgb(0x785a0a, 0.14f), rgb(0x785a0a, 0.14f), rgb(0xb08a10));
      ImVec2 ia(x + 9 * s, y + 7 * s);
      image(dl, icon(ICONS[k]), ia, {ia.x + 32 * s, ia.y + 32 * s});
      dl->AddRect(ia, {ia.x + 32 * s, ia.y + 32 * s}, IM_COL32(140, 200, 255, 89), 2 * s);
      float tx = ia.x + 41 * s;
      text(dl, {tx, y + 7 * s}, (std::string(NAMES[k]) + (n ? fmt(" \xC3\x97%d", n) : "")).c_str(), T(14, GOLD, true, 1));
      std::string fx = k == 0 ? fmt("+%d max HP", (int)cfg::armory_hp) : k == 1 ? fmt("+%d%% damage", (int)std::round(cfg::armory_dmg * 100))
                     : k == 2 ? fmt("+%d rounds", cfg::armory_ammo) : fmt("strike & nuke cooldowns \xE2\x88\x92%d%%", (int)std::round((1 - cfg::armory_cd) * 100));
      std::string tot = k == 0 ? fmt("+%d HP", n * (int)cfg::armory_hp) : k == 1 ? fmt("+%d%% damage", (int)std::round(n * cfg::armory_dmg * 100))
                      : k == 2 ? fmt("+%d rounds", n * cfg::armory_ammo) : fmt("cooldowns \xC3\x97%.2f", armoryCdMult(n));
      wrapped(dl, {tx, y + 7 * s + 18 * s}, cw - 18 * s - 41 * s, (fx + (n ? "\nnow " + tot : "")).c_str(), T(12, 0xb9cfe6), 1.3f);
      float by = y + 7 * s + chh + 3 * s;
      if (!maxed && !afford) rich(dl, {x + 9 * s, by}, ("need " + shortText(m, g, minerals_, gas_) + " more").c_str(), T(11, DANGER));
      by += 14 * s + 6 * s;
      std::string lbl = maxed ? "\xE2\x98\x85 MAXED" : (afford ? "BUY " : "") + resText(m, g);
      if (wideButton(dl, fmt("##arm%d", k).c_str(), {x + 9 * s, by}, {cw - 18 * s, 24 * s}, lbl, WB_SHOP, !maxed && afford)) buyArmory(k);
    }
    y += cardH + 8 * s;
  }
  y += 16 * s - 8 * s;
  contentH = y - o.y;
  ImGui::SetCursorScreenPos({o.x, o.y + contentH - 1});
  ImGui::Dummy({1, 1});
  ImGui::EndChild();
  ImGui::End();
}

// custom checkbox (accent #4a9bff)
static bool checkbox(ImDrawList* dl, const char* id, ImVec2 pos, bool& v, const char* label) {
  const float s = sc();
  Txt t = T(14, 0xbcd7f5, true, 0.5f);
  float w = 15 * s + 8 * s + richW(label, t);
  ImGui::SetCursorScreenPos(pos);
  Btn b = button(id, {w, 18 * s});
  ImVec2 a(pos.x, pos.y + 1.5f * s), e(a.x + 15 * s, a.y + 15 * s);
  dl->AddRectFilled(a, e, v ? rgb(0x4a9bff) : IM_COL32(255, 255, 255, 230), 3 * s);
  if (!v) dl->AddRect(a, e, IM_COL32(118, 118, 118, 255), 3 * s);
  if (v) dl->AddPolyline(std::vector<ImVec2>{{a.x + 3.5f * s, a.y + 7.5f * s}, {a.x + 6.5f * s, a.y + 10.5f * s}, {a.x + 11.5f * s, a.y + 4.5f * s}}.data(), 3, IM_COL32_WHITE, 0, 2 * s);
  rich(dl, {pos.x + 23 * s, pos.y + 9 * s - textH(t) / 2}, label, t);
  if (b.clicked) { v = !v; return true; }
  return false;
}

// ─── Inline markup and key chips ─────────────────────────────────────────────
// Text with the web version's inline markup — <b>…</b> (white), <kbd>…</kbd> (a
// key chip), &amp;, ◆ / ▲ (the mineral / gas icons) — wrapped at spaces into lines
// of at most w; returns the height.
struct MarkRun { std::string s; int style; };   // 0 plain · 1 bold · 2 key
static std::vector<std::vector<MarkRun>> markupWords(const std::string& src) {
  std::vector<std::vector<MarkRun>> words(1);   // runs between spaces (a key keeps its spaces)
  int style = 0;
  auto put = [&](char c) {
    auto& w = words.back();
    if (w.empty() || w.back().style != style) w.push_back({"", style});
    w.back().s += c;
  };
  for (size_t i = 0; i < src.size();) {
    if (!src.compare(i, 3, "<b>")) { style = 1; i += 3; }
    else if (!src.compare(i, 4, "</b>")) { style = 0; i += 4; }
    else if (!src.compare(i, 5, "<kbd>")) { style = 2; i += 5; }
    else if (!src.compare(i, 6, "</kbd>")) { style = 0; i += 6; }
    else if (!src.compare(i, 5, "&amp;")) { put('&'); i += 5; }
    else if (src[i] == ' ' && style != 2) { if (!words.back().empty()) words.emplace_back(); i++; }
    else put(src[i++]);
  }
  if (words.back().empty()) words.pop_back();
  return words;
}
// a key chip (kbd): gold monospace in a dark box, vertically centred on cy; returns its width
static float kbdChip(ImDrawList* dl, float x, float cy, const char* k, float px, float padX, float a = 1) {
  const float s = sc();
  Txt t = TA(px, alpha(rgb(0xffd27a), a), false, 0, true);
  float w = textW(k, t) + 2 * padX * s, h = textH(t) + 2 * s;
  if (dl) {
    dl->AddRectFilled({x, cy - h / 2}, {x + w, cy + h / 2}, IM_COL32(40, 30, 8, (int)(128 * a)));
    dl->AddRect({x, cy - h / 2}, {x + w, cy + h / 2}, alpha(rgb(0x4a3c1c), a));
    textC(dl, x + padX * s, cy, k, t);
  }
  return w;
}
static float markup(ImDrawList* dl, ImVec2 p, float w, const std::string& src, const Txt& base, float lineH, float kbdPx = 11) {
  const float s = sc(), lh = base.px * s * lineH, a = ((base.col >> IM_COL32_A_SHIFT) & 255) / 255.f;
  Txt bold = base;
  bold.bold = true;
  bold.col = alpha(IM_COL32_WHITE, a);
  auto runW = [&](const MarkRun& r) { return r.style == 2 ? kbdChip(nullptr, 0, 0, r.s.c_str(), kbdPx, 4) : richW(r.s.c_str(), r.style ? bold : base); };
  const float space = textW(" ", base);
  float x = 0, y = 0;
  bool lineEmpty = true;
  for (const auto& word : markupWords(src)) {
    float ww = 0;
    for (const auto& r : word) ww += runW(r);
    if (!lineEmpty && x + space + ww > w) { x = 0; y += lh; lineEmpty = true; }
    if (!lineEmpty) x += space;
    for (const auto& r : word) {
      float rw = runW(r);
      if (dl) {
        if (r.style == 2) kbdChip(dl, p.x + x, p.y + y + textH(base) / 2, r.s.c_str(), kbdPx, 4, a);
        else rich(dl, {p.x + x, p.y + y}, r.s.c_str(), r.style ? bold : base);   // ◆ / ▲: the mineral / gas icons
      }
      x += rw;
    }
    lineEmpty = false;
  }
  return lineEmpty ? 0 : y + lh;
}

// a cut-corner panel with a left → right gradient (the active menu tab)
static void hgradCut(ImDrawList* dl, ImVec2 a, ImVec2 b, float c, ImU32 left, ImU32 right) {
  ImVec2 p[6] = {{a.x + c, a.y}, {b.x, a.y}, {b.x, b.y - c}, {b.x - c, b.y}, {a.x, b.y}, {a.x, a.y + c}};
  ImVec2 uv = dl->_Data->TexUvWhitePixel;
  dl->PrimReserve(12, 6);
  ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
  for (const ImVec2& q : p) dl->PrimWriteVtx(q, uv, lerpCol(left, right, (q.x - a.x) / std::max(1.f, b.x - a.x)));
  for (int i = 1; i < 5; i++) { dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + i)); dl->PrimWriteIdx((ImDrawIdx)(base + i + 1)); }
}

// ─── Menu: two columns, as the web version lays it out ───────────────────────
// Left: the tabs, then RESUME / RESTART / MAIN MENU. Right: the chosen tab's page,
// the same height on every tab. Every setting applies at once (the graphics card
// at the next start). ↑ ↓ switch tabs; the last tab chosen opens again.
static const char* MENU_TAB_LABEL[] = {"GAME", "SETTINGS", "DISPLAY", "CONTROLS", "TUTORIAL", "CHEATS"};
static const char* MENU_TAB_ICON[] = {"save", "gfx", "display", "keys", "help", "cheat"};

void Game::drawMenu() {
  if (!menuOpen_) return;
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  if (!layer("##menuDim", {0, 0}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled({0, 0}, {W, H}, IM_COL32(0, 4, 10, 184));
  const float pw = std::min(760 * s, W - 24 * s), padT = 16 * s, padX = 22 * s, padB = 14 * s;
  Txt title = T(22, 0x9fd2ff, true, 5);
  title.glow = rgb(0x50aaff, 0.5f);
  Txt foot = T(11, DIM, true, 1);
  const float tabH = 34 * s, actH = 30 * s, gap = 5 * s;
  // the left column sets the page height: six tabs, a gap of at least 14 px, three commands
  const float bodyH = std::max(330 * s, MENU_TAB_COUNT * tabH + 3 * actH + (MENU_TAB_COUNT + 3) * gap + 14 * s);
  const float ph = padT + textH(title) + 8 * s + 1 + 12 * s + bodyH + 9 * s + textH(foot) + padB;
  const float px0 = std::round((W - pw) / 2), py0 = std::round(std::max(8 * s, (H - ph) / 2)), px1 = px0 + pw, py1 = py0 + ph;
  panelBg(dl, {px0, py0}, {px1, py1}, 14);
  text(dl, {(px0 + px1) / 2 - textW("MENU", title) / 2, py0 + padT}, "MENU", title);
  const float lineY = py0 + padT + textH(title) + 8 * s;
  dl->AddLine({px0 + padX, lineY}, {px1 - padX, lineY}, rgb(0x1d3a5a));
  const float colY0 = lineY + 1 + 12 * s, colY1 = colY0 + bodyH;

  // ── left: tabs, then the game commands ──
  const float lx0 = px0 + padX, lw = 168 * s;
  dl->AddLine({lx0 + 184 * s - 1, colY0}, {lx0 + 184 * s - 1, colY1}, rgb(0x132a44));
  for (int i = 0; i < MENU_TAB_COUNT; i++) {
    ImGui::SetCursorScreenPos({lx0, colY0 + i * (tabH + gap)});
    Btn b = button(fmt("##mtab%d", i).c_str(), {lw, tabH});
    bool on = menuTab_ == i;
    if (on) {
      hgradCut(dl, b.a, b.b, 6 * s, rgb(0x2e6fc0, 0.6f), rgb(0x18427a, 0.2f));
      cutPanel(dl, b.a, b.b, 6 * s, 0, 0, rgb(EDGE_HI));
    } else if (b.hovered) cutPanel(dl, b.a, b.b, 6 * s, rgb(0x2e6fc0, 0.18f), rgb(0x2e6fc0, 0.18f), 0);
    unsigned col = on ? 0xffffff : b.hovered ? 0xe2f1ff : 0x9fb8d3;
    float cy = (b.a.y + b.b.y) / 2;
    svgIcon(dl, MENU_TAB_ICON[i], {b.a.x + 12 * s + 8.5f * s, cy}, 17 * s, rgb(col));
    textC(dl, b.a.x + 12 * s + 17 * s + 10 * s, cy, MENU_TAB_LABEL[i], T(15, col, true, 2));
    if (b.clicked) menuTab_ = i;
  }
  bool playing = phase_ == Phase::Playing;
  WideBtn warnB = WB_BLUE, dangerB = WB_BLUE;
  warnB.text = rgb(0xffd27a);
  warnB.border = rgb(0x7a5a1e);
  dangerB.text = rgb(0xffb3aa);
  dangerB.border = rgb(0x8a3a32);
  {
    float y = colY1 - 3 * actH - 2 * gap;
    if (wideButton(dl, "##resume", {lx0, y}, {lw, actH}, playing ? "RESUME" : "BACK", WB_BLUE, true, 14, "play", false, 6, 2)) closeMenu();
    y += actH + gap;
    if (wideButton(dl, "##restart", {lx0, y}, {lw, actH}, "RESTART", dangerB, playing, 14, "restart", false, 6, 2)) { closeMenu(); restart(); }
    y += actH + gap;
    if (wideButton(dl, "##mainmenu", {lx0, y}, {lw, actH}, "MAIN MENU", dangerB, phase_ != Phase::Menu, 14, "home", false, 6, 2)) { closeMenu(); showTitle(); }
  }

  // ── right: the tab's page, in sections ──
  const float bx0 = lx0 + 184 * s + 18 * s, bx1 = px1 - padX, bw = bx1 - bx0;
  float y = colY0;
  bool firstSection = true;
  auto section = [&](const char* label) {
    if (!firstSection) {
      y += 9 * s;
      dl->AddLine({bx0, y}, {bx1, y}, rgb(0x132a44));
      y += 9 * s;
    }
    firstSection = false;
    if (label) {
      Txt lt = T(12, MUTED, true, 2.5f);
      text(dl, {bx0, y}, label, lt);
      y += textH(lt) + 7 * s;
    }
  };
  Txt noteT = T(12, DIM, true);
  auto note = [&](const std::string& t, unsigned col = DIM) {
    Txt nt = noteT;
    nt.col = rgb(col);
    y += 6 * s + wrapped(dl, {bx0, y + 6 * s}, bw, t.c_str(), nt, 1.35f);
  };
  auto check = [&](const char* id, float x, bool& v, const char* label) { return checkbox(dl, id, {x, y}, v, label); };
  auto checkW = [&](const char* label) { return 15 * s + 8 * s + richW(label, T(14, 0xbcd7f5, true, 0.5f)); };
  const float rowH = 30 * s;

  switch (menuTab_) {
    case 0: {   // GAME
      section("SAVE / LOAD");
      storage::Record d;
      bool haveSave = readSave("save_v1", d) && validSave(d);
      float hw = (bw - 10 * s) / 2;
      if (wideButton(dl, "##save", {bx0, y}, {hw, rowH}, "SAVE", WB_BLUE, playing, 14, "save", false, 6, 2)) saveGame();
      if (wideButton(dl, "##load", {bx0 + hw + 10 * s, y}, {hw, rowH}, "LOAD", WB_BLUE, haveSave, 14, "load", false, 6, 2)) loadGame(false);
      y += rowH + 8 * s;
      std::string info;
      unsigned col = MUTED;
      if (saveMsgSet_) { info = (saveMsgOk_ ? "\xE2\x9C\x93 " : "\xE2\x9C\x97 ") + saveMsg_; col = saveMsgOk_ ? OK : DANGER; }
      else info = haveSave ? "Saved game: " + saveLabel(d) : "No saved game yet";
      y += std::max(18 * s, wrapped(dl, {bx0, y}, bw, info.c_str(), T(13, col, true), 1.3f));
      note("One save slot. The game also autosaves after every cleared wave: CONTINUE on the title screen.");
      break;
    }
    case 1: {   // SETTINGS
      section("GRAPHICS QUALITY");
      if (wideButton(dl, "##gfxhi", {bx0, y}, {90 * s, rowH}, "HIGH", WB_BLUE, true, 14, nullptr, gfxHigh_, 6, 2)) setGfx(true);
      if (wideButton(dl, "##gfxlo", {bx0 + 100 * s, y}, {90 * s, rowH}, "LOW", WB_BLUE, true, 14, nullptr, !gfxHigh_, 6, 2)) setGfx(false);
      if (gpuFx_.ok()) {
        bool gp = gpuFxOn_;
        float sy = y;
        y += (rowH - 18 * s) / 2;
        if (check("##gpufx", bx0 + 214 * s, gp, "GPU PARTICLES")) setGpuFx(gp);
        y = sy;
      }
      y += rowH;
      note(std::string(gfxHigh_ ? "Sun shadows, 4\xC3\x97 MSAA, lit muzzle flashes." : "No shadows, no MSAA \xE2\x80\x94 fastest.") + " [Q] switches during play.");
      note(gpuFx_.ok() ? "GPU particles: compute shaders for denser sparks, fireballs, shockwave dust and smoke columns."
                       : "GPU particles need OpenGL 4.3 compute shaders, which this graphics card / driver does not offer.");
      section("SOUND");
      {
        bool m = audio::muted(), mu = audio::musicOn();
        float x = bx0, sy = y;
        if (check("##mute", x, m, "MUTE")) { audio::setMuted(m); storage::set("muted", m ? "1" : "0"); }
        x += checkW("MUTE") + 10 * s;
        if (check("##music", x, mu, "MUSIC")) { audio::setMusicOn(mu); storage::set("music", mu ? "1" : "0"); }
        x += checkW("MUSIC") + 14 * s;
        float sx0 = x, sx1 = bx1 - 46 * s, cyy = sy + 9 * s;
        ImGui::SetCursorScreenPos({sx0, cyy - 12 * s});
        ImGui::InvisibleButton("##vol", {std::max(1.f, sx1 - sx0), 24 * s});
        float v = audio::volume();
        if (ImGui::IsItemActive()) {
          float nv = std::round(glm::clamp((ImGui::GetIO().MousePos.x - sx0) / (sx1 - sx0), 0.f, 1.f) * 100) / 100;
          if (nv != v) {
            audio::setVolume(nv);
            storage::set("volume", fmt("%.2f", nv));
            storage::set("muted", audio::muted() ? "1" : "0");
            audio::play("buy");
          }
          v = nv;
        }
        dl->AddRectFilled({sx0, cyy - 3 * s}, {sx1, cyy + 3 * s}, IM_COL32(255, 255, 255, 60), 3 * s);
        dl->AddRectFilled({sx0, cyy - 3 * s}, {sx0 + (sx1 - sx0) * v, cyy + 3 * s}, rgb(0x4a9bff), 3 * s);
        dl->AddCircleFilled({sx0 + (sx1 - sx0) * v, cyy}, 8 * s, rgb(0x4a9bff));
        std::string vv = audio::muted() ? "OFF" : fmt("%d%%", (int)std::round(v * 100));
        Txt vt = T(13, MUTED, false, 0, true);
        textC(dl, bx1 - textW(vv.c_str(), vt), cyy, vv.c_str(), vt);
        y = sy + 18 * s;
      }
      note("[K] mutes during play.");
      section("EXPERIMENTAL");
      {
        bool dn = dnEnabled_;
        if (check("##dn", bx0, dn, "\xE2\x98\x80 / \xE2\x98\xBD DAY/NIGHT CYCLE")) toggleDayNight();
        y += 18 * s;
      }
      note("At night only the area around the bunker is visible \xE2\x80\x94 enemies appear when they come close. [Y] toggles.");
      break;
    }
    case 2: {   // DISPLAY (desktop only)
      section("SCREEN");
      {
        bool fs = glfwGetWindowMonitor(win_) != nullptr;
        float bwid = 120 * s;
        if (wideButton(dl, "##windowed", {bx0, y}, {bwid, rowH}, "WINDOWED", WB_BLUE, true, 14, nullptr, !fs, 6, 2) && fs)
          setDisplay(false, display::windowSizes(win_).back());
        if (wideButton(dl, "##fullscreen", {bx0 + bwid + 8 * s, y}, {bwid, rowH}, "FULLSCREEN", WB_BLUE, true, 14, nullptr, fs, 6, 2) && !fs) {
          auto modes = display::fullscreenSizes(win_);
          setDisplay(true, modes.empty() ? display::Size{winW_, winH_} : modes.front());
        }
        // resolution: ◀ W × H ▶ (window presets that fit the monitor, or its video modes)
        std::vector<display::Size> sizes = fs ? display::fullscreenSizes(win_) : display::windowSizes(win_);
        int cur = -1;
        for (int i = 0; i < (int)sizes.size(); i++) if (sizes[i].w == winW_ && sizes[i].h == winH_) cur = i;
        float rx0 = bx0 + 2 * bwid + 24 * s, aw = rowH, cyy = y + rowH / 2;
        auto arrow = [&](const char* id, float x, int step, bool left) {   // step: index change in `sizes`
          ImGui::SetCursorScreenPos({x, y});
          int next = cur < 0 ? (step > 0 ? 0 : (int)sizes.size() - 1) : cur + step;   // a dragged window size: snap to the list
          bool ok = next >= 0 && next < (int)sizes.size();
          Btn b = button(id, {aw, rowH}, ok);
          cutPanel(dl, b.a, b.b, 6 * s, b.hovered && ok ? rgb(0x2a5684) : rgb(0x1d3b5e), b.hovered && ok ? rgb(0x173253) : rgb(0x10223a), rgb(b.hovered && ok ? EDGE_HI : EDGE));
          ImVec2 c((b.a.x + b.b.x) / 2, cyy);
          float k = 5 * s;
          ImU32 col = ok ? rgb(0xcfe6ff) : rgb(DIM);
          if (left) dl->AddTriangleFilled({c.x - k, c.y}, {c.x + k * .7f, c.y - k}, {c.x + k * .7f, c.y + k}, col);
          else dl->AddTriangleFilled({c.x + k, c.y}, {c.x - k * .7f, c.y - k}, {c.x - k * .7f, c.y + k}, col);
          if (b.clicked && ok) setDisplay(fs, sizes[next]);
        };
        int smaller = fs ? 1 : -1;   // window sizes are listed smallest first, fullscreen modes largest first
        arrow("##resPrev", rx0, smaller, true);
        arrow("##resNext", bx1 - aw, -smaller, false);
        std::string rs = fmt("%d \xC3\x97 %d", winW_, winH_);
        Txt rt = T(16, TEXT, false, 0, true);
        textC(dl, (rx0 + aw + bx1 - aw) / 2 - textW(rs.c_str(), rt) / 2, cyy, rs.c_str(), rt);
        y += rowH;
        note((cur < 0 ? std::string("A custom window size \xE2\x80\x94 the arrows snap to the list. ") : std::string()) +
             fmt("Applied at once. The HUD keeps its 1600\xC3\x97" "900 layout, scaled to the height (\xC3\x97%.2f).", uiScale_));
      }
      section("GRAPHICS CARD");
      {
        const display::Gpu *ded = nullptr, *integ = nullptr;
        for (const auto& g : gpus_) {
          if (g.dedicated && !ded) ded = &g;
          if (!g.dedicated && !integ) integ = &g;
        }
        if (ded && integ) {   // side by side, the card's name under its kind
          float cw = (bw - 10 * s) / 2, chh = 46 * s;
          auto card = [&](const char* id, float x, const char* label, const display::Gpu* g, bool chosen, bool dedicated) {
            ImGui::SetCursorScreenPos({x, y});
            Btn b = button(id, {cw, chh});
            bool hov = b.hovered;
            if (hov && !chosen) glowRect(dl, b.a, b.b, rgb(0x46a0ff, 0.45f), 10 * s);
            cutPanel(dl, b.a, b.b, 6 * s, chosen ? rgb(0x2e6fc0) : hov ? rgb(0x2a5684) : rgb(0x1d3b5e), chosen ? rgb(0x18427a) : hov ? rgb(0x173253) : rgb(0x10223a),
                     chosen || hov ? rgb(EDGE_HI) : rgb(EDGE));
            text(dl, {b.a.x + 12 * s, b.a.y + 6 * s}, label, T(14, chosen ? 0xffffff : 0xcfe6ff, true, 2));
            Txt nt = T(12, chosen ? 0xd8e8fb : MUTED, true);
            text(dl, {b.a.x + 12 * s, b.a.y + 25 * s}, ellipsize(g->name, cw - 24 * s, nt).c_str(), nt);
            if (b.clicked && !chosen) {
              gpuChoiceDedicated_ = dedicated;
              storage::set("gpu", dedicated ? "dedicated" : "integrated");
            }
          };
          card("##gpuDed", bx0, "DEDICATED", ded, gpuChoiceDedicated_, true);
          card("##gpuInt", bx0 + cw + 10 * s, "INTEGRATED", integ, !gpuChoiceDedicated_, false);
          y += chh;
        }
        if (!(ded && integ)) note("Running on: " + gpuRunning_ + ". Only one graphics card found \xE2\x80\x94 nothing to switch.");
        else if (gpuChoiceDedicated_ != gpuRunningDedicated_) {
          note("Running on: " + gpuRunning_ + ". The choice takes effect after a restart; the current game is not saved.", WARN);
          y += 8 * s;
          if (wideButton(dl, "##gpuRestart", {bx0, y}, {(bw - 10 * s) / 2, rowH}, "RESTART NOW", warnB, true, 14, "restart", false, 6, 2)) relaunch_ = true;
          y += rowH;
        } else note("Running on: " + gpuRunning_ + ". The dedicated card is the faster one; the integrated one saves battery.");
      }
      section("PERFORMANCE");
      check("##perf", bx0, perfOverlay_, "SHOW FRAME TIMES");
      y += 18 * s;
      note("FPS and CPU time per frame in the top-left corner. [F3] toggles.");
      break;
    }
    case 3: {   // CONTROLS: key → action
      static const char* keys[][2] = {
        {"CLICK", "Fire a volley (every unit in the bunker shoots)"}, {"HOLD", "Full auto (empties the magazine fast)"},
        {"R", "Airstrike + resupply \xE2\x80\x94 when out of ammo"}, {"N", "Nuke (wave 5+)"}, {"T", "Stimpack (six Marines in the bunker; fewer with Super Stimpack's tiers)"},
        {"B", "Roster: hire units, research (pauses)"}, {"1 2 3 4", "Upgrade damage \xC2\xB7 armor \xC2\xB7 bunker \xC2\xB7 ammo"},
        {"WHEEL  + \xE2\x88\x92", "Zoom in on the bunker / out"}, {"INSERT  DELETE", "Rotate the view 90\xC2\xB0 left / right"},
        {"HOME", "Reset the view"}, {"Q \xC2\xB7 Y \xC2\xB7 K", "Graphics \xC2\xB7 day/night \xC2\xB7 sound"}, {"F3", "Frame times"},
        {"M  ESC", "This menu (pauses)"}};
      float kw = 0;
      for (auto& k : keys) kw = std::max(kw, kbdChip(nullptr, 0, 0, k[0], 11, 5));
      Txt dt = T(13, 0xb9cfe6, true);
      for (auto& k : keys) {
        float cy = y + 10 * s;
        kbdChip(dl, bx0, cy, k[0], 11, 5);
        textC(dl, bx0 + kw + 12 * s, cy, k[1], dt);
        y += 23 * s;
      }
      break;
    }
    case 4: {   // TUTORIAL
      section("HINTS");
      {
        bool on = tutOn_;
        if (check("##tutcb", bx0, on, "TUTORIAL HINTS IN GAME")) setTutorial(on);
        int total = 0, seen = 0;
        tutCounts(seen, total);
        std::string n = fmt("%d / %d seen", seen, total);
        Txt nt = T(12, DIM, false, 0, true);
        textC(dl, bx1 - textW(n.c_str(), nt), y + 9 * s, n.c_str(), nt);
        y += 18 * s;
      }
      note("The first time a mechanic matters, a hint pauses the game and spotlights the control it is about.");
      section(nullptr);
      {
        float hw = (bw - 10 * s) / 2;
        if (wideButton(dl, "##tour", {bx0, y}, {hw, rowH}, "HUD TOUR", WB_BLUE, true, 14, "help", false, 6, 2)) { closeMenu(); startTour(); }
        if (wideButton(dl, "##replay", {bx0 + hw + 10 * s, y}, {hw, rowH}, "START OVER", WB_BLUE, true, 14, "restart", false, 6, 2)) replayTutorial();
        y += rowH;
      }
      note("HUD TOUR walks through the console and every command-card button now. START OVER shows the tour and every hint again as you play.");
      break;
    }
    default: {   // CHEATS
      section("CHEAT CODES");
      markup(dl, {bx0, y}, bw, "Type <kbd>dotd</kbd> during play to open the console; there <b>help</b> lists the codes.", T(13, DIM, true), 1.35f);
      y += 26 * s;
      if (wideButton(dl, "##cheat", {bx0, y}, {(bw - 10 * s) / 2, rowH}, "CHEAT CONSOLE", warnB, playing, 14, "cheat", false, 6, 2)) { closeMenu(); openCheat(); }
      y += rowH;
      break;
    }
  }
  const char* fs = "[ M ] or [ ESC ] to close \xC2\xB7 [ \xE2\x86\x91 \xE2\x86\x93 ] tabs";
  text(dl, {(px0 + px1) / 2 - textW(fs, foot) / 2, py1 - padB - textH(foot)}, fs, foot);
  ImGui::End();
}

void Game::drawCheat() {
  if (!cheatOpen_) return;
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  if (cheatCloseT_ >= 0) {
    cheatCloseT_ -= ImGui::GetIO().DeltaTime;
    if (cheatCloseT_ < 0) { closeCheat(); return; }
  }
  if (!layer("##cheatDim", {0, 0}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool helpShown = cheatMsg_.find('\n') != std::string::npos;
  float pw = (helpShown ? 460 : 340) * s;
  Txt mt = TA(12, rgb(cheatMsgCol_), false, 0, true);
  float msgH = std::max(16 * s, wrapped(nullptr, {0, 0}, pw - 36 * s, cheatMsg_.c_str(), mt, 1.35f, false));
  float ph = 16 * s + 22 * s + 30 * s + 8 * s + msgH + 8 * s + 14 * s + 16 * s;
  float px0 = (W - pw) / 2, py0 = (H - ph) / 2;
  ImGui::SetCursorScreenPos({0, 0});
  Btn back = button("##cheatBack", {W, H});
  glowRect(dl, {px0, py0}, {px0 + pw, py0 + ph}, rgb(0x28c85a, 0.25f), 18 * s);
  dl->AddRectFilled({px0, py0}, {px0 + pw, py0 + ph}, IM_COL32(4, 10, 6, 247));
  dl->AddRect({px0, py0}, {px0 + pw, py0 + ph}, rgb(0x2f6a3a));
  float x0 = px0 + 18 * s, y = py0 + 16 * s;
  text(dl, {x0, y}, "\xE2\x8D\xBC CHEAT CODE", T(13, WARN, false, 2, true));
  y += 22 * s;
  float okW = 46 * s, inW = pw - 36 * s - okW - 8 * s;
  ImGui::SetCursorScreenPos({x0, y});
  ImGui::PushFont(font(14, true));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, rgb(0x06140a));
  ImGui::PushStyleColor(ImGuiCol_Text, rgb(0x5cff96));
  ImGui::PushStyleColor(ImGuiCol_Border, rgb(0x275a33));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10 * s, 6 * s});
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
  ImGui::SetNextItemWidth(inW);
  if (cheatFocus_) { ImGui::SetKeyboardFocusHere(); cheatFocus_ = false; }
  bool enter = ImGui::InputTextWithHint("##cheatIn", "enter code...", cheatInput_, sizeof cheatInput_, ImGuiInputTextFlags_EnterReturnsTrue);
  float inH = ImGui::GetItemRectSize().y;
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(3);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos({x0 + inW + 8 * s, y});
  Btn ok = button("##cheatOk", {okW, inH});
  dl->AddRectFilled(ok.a, ok.b, rgb(0x123a1c));
  dl->AddRect(ok.a, ok.b, rgb(0x2f6a3a));
  Txt okt = T(13, 0x5cff96, false, 0, true);
  textC(dl, (ok.a.x + ok.b.x) / 2 - textW("OK", okt) / 2, (ok.a.y + ok.b.y) / 2, "OK", okt);
  if (enter || ok.clicked) { submitCheat(cheatInput_); if (cheatOpen_) cheatFocus_ = true; }
  y += inH + 8 * s;
  wrapped(dl, {x0, y}, pw - 36 * s, cheatMsg_.c_str(), mt, 1.35f);
  y += msgH + 8 * s;
  text(dl, {x0, y}, "press ESC to close \xC2\xB7 'help' lists the codes", T(11, 0x3a5a44, false, 0, true));
  if (back.clicked && !ImGui::IsMouseHoveringRect({px0, py0}, {px0 + pw, py0 + ph})) closeCheat();
  ImGui::End();
}

// ─── Title / results ─────────────────────────────────────────────────────────
static void overlayBg(ImDrawList* dl, float W, float H) {
  dl->AddRectFilled({0, 0}, {W, H}, IM_COL32(0, 3, 8, 242));
  radial(dl, {W / 2, H * 0.4f}, {W * 0.8f, H * 0.7f}, IM_COL32(10, 30, 60, 209), IM_COL32(0, 3, 8, 0), 0);
}

// The title card's key help, two columns (also on HOW TO PLAY); returns its height
static float keyGrid(ImDrawList* dl, float x0, float y0, float w) {
  const float s = sc();
  static const char* keys[][2] = {{"CLICK / HOLD", "Fire a volley / full auto"}, {"R", "Airstrike + resupply"}, {"N", "Nuke (wave 5+)"}, {"B", "Roster (pauses)"},
                                  {"1-4", "Upgrades"}, {"M / ESC", "Menu"}, {"WHEEL", "Zoom on the bunker"}, {"INS / DEL", "Rotate view 90\xC2\xB0"}};
  Txt dt = T(13, 0x9fb8d3);
  float kw[2] = {0, 0};   // each column's widest key
  for (int i = 0; i < 8; i++) kw[i % 2] = std::max(kw[i % 2], kbdChip(nullptr, 0, 0, keys[i][0], 11, 5));
  for (int i = 0; i < 8; i++) {
    float kx = x0 + (i % 2) * w / 2, cy = y0 + (i / 2) * 19 * s + 8 * s;
    kbdChip(dl, kx, cy, keys[i][0], 11, 5);
    textC(dl, kx + kw[i % 2] + 10 * s, cy, keys[i][1], dt);
  }
  return 4 * 19 * s;
}

// a mode button on the title card: icon, title, description
static bool modeButton(ImDrawList* dl, const char* id, ImVec2 pos, float w, const char* svg, const char* title, const std::string& desc, bool gold, bool enabled) {
  const float s = sc(), h = 10 * s + 23 * s + 17 * s + 10 * s;
  ImGui::SetCursorScreenPos(pos);
  Btn b = button(id, {w, h}, enabled);
  bool hov = b.hovered && enabled;
  float k = enabled ? 1 : 0.6f;
  auto D = [&](ImU32 c) {
    if (enabled) return c;
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    float l = 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
    v.x = (l + (v.x - l) * 0.2f) * k; v.y = (l + (v.y - l) * 0.2f) * k; v.z = (l + (v.z - l) * 0.2f) * k;
    return ImGui::ColorConvertFloat4ToU32(v);
  };
  if (hov) glowRect(dl, b.a, b.b, rgb(0x46a0ff, 0.45f), 16 * s);
  cutPanel(dl, b.a, b.b, 10 * s, D(hov ? rgb(0x2a5a92) : rgb(0x1d3d63)), D(hov ? rgb(0x173562) : rgb(0x0f2240)), D(gold ? rgb(0x8a6a10) : hov ? rgb(EDGE_HI) : rgb(EDGE)));
  gradRect(dl, {b.a.x + 10 * s, b.a.y + 1}, {b.b.x - 1, b.a.y + h * 0.45f}, D(IM_COL32(150, 205, 255, hov ? 51 : 31)), IM_COL32(150, 205, 255, 0));
  ImU32 ic = D(gold ? rgb(GOLD) : rgb(0x9fd2ff));
  svgIcon(dl, svg, {b.a.x + 16 * s + 13 * s, (b.a.y + b.b.y) / 2}, 26 * s, ic);
  float tx = b.a.x + 16 * s + 26 * s + 14 * s;
  text(dl, {tx, b.a.y + 10 * s}, title, TA(19, D(gold ? rgb(GOLD) : rgb(TEXT)), true, 3));
  text(dl, {tx, b.a.y + 10 * s + 23 * s}, desc.c_str(), TA(13, D(rgb(MUTED)), false, 0.3f));
  return b.clicked;
}

void Game::drawTitle() {
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  if (!layer("##title", {0, 0}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  overlayBg(dl, W, H);
  float cw = std::min(560 * s, W - 24 * s), pad = 30 * s;
  static float ch = 600;
  float x0 = (W - cw) / 2, y0 = std::max(12 * s, (H - ch) / 2);
  glowRect(dl, {x0, y0}, {x0 + cw, y0 + ch}, rgb(0x2878dc, 0.3f), 30 * s);
  cutPanel(dl, {x0, y0}, {x0 + cw, y0 + ch}, 18 * s, rgb(0x0e1e34, 0.96f), rgb(0x050c16, 0.97f), rgb(EDGE_HI));
  float ix0 = x0 + pad, iw = cw - 2 * pad, cx = x0 + cw / 2, y = y0 + 24 * s;
  auto centred = [&](const char* t, const Txt& tt) { text(dl, {cx - textW(t, tt) / 2, y}, t, tt); };
  centred("TERRAN DOMINION \xC2\xB7 BUNKER DEFENSE", T(12, MUTED, true, 4));
  y += 16 * s + 4 * s;
  Txt tt = T(38, 0xe8f4ff, true, 6);
  tt.glow = rgb(0x50aaff, 0.55f);
  centred("DEFENSE OF THE", tt);
  y += 40 * s;
  centred("DOMINION", tt);
  y += 40 * s + 6 * s;
  centred("Hold the bunker. Every unit inside fires with each click.", T(15, MUTED, true));
  y += 20 * s + 14 * s + 4 * s;
  // start buttons wait for the bunker and the first waves' units (or "start now")
  bool ready = skipWait_ || assetsSettled();
  bool unlocked = endlessUnlocked();
  int bw, bsc;
  bool best = bestEndless(bw, bsc);
  float bh = 60 * s;
  if (modeButton(dl, "##campaign", {ix0, y}, iw, "campaign", "CAMPAIGN", fmt("Clear %d waves of the swarm to win", cfg::victory_wave), false, ready)) start(Mode::Campaign);
  y += bh + 8 * s;
  std::string ed = unlocked ? (best ? fmt("Best: wave %d \xC2\xB7 score %d", bw, bsc) : "No wave limit \xE2\x80\x94 how long can you hold?") : "Locked \xE2\x80\x94 win the campaign to unlock";
  if (modeButton(dl, "##endless", {ix0, y}, iw, unlocked ? "endless" : "lock", "ENDLESS", ed, true, ready && unlocked)) start(Mode::Endless);
  y += bh + 8 * s;
  storage::Record a, m;
  if (readSave("autosave_v1", a) && validSave(a)) {
    if (modeButton(dl, "##continue", {ix0, y}, iw, "play", "CONTINUE", "Autosave \xE2\x80\x94 " + saveLabel(a), false, ready)) loadGame(true);
    y += bh + 8 * s;
  }
  if (readSave("save_v1", m) && validSave(m)) {
    if (modeButton(dl, "##loadgame", {ix0, y}, iw, "load", "LOAD GAME", "Saved game \xE2\x80\x94 " + saveLabel(m), false, ready)) loadGame(false);
    y += bh + 8 * s;
  }
  // HOW TO PLAY and SETTINGS share a row; SETTINGS opens the menu on its tab
  float hw = (iw - 8 * s) / 2;
  if (modeButton(dl, "##howto", {ix0, y}, hw, "help", "HOW TO PLAY", "Rules, controls, units", false, true)) showHowTo();
  if (modeButton(dl, "##settings", {ix0 + hw + 8 * s, y}, hw, "gfx", "SETTINGS", "Graphics, sound, display", false, true)) { menuTab_ = 1; openMenu(); }
  y += bh + 8 * s;
  y += 14 * s - 8 * s;
  // loading progress
  int done, total;
  models::progress(done, total);
  if (done < total) {
    y += 10 * s;
    dl->AddRectFilled({ix0, y}, {ix0 + iw, y + 4 * s}, rgb(0x0b1828));
    if (total) hgradRect(dl, {ix0, y}, {ix0 + iw * done / total, y + 4 * s}, rgb(0x2f6dff), rgb(0x7fc0ff));
    dl->AddRect({ix0, y}, {ix0 + iw, y + 4 * s}, rgb(0x1d3a5a));
    y += 9 * s;
    std::string lt = ready ? fmt("Ready \xE2\x80\x94 %d more files loading in the background", total - done) : fmt("Loading StarCraft II assets\xE2\x80\xA6 %d / %d", done, total);
    Txt lt2 = T(12, MUTED);
    float tw = textW(lt.c_str(), lt2) + (ready ? 0 : 8 * s + textW("start now", T(12, DIM)));
    float tx = cx - tw / 2;
    text(dl, {tx, y}, lt.c_str(), lt2);
    if (!ready) {
      float sx = tx + textW(lt.c_str(), lt2) + 8 * s;
      Txt sk = T(12, DIM);
      ImGui::SetCursorScreenPos({sx, y});
      Btn b = button("##skip", {textW("start now", sk), 15 * s});
      text(dl, {sx, y}, "start now", sk);
      dl->AddLine({sx, y + 14 * s}, {sx + textW("start now", sk), y + 14 * s}, rgb(DIM));
      if (b.clicked) skipAssetWait();
    }
    y += 16 * s;
  }
  // key help
  y += 12 * s;
  dl->AddLine({ix0, y}, {ix0 + iw, y}, rgb(0x1d3a5a));
  y += 12 * s;
  y += keyGrid(dl, ix0, y, iw) + 12 * s;
  float lh = wrapped(dl, {ix0, y}, iw, BLIZZARD_NOTICE, T(11, DIM), 1.35f);
  y += lh + 20 * s;
  ch = y - y0;
  ImGui::End();
}

void Game::drawHowTo() {
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  if (!layer("##howto", {0, 0}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  overlayBg(dl, W, H);
  float cw = std::min(560 * s, W - 24 * s), pad = 30 * s;
  static float ch = 700;
  float x0 = (W - cw) / 2, y0 = std::max(12 * s, (H - ch) / 2);
  glowRect(dl, {x0, y0}, {x0 + cw, y0 + ch}, rgb(0x2878dc, 0.3f), 30 * s);
  cutPanel(dl, {x0, y0}, {x0 + cw, y0 + ch}, 18 * s, rgb(0x0e1e34, 0.96f), rgb(0x050c16, 0.97f), rgb(EDGE_HI));
  float ix0 = x0 + pad, iw = cw - 2 * pad, cx = x0 + cw / 2, y = y0 + 24 * s;
  auto centred = [&](const char* t, const Txt& tt) { text(dl, {cx - textW(t, tt) / 2, y}, t, tt); };
  centred("FIELD MANUAL", T(12, MUTED, true, 4));
  y += 16 * s + 4 * s;
  Txt tt = T(30, 0xe8f4ff, true, 5);
  tt.glow = rgb(0x50aaff, 0.55f);
  centred("HOW TO PLAY", tt);
  y += 34 * s + 12 * s;
  // topic → rule rows
  const std::pair<const char*, std::string> rows[] = {
    {"GOAL", fmt("Hold the bunker against the Zerg swarm. Campaign: clear %d waves to win \xE2\x80\x94 that unlocks Endless mode.", cfg::victory_wave)},
    {"FIRE", "Click the ground: every unit inside the bunker fires one volley at that spot, for one round. Hold the button for full auto \xE2\x80\x94 it empties the magazine fast."},
    {"AMMO", "After each wave a Marine fetches a supply crate that refills the magazine. Out of ammo mid-wave? <kbd>R</kbd> calls a Viking: it fires missiles into the swarm and drops an ammo crate \xE2\x80\x94 one crate covers the wave."},
    {"MONEY", "Kills and cleared waves pay minerals and gas. Spend them on upgrades <kbd>1</kbd>\xE2\x80\x93<kbd>4</kbd> (damage, armor, hull, ammo) and on units in the roster <kbd>B</kbd>, which pauses the game."},
    {"UNITS", "The bunker has 6 slots. Marine (1 slot): all-round. Marauder (2): aims itself at the nearest enemies; its grenade blasts an area, slowing all in it, double damage to Armored. Ghost (2): bonus damage to Light, researches auto-Snipe."},
    {"POWERS", fmt("<kbd>N</kbd> nuke from wave %d. <kbd>T</kbd> Stimpack with six Marines (fewer with Super Stimpack's tiers), for Marines and Marauders. Bunker hull level 3: Shrike auto-turret and self-repair.", cfg::nuke_wave)},
    {"ZERG", fmt("Zerglings: fast, Light, bite the hull. Roaches from wave %d: Armored, spit acid from range. From wave %d the Zerglings run faster.", cfg::roach_wave, cfg::metabolic_wave)},
  };
  Txt topic = T(12, 0x9fd2ff, true, 2), rule = T(14, 0xc4d8ee);
  float tw = 0;
  for (auto& r : rows) tw = std::max(tw, textW(r.first, topic));
  for (auto& r : rows) {
    text(dl, {ix0, y + 2 * s}, r.first, topic);
    y += markup(dl, {ix0 + tw + 12 * s, y}, iw - tw - 12 * s, r.second, rule, 1.35f) + 7 * s;
  }
  y += 5 * s;
  dl->AddLine({ix0, y}, {ix0 + iw, y}, rgb(0x1d3a5a));
  y += 12 * s;
  y += keyGrid(dl, ix0, y, iw) + 12 * s;
  {   // TUTORIAL HINTS checkbox, centred
    const char* lbl = "TUTORIAL HINTS IN GAME \xE2\x80\x94 pause on each new mechanic";
    float w = 15 * s + 8 * s + richW(lbl, T(14, 0xbcd7f5, true, 0.5f));
    bool on = tutOn_;
    if (checkbox(dl, "##howtoTut", {cx - w / 2, y}, on, lbl)) setTutorial(on);
    y += 18 * s + 14 * s;
  }
  float bw = (iw - 8 * s) / 2, bh = 44 * s;
  auto rowBtn = [&](const char* id, float x, const char* svg, const char* label, bool gold) {
    ImGui::SetCursorScreenPos({x, y});
    Btn b = button(id, {bw, bh});
    bool hov = b.hovered;
    if (hov) glowRect(dl, b.a, b.b, rgb(0x46a0ff, 0.45f), 16 * s);
    cutPanel(dl, b.a, b.b, 10 * s, hov ? rgb(0x2a5a92) : rgb(0x1d3d63), hov ? rgb(0x173562) : rgb(0x0f2240), gold ? rgb(0x8a6a10) : hov ? rgb(EDGE_HI) : rgb(EDGE));
    Txt lt = T(17, gold ? GOLD : TEXT, true, 2);
    float w = 26 * s + 14 * s + textW(label, lt), tx = (b.a.x + b.b.x) / 2 - w / 2, cy = (b.a.y + b.b.y) / 2;
    svgIcon(dl, svg, {tx + 13 * s, cy}, 26 * s, gold ? rgb(GOLD) : rgb(0x9fd2ff));
    textC(dl, tx + 40 * s, cy, label, lt);
    return b.clicked;
  };
  bool tour = rowBtn("##howtoTour", ix0, "help", "HUD TOUR", true);
  bool back = rowBtn("##howtoBack", ix0 + bw + 8 * s, "home", "BACK", false);
  y += bh + 20 * s;
  ch = y - y0;
  ImGui::End();
  if (tour) startTour();
  else if (back) overlay_ = Overlay::Title;
}

void Game::drawResults() {
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  if (!layer("##results", {0, 0}, {W, H}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  overlayBg(dl, W, H);
  float cw = std::min(560 * s, W - 24 * s), pad = 30 * s;
  static float ch = 400;
  float x0 = (W - cw) / 2, y0 = std::max(12 * s, (H - ch) / 2);
  glowRect(dl, {x0, y0}, {x0 + cw, y0 + ch}, rgb(0x2878dc, 0.3f), 30 * s);
  cutPanel(dl, {x0, y0}, {x0 + cw, y0 + ch}, 18 * s, rgb(0x0e1e34, 0.96f), rgb(0x050c16, 0.97f), rgb(EDGE_HI));
  float ix0 = x0 + pad, iw = cw - 2 * pad, cx = x0 + cw / 2, y = y0 + 24 * s;
  bool camp = mode_ == Mode::Campaign;
  auto centred = [&](const std::string& t, const Txt& tt) { text(dl, {cx - textW(t.c_str(), tt) / 2, y}, t.c_str(), tt); };
  centred(resVictory_ ? "CAMPAIGN COMPLETE" : camp ? "CAMPAIGN" : "ENDLESS MODE", T(12, MUTED, true, 4));
  y += 20 * s;
  Txt tt = T(38, resVictory_ ? GOLD : 0xff8a7a, true, 6);
  tt.glow = resVictory_ ? rgb(0xffc83c, 0.6f) : rgb(0xff503c, 0.5f);
  centred(resVictory_ ? "VICTORY" : "BUNKER LOST", tt);
  y += 46 * s;
  std::string sub = resVictory_ ? fmt("All %d waves held. The Dominion stands.", cfg::victory_wave)
                    : camp ? fmt("Overrun on wave %d of %d.", wave_, cfg::victory_wave) : fmt("The swarm broke through on wave %d.", wave_);
  centred(sub, T(15, MUTED, true));
  y += 20 * s + 14 * s;
  float pulse = 0.5f - 0.5f * std::cos(uiTime_ * 2 * 3.14159f / 1.6f);
  auto badge = [&](const char* t, bool ic) {
    Txt bt = T(13, GOLD, true, 2);
    float w = textW(t, bt) + 24 * s + (ic ? 20 * s : 0), bx = cx - w / 2;
    glowRect(dl, {bx, y}, {bx + w, y + 24 * s}, rgb(0xffb43c, 0.25f + 0.5f * pulse), (4 + 10 * pulse) * s);
    dl->AddRectFilled({bx, y}, {bx + w, y + 24 * s}, rgb(0xa06e0a, 0.16f));
    dl->AddRect({bx, y}, {bx + w, y + 24 * s}, rgb(0x8a6a10));
    float tx = bx + 12 * s;
    if (ic) { svgIcon(dl, "endless", {tx + 7 * s, y + 12 * s}, 14 * s, rgb(GOLD)); tx += 20 * s; }
    textC(dl, tx, y + 12 * s, t, bt);
    y += 24 * s + 14 * s;
  };
  if (resNewly_) badge("ENDLESS MODE UNLOCKED", true);
  if (resRecord_) badge("NEW ENDLESS RECORD", false);
  const Rank& rk = getRank(kills_);
  int t = (int)gameT_;
  std::pair<std::string, std::string> stats[6] = {{"WAVE", std::to_string(wave_) + (camp ? fmt(" / %d", cfg::victory_wave) : "")}, {"TIME", fmt("%d:%02d", t / 60, t % 60)},
                                                   {"KILLS", std::to_string(kills_)}, {"SCORE", std::to_string(score_)}, {"RANK", rk.name},
                                                   {"MODE", camp ? "CAMPAIGN" : "ENDLESS"}};
  float sw = (iw - 12 * s) / 3, sh = 6 * s + 22 * s + 14 * s + 6 * s;
  for (int i = 0; i < 6; i++) {
    float sx = ix0 + (i % 3) * (sw + 6 * s), sy = y + (i / 3) * (sh + 6 * s);
    dl->AddRectFilled({sx, sy}, {sx + sw, sy + sh}, IM_COL32(10, 24, 42, 204));
    dl->AddRect({sx, sy}, {sx + sw, sy + sh}, rgb(0x1d3a5a));
    Txt vt = T(20, i == 4 ? rk.color : 0xe6f3ff, false, 0, true);
    text(dl, {sx + sw / 2 - textW(stats[i].second.c_str(), vt) / 2, sy + 6 * s}, stats[i].second.c_str(), vt);
    Txt lt = T(11, MUTED, true, 2);
    text(dl, {sx + sw / 2 - textW(stats[i].first.c_str(), lt) / 2, sy + 6 * s + 24 * s}, stats[i].first.c_str(), lt);
  }
  y += 2 * sh + 6 * s + 16 * s;
  float bw = (iw - 8 * s) / 2, bh = 44 * s;
  auto rowBtn = [&](const char* id, float x, const char* svg, const char* label, bool gold) {
    ImGui::SetCursorScreenPos({x, y});
    Btn b = button(id, {bw, bh});
    bool hov = b.hovered;
    if (hov) glowRect(dl, b.a, b.b, rgb(0x46a0ff, 0.45f), 16 * s);
    cutPanel(dl, b.a, b.b, 10 * s, hov ? rgb(0x2a5a92) : rgb(0x1d3d63), hov ? rgb(0x173562) : rgb(0x0f2240), gold ? rgb(0x8a6a10) : hov ? rgb(EDGE_HI) : rgb(EDGE));
    Txt lt = T(17, gold ? GOLD : TEXT, true, 2);
    float tw = 26 * s + 14 * s + textW(label, lt), tx = (b.a.x + b.b.x) / 2 - tw / 2, cy = (b.a.y + b.b.y) / 2;
    svgIcon(dl, svg, {tx + 13 * s, cy}, 26 * s, gold ? rgb(GOLD) : rgb(0x9fd2ff));
    textC(dl, tx + 40 * s, cy, label, lt);
    return b.clicked;
  };
  if (resVictory_) { if (rowBtn("##cont", ix0, "endless", "CONTINUE \xC2\xB7 ENDLESS", true)) continueEndless(); }
  else if (rowBtn("##again", ix0, "restart", "PLAY AGAIN", false)) restart();
  if (rowBtn("##main", ix0 + bw + 8 * s, "home", "MAIN MENU", false)) showTitle();
  y += bh + 20 * s;
  ch = y - y0;
  ImGui::End();
}

// ─── Tutorial: HUD tour + hints for new players ──────────────────────────────
// The first game opens with a HUD tour: the console and each command-card
// button in turn, spotlit (the rest of the screen dimmed), BACK / NEXT, and
// OK, LET'S ROLL at the end. After it, the first time a mechanic matters the
// game pauses on a short hint, spotlighting the control it is about, until
// GOT IT, Enter / Space / Esc, a key it names or a click anywhere else (which
// then does its job). Each shows once; MENU → TUTORIAL turns the hints off,
// replays them or runs the tour again (so does HOW TO PLAY on the title).
static const float TUT_GAP = 3;        // game seconds between two hints
static const float TUT_GRACE = 0.8f;   // s after a hint opens in which clicks are swallowed: mid-fight clicks must not close it unread

// In priority order; `after` = shown only once that one has been seen; `keys` = the
// keys the hint names, which close it and do their job
struct HintDef { const char *id, *after; std::vector<std::string> focus; const char *keys, *title; std::string text; };
static const std::vector<HintDef>& hintDefs() {
  static const std::vector<HintDef> h = {
    {"tour", nullptr, {}, "", "", ""},
    {"strike", nullptr, {"cmd-air"}, "r", "OUT OF AMMO",
     "Call an airstrike: press <kbd>R</kbd> or click STRIKE. A Viking fires a missile at every Zerg group and drops an ammo crate by the bunker \xE2\x80\x94 that crate is also this wave's resupply."},
    {"cleared", nullptr, {"res-min", "res-gas"}, "", "WAVE CLEARED",
     "Kills and cleared waves pay minerals and vespene gas. A Marine runs out for the wave's supply crate (unless the Viking's crate already came in), the magazine is refilled, then the next wave comes. The game autosaves after every wave."},
    {"upgrade", "cleared", {"cmd-dmg", "cmd-ammo"}, "1234", "UPGRADES",
     "You can afford an upgrade: press <kbd>1</kbd>\xE2\x80\x93<kbd>4</kbd> or click a lit button \xE2\x80\x94 damage, armor, bunker hull, ammo storage."},
    {"roster", "cleared", {"cmd-roster"}, "b", "BUNKER ROSTER",
     "Between waves is a good moment to press <kbd>B</kbd>: hire more units into the bunker's free slots \xE2\x80\x94 every one of them fires with each volley."},
    {"roach", nullptr, {}, "", "ROACHES",
     "Roaches are <b>Armored</b> and spit acid from range. Marauders deal Armored enemies double damage; armor (<kbd>2</kbd>) cuts the damage the bunker takes."},
    {"nuke", nullptr, {"cmd-nuke"}, "n", "NUKE READY",
     fmt("From wave %d, press <kbd>N</kbd> or click NUKE: a Ghost marks the middle of the swarm and the warhead wipes out everything in the blast. It recharges after each launch.", cfg::nuke_wave)},
    {"hull", nullptr, {"hp"}, "", "BUNKER DAMAGED",
     "Below half HP the bunker burns \xE2\x80\x94 if it falls, the game is lost. Armor reduces the damage; bunker hull level 3 mounts a Shrike turret and repairs itself."},
    {"stim", nullptr, {"cmd-stim"}, "t", "STIMPACK",
     fmt("Your squad can stim: press <kbd>T</kbd> or click STIM. For %d s the Marines and Marauders fire 50%% faster, for %d HP per Marine and %d per Marauder from the bunker (free with the Super Stimpack research).", (int)cfg::stim_duration, (int)cfg::stim_hp, (int)cfg::stim_hp_marauder)},
  };
  return h;
}
// The tour: welcome, the console, the ten command-card buttons (in card order,
// titled from CMD_INFO), ready
struct TourDef { std::vector<std::string> focus; std::string cmd, title, text; };
static const std::vector<TourDef>& tourDefs() {
  static const std::vector<TourDef> t = [] {
    std::vector<TourDef> v = {
      {{}, "", "WELCOME, COMMANDER", "The Zerg attack the bunker from every side, wave after wave. <b>Click on the ground</b> to fire: every unit inside the bunker shoots one volley at that spot. <b>Hold the button</b> for full auto. First, a look at your console."},
      {{"hp"}, "", "BUNKER HP", "The squad's life. Below half the bunker burns; at zero it falls and the game is lost."},
      {{"ammo"}, "", "AMMO", "One round per volley \xE2\x80\x94 full auto empties it fast. The magazine is refilled after every wave."},
      {{"res-min", "res-gas"}, "", "MINERALS & GAS", "Paid for every kill and every cleared wave. Spend them on the command card and in the roster."},
      {{"cmd-grid"}, "", "COMMAND CARD", "Your orders and upgrades. The hotkey is in the corner of each button, the cost in minerals and gas under it; in game, hover a button for its details."},
    };
    for (const char* k : {"air", "nuke", "roster", "view", "menu", "dmg", "armor", "hp", "ammo", "stim"}) v.push_back({{std::string("cmd-") + k}, k, "", ""});
    v.push_back({{"cmd-grid"}, "", "READY", ""});
    return v;
  }();
  return t;
}
// The tour's own words for each button: shorter than the hover tooltips (CMD_INFO),
// which name things a new player has not met yet
static std::string tourText(const std::string& k) {
  if (k == "air") return "When the magazine is empty: a Viking fires a missile at every Zerg group and drops an ammo crate by the bunker.";
  if (k == "nuke") return fmt("From wave %d (the button shows W%d until then): a nuke on the middle of the swarm wipes out everything in a huge radius. It recharges after each launch.", cfg::nuke_wave, cfg::nuke_wave);
  if (k == "roster") return "Hire Marines, Marauders and Ghosts into the bunker's 6 slots and research their upgrades. Every unit inside fires with each volley. The game pauses while it is open.";
  if (k == "view") return "Back to the overview. The mouse wheel zooms in on the bunker; INSERT / DELETE rotate the view.";
  if (k == "menu") return "Save and load, settings, the controls and this tutorial. The game pauses while it is open.";
  if (k == "dmg") return "Every round from every unit hits harder. Three levels.";
  if (k == "armor") return "The bunker takes less damage from bites and acid. Three levels.";
  if (k == "hp") return "+250 max HP per level. Level 3 adds a Shrike auto-turret and self-repair.";
  if (k == "ammo") return "+10 rounds in the magazine per level.";
  if (k == "stim") return fmt("With six Marines in the bunker (fewer with Super Stimpack's tiers): %d s of 50%% faster fire for the Marines and Marauders, paid with a little bunker HP.", (int)cfg::stim_duration);
  return "";
}

void Game::tutLoad() {   // ids of removed hints drop out
  tutOn_ = storage::get("tutorial", "on") != "off";
  tutSeen_.clear();
  std::stringstream ss(storage::get("tutorial_seen"));
  std::string id;
  while (std::getline(ss, id, ','))
    for (const HintDef& h : hintDefs())
      if (id == h.id && !tutSeen(id)) tutSeen_.push_back(id);
}
void Game::tutSave() {
  std::string seen;
  for (const auto& id : tutSeen_) seen += (seen.empty() ? "" : ",") + id;
  storage::set("tutorial", tutOn_ ? "on" : "off");
  storage::set("tutorial_seen", seen);
}
bool Game::tutSeen(const std::string& id) const { return std::find(tutSeen_.begin(), tutSeen_.end(), id) != tutSeen_.end(); }
bool Game::tutLeft() const {
  if (!tutOn_) return false;
  for (const HintDef& h : hintDefs()) if (strcmp(h.id, "tour") && !tutSeen(h.id)) return true;
  return false;
}
void Game::tutCounts(int& seen, int& total) const {
  total = (int)hintDefs().size();
  seen = (int)tutSeen_.size();
}
bool Game::tutWhen(const std::string& id) {
  bool combat = wavePhase_ == WavePhase::Combat;
  if (id == "tour") return true;
  if (id == "strike") return combat && ammo_ <= 0 && airCD_ <= 0 && !vikingActive_;
  if (id == "cleared") return wavePhase_ == WavePhase::Cleared;
  if (id == "upgrade") {
    int lv[4] = {dmgLvl_, armorLvl_, hpLvl_, ammoLvl_};
    const int(*costs[4])[2] = {cfg::dmg_costs, cfg::armor_costs, cfg::hp_costs, cfg::ammo_costs};
    for (int i = 0; i < 4; i++) if (lv[i] < 3 && minerals_ >= costs[i][lv[i]][0] && gas_ >= costs[i][lv[i]][1]) return true;
    return false;
  }
  if (id == "roster") return wave_ >= 2 && !combat;
  if (id == "roach") { for (auto& r : roaches_) if (!r->dead) return true; return false; }
  if (id == "nuke") return wave_ >= cfg::nuke_wave && combat && nukeCD_ <= 0 && !nukeInFlight_;
  if (id == "hull") return combat && bunkerHp_ < maxHp_ * 0.5f;
  if (id == "stim") return combat && stimUnlocked() && stimT_ <= 0;
  return false;
}

void Game::tutorialTick() {   // live frames only (paused / title: never)
  // phase: the frame that wins the campaign still runs to the end — a hint
  // raised there would hide under the victory card
  if (phase_ != Phase::Playing || !tutOn_ || !tutRun_ || tutOpen_ || gameT_ < tutNext_) return;
  for (const HintDef& h : hintDefs()) {
    if (tutSeen(h.id) || (h.after && !tutSeen(h.after)) || !tutWhen(h.id)) continue;
    if (!strcmp(h.id, "tour")) { startTour(); return; }
    tutSeen_.push_back(h.id);
    tutSave();
    tutOpenStep({h.id, h.title, h.text, h.focus, h.keys, ""});
    return;
  }
}
// One box for both: a hint (GOT IT) or a tour page (BACK / NEXT / OK, LET'S ROLL)
void Game::tutOpenStep(const TutStep& st) {
  tutStep_ = st;
  tutOpen_ = true;
  tutShownAt_ = uiTime_;
}
void Game::tourPage(int i) {
  i = std::clamp(i, 0, (int)tourDefs().size() - 1);
  tourI_ = i;
  const TourDef& p = tourDefs()[i];
  TutStep st;
  st.id = "tour";
  st.focus = p.focus;
  if (!p.cmd.empty()) {
    const CmdInfo& c = CMD_INFO.at(p.cmd);
    st.title = c.t;
    st.titleKey = c.k;
    st.text = tourText(p.cmd);
  } else if (i == (int)tourDefs().size() - 1) {
    st.title = p.title;
    st.text = std::string("Those are your controls. ") + (tutLeft() ? "A hint will pause the game the first time something new happens \xE2\x80\x94 read it, then carry on." : "In game, hover any button for its cost and details.");
  } else {
    st.title = p.title;
    st.text = p.text;
  }
  tutOpenStep(st);
}
// From the title / results (HOW TO PLAY, the menu) the card is hidden for the
// tour, so the console shows, and comes back after it
void Game::startTour() {
  tutDismiss();
  if (rosterPaused_) closeRoster();   // it covered the console the tour lights up
  if (phase_ == Phase::Menu) newGameState();   // title: nothing to keep, show a new game's console
  if (phase_ != Phase::Playing && overlay_ != Overlay::None) { tourOverlay_ = true; tourSavedOverlay_ = overlay_; overlay_ = Overlay::None; }
  if (!tutSeen("tour")) tutSeen_.push_back("tour");
  tutSave();
  tourPage(0);
}
// A page whose control is off-screen (a narrow window cuts the console) is
// skipped: the screen would dim with nothing lit. The last page always shows.
bool Game::tourShown(int i) const {
  float a, b, c, d;
  return i == 0 || i == (int)tourDefs().size() - 1 || focusRect(tourDefs()[i].focus, a, b, c, d);
}
void Game::tutNextPage() {   // GOT IT / NEXT / OK, LET'S ROLL
  if (tourI_ < 0 || tourI_ == (int)tourDefs().size() - 1) { tutDismiss(); return; }
  int i = tourI_ + 1;
  while (!tourShown(i)) i++;
  tourPage(i);
}
void Game::tutBack() {
  if (tourI_ <= 0) return;
  int i = tourI_ - 1;
  while (!tourShown(i)) i--;
  tourPage(i);
}
void Game::tutDismiss() {
  if (!tutOpen_) return;
  tutOpen_ = false;
  tourI_ = -1;
  tutNext_ = gameT_ + TUT_GAP;
  if (tourOverlay_) {
    tourOverlay_ = false;
    if (phase_ != Phase::Playing) overlay_ = tourSavedOverlay_;
  }
}
void Game::setTutorial(bool on) {
  tutOn_ = on;
  if (!on && tourI_ < 0) tutDismiss();
  tutSave();
}
void Game::replayTutorial() {
  tutSeen_.clear();
  tutOn_ = true;
  tutNext_ = 0;
  tutSave();
}
void Game::tutSkip() {
  if (tourI_ >= 0) { tutDismiss(); return; }   // the tour only: the hints stay on
  setTutorial(false);
  setStatus("Tutorial hints off \xE2\x80\x94 MENU \xE2\x86\x92 TUTORIAL turns them back on");
}

// A press anywhere outside the box (and the menu) closes a hint first — the press
// then does what it would have done (fire, a command button on release). In the
// first TUT_GRACE s the press is swallowed instead (a blocker layer stops the HUD
// buttons too): a player clicking away mid-fight would otherwise close the hint
// unread. The tour only moves with its own buttons and keys.
bool Game::tutorialPress() {
  if (!tutOpen_ || menuOpen_ || cheatOpen_) return true;
  double cx, cy;
  glfwGetCursorPos(win_, &cx, &cy);
  if (cx >= tutBoxX0_ && cx < tutBoxX1_ && cy >= tutBoxY0_ && cy < tutBoxY1_) return false;   // the box's own buttons
  if (tourI_ >= 0 || uiTime_ - tutShownAt_ < TUT_GRACE) return false;
  tutDismiss();
  return true;
}
// HUD tour: Enter / Space / → next, ← back, Esc ends it; other keys wait. Hint:
// Enter / Space / Esc close it; a key it names (R, B, N, T, 1-4) closes it and does its job
bool Game::tutorialKey(int key) {
  if (!tutOpen_) return false;
  bool enter = key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_SPACE;
  if (tourI_ >= 0) {
    if (enter || key == GLFW_KEY_RIGHT) tutNextPage();
    else if (key == GLFW_KEY_LEFT) tutBack();
    else if (key == GLFW_KEY_ESCAPE) tutDismiss();
    return true;
  }
  if (enter || key == GLFW_KEY_ESCAPE) { tutDismiss(); return true; }
  char c = key >= GLFW_KEY_A && key <= GLFW_KEY_Z ? (char)('a' + key - GLFW_KEY_A) : key >= GLFW_KEY_0 && key <= GLFW_KEY_9 ? (char)('0' + key - GLFW_KEY_0) : 0;
  if (!c || tutStep_.keys.find(c) == std::string::npos) return true;
  tutDismiss();
  return false;
}

// Union of a step's HUD controls drawn in the last frame, or false when there are
// none or it is off-screen
bool Game::focusRect(const std::vector<std::string>& ids, float& x0, float& y0, float& x1, float& y1) const {
  x0 = y0 = 1e30f;
  x1 = y1 = -1e30f;
  bool any = false;
  for (const auto& id : ids) {
    auto it = hudRects_.find(id);
    if (it == hudRects_.end() || it->second.frame < uiFrame_ - 1) continue;
    const HudRect& r = it->second;
    x0 = std::min(x0, r.x0); y0 = std::min(y0, r.y0); x1 = std::max(x1, r.x1); y1 = std::max(y1, r.y1);
    any = true;
  }
  return any && x1 - x0 > 0 && y1 - y0 > 0 && x0 >= -1 && y0 >= -1 && x1 <= winW_ + 1 && y1 <= winH_ + 1;
}

// Spotlight on the step's controls (the rest dimmed, a gold ring with a ripple)
// and the box just above it, centred on it as far as the window allows. No
// control: no dimming, the box centred 160 px above the bottom.
void Game::drawTutorial() {
  if (!tutOpen_) return;
  const float s = sc(), W = (float)winW_, H = (float)winH_;
  const bool tour = tourI_ >= 0, last = tour && tourI_ == (int)tourDefs().size() - 1, noGame = phase_ != Phase::Playing;
  float fx0, fy0, fx1, fy1;
  const bool focus = focusRect(tutStep_.focus, fx0, fy0, fx1, fy1);
  if (focus && layer("##tutDim", {0, 0}, {W, H}, false)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(fx0 - 4 * s, fy0 - 4 * s), b(fx1 + 4 * s, fy1 + 4 * s);
    ImU32 dim = IM_COL32(0, 4, 10, 158);
    dl->AddRectFilled({0, 0}, {W, a.y}, dim);
    dl->AddRectFilled({0, b.y}, {W, H}, dim);
    dl->AddRectFilled({0, a.y}, {a.x, b.y}, dim);
    dl->AddRectFilled({b.x, a.y}, {W, b.y}, dim);
    glowRect(dl, a, b, rgb(GOLD, 0.85f), 16 * s);
    dl->AddRect(a, b, rgb(GOLD), 0, 0, 2 * s);
    float t = std::fmod(uiTime_, 1.1f) / 1.1f, k = 1 + 0.12f * t;   // ripple: scale 1 → 1.12, fading out
    ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2), hw(((b.x - a.x) / 2 + 6 * s) * k, ((b.y - a.y) / 2 + 6 * s) * k);
    dl->AddRect({c.x - hw.x, c.y - hw.y}, {c.x + hw.x, c.y + hw.y}, rgb(GOLD, 0.9f * (1 - t)), 0, 0, 2 * s);
    ImGui::End();
  } else if (focus) ImGui::End();
  if (tour || uiTime_ - tutShownAt_ < TUT_GRACE) {   // clicks outside the box are swallowed
    if (layer("##tutBlock", {0, 0}, {W, H}, true)) {
      ImGui::SetCursorScreenPos({0, 0});
      button("##tutBlockBtn", {W, H});
    }
    ImGui::End();
  }

  // ── the box ──
  const float bw = std::min(480 * s, W - 24 * s), padX = 16 * s, iw = bw - 2 * padX;
  Txt head = T(11, GOLD, true, 3), num = T(11, MUTED, false, 1, true), pausedT = T(11, MUTED, true, 2);
  Txt tt = T(20, 0xe8f4ff, true, 3), body = T(15, 0xc4d8ee);
  Txt btnT = T(15, GOLD, true, 2), skipT = T(13, DIM);
  const float btnH = 30 * s;
  const float textHt = markup(nullptr, {0, 0}, iw, tutStep_.text, body, 1.38f);
  const float bh = 11 * s + textH(head) + 3 * s + textH(tt) + 4 * s + textHt + 10 * s + btnH + 12 * s;
  float x0 = (W - bw) / 2, y1 = H - 160 * s;
  if (focus) {
    x0 = std::max(12 * s, std::min(W - bw - 12 * s, (fx0 + fx1) / 2 - bw / 2));
    y1 = fy0 - 14 * s;
  }
  float y0 = std::max(8 * s, y1 - bh);
  y1 = y0 + bh;
  tutBoxX0_ = x0; tutBoxY0_ = y0; tutBoxX1_ = x0 + bw; tutBoxY1_ = y1;
  if (!layer("##tutBox", {x0, y0}, {bw, bh}, true)) { ImGui::End(); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  glowRect(dl, {x0, y0}, {x0 + bw, y1}, rgb(0xffc83c, 0.22f), 26 * s);
  cutPanel(dl, {x0, y0}, {x0 + bw, y1}, 12 * s, rgb(0x102036, 0.97f), rgb(0x060d18, 0.97f), rgb(GOLD));
  float x = x0 + padX, y = y0 + 11 * s;
  {   // head: TUTORIAL / HUD TOUR · n / N · ❙❙ PAUSED
    const char* kind = tour ? "HUD TOUR" : "TUTORIAL";
    text(dl, {x, y}, kind, head);
    int seen, total;
    tutCounts(seen, total);
    std::string n = tour ? fmt("%d / %d", tourI_ + 1, (int)tourDefs().size()) : fmt("%d / %d", seen, total);
    text(dl, {x + textW(kind, head) + 10 * s, y}, n.c_str(), num);
    if (!noGame) {   // the tour from the title: no game to pause
      const char* ps = "\xE2\x9D\x99\xE2\x9D\x99 PAUSED";
      text(dl, {x0 + bw - padX - textW(ps, pausedT), y}, ps, pausedT);
    }
    y += textH(head) + 3 * s;
  }
  text(dl, {x, y}, tutStep_.title.c_str(), tt);
  if (!tutStep_.titleKey.empty()) kbdChip(dl, x + textW(tutStep_.title.c_str(), tt) + 8 * s, y + textH(tt) / 2, tutStep_.titleKey.c_str(), 13, 6);
  y += textH(tt) + 4 * s;
  markup(dl, {x, y}, iw, tutStep_.text, body, 1.38f);
  // row: Skip · BACK · GOT IT / NEXT / OK, LET'S ROLL  [ENTER]
  const float ry = y1 - 12 * s - btnH, rcy = ry + btnH / 2;
  {
    const char* sk = tour ? "Skip the tour" : "Skip the tutorial";
    ImGui::SetCursorScreenPos({x, rcy - 9 * s});
    Btn b = button("##tutSkip", {textW(sk, skipT), 18 * s});
    Txt st = skipT;
    if (b.hovered) st.col = rgb(MUTED);
    textC(dl, x, rcy, sk, st);
    dl->AddLine({x, rcy + textH(st) / 2}, {x + textW(sk, st), rcy + textH(st) / 2}, st.col);
    if (b.clicked) { tutSkip(); ImGui::End(); return; }
  }
  auto tutBtn = [&](const char* id, float bx1, const char* label, bool gold, bool key) {   // right-aligned at bx1; returns clicked
    float w = 32 * s + textW(label, btnT) + (key ? 8 * s + kbdChip(nullptr, 0, 0, "ENTER", 11, 4) : 0), bx0 = bx1 - w;
    ImGui::SetCursorScreenPos({bx0, ry});
    Btn b = button(id, {w, btnH});
    if (b.hovered) glowRect(dl, b.a, b.b, gold ? rgb(0xffc83c, 0.45f) : rgb(0x46a0ff, 0.45f), 12 * s);
    cutPanel(dl, b.a, b.b, 6 * s, gold ? rgb(0x3a2e0e) : rgb(0x1d3b5e), gold ? rgb(0x211a08) : rgb(0x10223a),
             gold ? (b.hovered ? rgb(GOLD) : rgb(0x8a6a10)) : (b.hovered ? rgb(EDGE_HI) : rgb(EDGE)));
    Txt lt = btnT;
    lt.col = gold ? rgb(GOLD) : rgb(0xbfe0ff);
    textC(dl, bx0 + 16 * s, rcy, label, lt);
    if (key) kbdChip(dl, bx0 + 16 * s + textW(label, lt) + 8 * s, rcy, "ENTER", 11, 4);
    return std::make_pair(b.clicked, bx0);
  };
  auto ok = tutBtn("##tutOk", x0 + bw - padX, !tour ? "GOT IT" : last ? "OK, LET'S ROLL" : "NEXT", true, true);
  bool back = false;
  if (tour && tourI_ > 0) back = tutBtn("##tutBack", ok.second - 10 * s, "\xE2\x97\x80 BACK", false, false).first;
  ImGui::End();
  if (ok.first) tutNextPage();
  else if (back) tutBack();
}

// ─── HOW TO PLAY (title): the rules on one card ──────────────────────────────
// (the in-game hints teach them one at a time)
void Game::showHowTo() {
  if (phase_ != Phase::Menu) return;
  overlay_ = Overlay::HowTo;
}

// ─── Menus: open / close ─────────────────────────────────────────────────────
void Game::openMenu() { menuOpen_ = true; saveMsgSet_ = false; }
void Game::closeMenu() { menuOpen_ = false; }
void Game::openRoster() {
  if (phase_ != Phase::Playing && phase_ != Phase::Menu) return;
  rosterPaused_ = true;
  slotOverlayTarget_ = 1;
}
void Game::closeRoster() {
  rosterPaused_ = false;
  slotOverlayTarget_ = 0;
  bunkerHoverTimer_ = 0;
}
void Game::setGfx(bool high) {
  gfxHigh_ = high;
  applyDNLights(dnLit());   // light levels for this quality, blended with day/night (and a flare)
  storage::set("gfx", high ? "HIGH" : "LOW");
}
// Window / fullscreen and resolution, applied at once and remembered
void Game::setDisplay(bool fullscreen, display::Size sz) {
  display::apply(win_, fullscreen, sz);
  storage::set("display", fullscreen ? "fullscreen" : "windowed");
  storage::set("resolution", fmt("%dx%d", sz.w, sz.h));
}
void Game::setGpuInfo(std::vector<display::Gpu> gpus, const std::string& running, bool dedicatedPref) {
  gpus_ = std::move(gpus);
  gpuRunning_ = running;
  gpuChoiceDedicated_ = dedicatedPref;
  // which card the driver gave us: compare the GL renderer with the adapters' names
  gpuRunningDedicated_ = dedicatedPref;
  for (const auto& g : gpus_) {
    std::string first = g.name.substr(0, g.name.find(' '));   // vendor word ("NVIDIA", "Intel(R)", "AMD")
    if (!first.empty() && running.find(first) != std::string::npos) gpuRunningDedicated_ = g.dedicated;
  }
}

void Game::toggleMute() {
  audio::setMuted(!audio::muted());
  storage::set("muted", audio::muted() ? "1" : "0");
}

// ─── Performance overlay (F3) ────────────────────────────────────────────────
void Game::drawPerf() {
  if (!perfOverlay_ || perfShown_.frames == 0) return;
  const float s = sc();
  const Perf& p = perfShown_;
  double n = p.frames;
  std::string lines[] = {
    fmt("%.0f FPS   frame %.2f ms (worst %.1f)", n / p.frame, p.frame / n * 1000, p.worst * 1000),
    fmt("CPU  update %.2f  render %.2f  UI %.2f ms", p.update / n * 1000, p.render / n * 1000, p.ui / n * 1000),
    fmt("workers %d   draws %d   particles %d+%d", jobs::workers(), R.drawCount(), (int)glow_.p.size(), (int)smoke_.p.size()),
    fmt("enemies %d   bullets %d   acid %d", (int)(zerglings_.size() + roaches_.size()), (int)bullets_.size(), (int)acids_.size()),
    gpuFx() ? fmt("GPU particles: up to %d live", gpuFx_.live()) : std::string(gpuFx_.ok() ? "GPU particles: off" : "GPU particles: unavailable"),
  };
  Txt t = T(12, 0x9fffb0, false, 0, true);
  float w = 0;
  for (auto& l : lines) w = std::max(w, textW(l.c_str(), t));
  float x = canvasX_ + 8 * s, y = canvasY_ + 8 * s, lh = 15 * s;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled({x - 6 * s, y - 4 * s}, {x + w + 6 * s, y + (sizeof lines / sizeof lines[0]) * lh + 4 * s}, IM_COL32(0, 0, 0, 170));
  for (auto& l : lines) { text(dl, {x, y}, l.c_str(), t); y += lh; }
}

// ─── Frame UI ────────────────────────────────────────────────────────────────
void Game::drawUI(float dt) {
  uiTime_ += dt;
  uiFrame_++;
  statusFlash_ = std::max(0.f, statusFlash_ - dt);
  drawOverlayCanvas();
  drawSlotOverlay();
  drawTopBar();
  drawConsole(dt);
  drawFlashes();
  drawRoster();
  drawTooltip();
  if (overlay_ == Overlay::Title) drawTitle();
  else if (overlay_ == Overlay::Results) drawResults();
  else if (overlay_ == Overlay::HowTo) drawHowTo();
  drawTutorial();   // above the HUD (spotlight); the cheat console and the menu stay above it
  drawCheat();
  drawMenu();
  drawPerf();
}

// ─── Input ───────────────────────────────────────────────────────────────────
void Game::onScroll(double dy) {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantCaptureMouse) return;
  if (mx_ < 0 || mx_ > 1 || my_ < 0 || my_ > 1) return;   // the canvas only (as the web wheel listener)
  zoomView((float)glm::clamp(dy, -3.0, 3.0) * 0.12f);   // ~8 notches overview → close-up
}

// Fire: a press fires one volley; held past HOLD_DELAY the button keeps firing at
// the gun's rate (shootCD_) where the cursor is — full auto, which empties the
// magazine fast. GLFW reports the release wherever the cursor is (and on focus loss).
void Game::onMouseButton(int button, int action) {
  if (button != GLFW_MOUSE_BUTTON_LEFT) return;
  if (action == GLFW_RELEASE) { holding_ = false; return; }
  if (action != GLFW_PRESS) return;
  if (!tutorialPress()) return;   // a tutorial box open: the press may be its own or swallowed
  if (ImGui::GetIO().WantCaptureMouse) return;
  if (phase_ != Phase::Playing || isPaused()) return;
  if (mx_ < 0 || mx_ > 1 || my_ < 0 || my_ > 1) return;
  holding_ = true;
  holdT_ = 0;
  double cx, cy;
  glfwGetCursorPos(win_, &cx, &cy);
  glm::vec3 pt;
  if (!groundPoint(cx, cy, pt)) return;
  if (ammo_ > 0) shoot(pt.x, pt.z);
  else setStatus("No ammo \xE2\x80\x94 R: Airstrike  N: Nuke");
}
void Game::holdFire(float dt) {   // live frames only: frozen while paused
  if (!holding_) return;
  holdT_ += dt;
  if (holdT_ < HOLD_DELAY || shootCD_ > 0 || ammo_ <= 0) return;
  double cx, cy;
  glfwGetCursorPos(win_, &cx, &cy);
  glm::vec3 pt;
  if (groundPoint(cx, cy, pt)) shoot(pt.x, pt.z);
}

void Game::onChar(unsigned) {}

void Game::onKey(int key, int action, int mods) {
  if (action == GLFW_RELEASE) return;
  bool repeat = action == GLFW_REPEAT;
  if (cheatOpen_) {   // the console's input handles the rest
    if (key == GLFW_KEY_ESCAPE) closeCheat();
    return;
  }
  if (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_ALT)) return;   // shortcuts are not game keys
  if (ImGui::GetIO().WantTextInput) return;
  if (key == GLFW_KEY_F3 && !repeat) { perfOverlay_ = !perfOverlay_; return; }   // performance overlay
  // view controls work everywhere
  if (key == GLFW_KEY_INSERT || key == GLFW_KEY_DELETE) { if (!repeat) rotateView(key == GLFW_KEY_INSERT ? -1 : 1); return; }
  if (key == GLFW_KEY_HOME) { resetView(); return; }
  if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) { zoomView(0.15f); return; }
  if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) { zoomView(-0.15f); return; }
  if (repeat) return;   // held keys must not flicker the menu / roster
  if (menuOpen_) {
    if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_M) closeMenu();
    else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) menuTab_ = (menuTab_ + (key == GLFW_KEY_DOWN ? 1 : MENU_TAB_COUNT - 1)) % MENU_TAB_COUNT;
    return;
  }
  if (tutorialKey(key)) return;   // a tutorial box open: its keys (a key a hint names falls through and does its job)
  if (phase_ != Phase::Playing) {   // title / results: only the menu (for load and settings)
    if (key == GLFW_KEY_M || key == GLFW_KEY_ESCAPE) openMenu();
    return;
  }
  if (rosterPaused_) {
    if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_B) closeRoster();
    return;
  }
  if (key >= GLFW_KEY_1 && key <= GLFW_KEY_4) {
    const char* t[4] = {"dmg", "armor", "hp", "ammo"};
    buy(t[key - GLFW_KEY_1]);
  }
  if (key == GLFW_KEY_K) toggleMute();
  if (key == GLFW_KEY_R) doAirstrike();
  if (key == GLFW_KEY_N) doNuke();
  if (key == GLFW_KEY_T && !(cheatBuf_.size() >= 2 && cheatBuf_.substr(cheatBuf_.size() - 2) == "do")) doStim();   // not while typing "dotd"
  if (key == GLFW_KEY_B) openRoster();
  if (key == GLFW_KEY_Q) setGfx(!gfxHigh_);
  if (key == GLFW_KEY_M) openMenu();
  if (key == GLFW_KEY_Y) toggleDayNight();
  if (key == GLFW_KEY_ESCAPE) openMenu();
  // secret cheat sequence: d-o-t-d
  if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
    cheatBuf_ += (char)('a' + (key - GLFW_KEY_A));
    if (cheatBuf_.size() > 4) cheatBuf_ = cheatBuf_.substr(cheatBuf_.size() - 4);
    if (cheatBuf_ == "dotd") { cheatBuf_.clear(); openCheat(); }
  } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
    cheatBuf_ += (char)('0' + (key - GLFW_KEY_0));
    if (cheatBuf_.size() > 4) cheatBuf_ = cheatBuf_.substr(cheatBuf_.size() - 4);
  }
}
