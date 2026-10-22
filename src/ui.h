// Drawing helpers for the Terran console look of the web HUD (css/hud.css):
// fonts, letter-spaced text, text with resource icons, cut-corner gradient
// panels, the stroke icons of index.html's SVG sprite, and a few widgets.
#pragma once
#include <string>

#include "imgui.h"

namespace ui {
// ─── palette (css :root) ─────────────────────────────────────────────────────
constexpr ImU32 rgb(unsigned hex, float a = 1.f) {
  return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, (int)(a * 255 + 0.5f));
}
constexpr unsigned EDGE = 0x2b5684, EDGE_HI = 0x5cb8ff, TEXT = 0xd8e8fb, MUTED = 0x7f9cbc, DIM = 0x4c6683, MINERAL = 0x56b8ff,
                   GAS = 0x5ee69c, GOLD = 0xffd24a, WARN = 0xffb347, DANGER = 0xff5a4f, OK = 0x4ee07a;

// ─── setup ───────────────────────────────────────────────────────────────────
bool init(float scale);   // fonts (Bahnschrift / Consolas / Segoe UI Symbol) and HUD icons; call before the first frame
float S();                // UI scale (CSS px → window px)
ImFont* font(float cssPx, bool mono = false);
ImTextureID icon(const std::string& rel);   // a HUD PNG (assets/ui), 0 if missing

// ─── text ────────────────────────────────────────────────────────────────────
struct Txt {
  float px = 13;          // CSS px
  bool mono = false, bold = false;
  float spacing = 0;      // letter-spacing, CSS px
  ImU32 col = rgb(TEXT);
  ImU32 glow = 0;         // text-shadow glow colour (0 = none)
  bool shadow = false;    // dark drop shadow
};
float textW(const char* s, const Txt& t);
float textH(const Txt& t);
void text(ImDrawList* dl, ImVec2 p, const char* s, const Txt& t);   // p = top-left
// ◆ / ▲ in s are drawn as the mineral / gas icons (1.2em), as the web status line does
float richW(const char* s, const Txt& t);
void rich(ImDrawList* dl, ImVec2 p, const char* s, const Txt& t);
// wraps s into lines of at most w; returns the height drawn
float wrapped(ImDrawList* dl, ImVec2 p, float w, const char* s, const Txt& t, float lineH = 1.35f, bool draw = true);

// ─── shapes ──────────────────────────────────────────────────────────────────
ImU32 lerpCol(ImU32 a, ImU32 b, float t);
ImU32 alpha(ImU32 c, float a);   // multiply alpha
// clip-path polygon with the top-left and bottom-right corners cut by `cut`
void cutPanel(ImDrawList* dl, ImVec2 a, ImVec2 b, float cut, ImU32 top, ImU32 bottom, ImU32 border, float borderW = 1);
void gradRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom);
void hgradRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 left, ImU32 right);
void glowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float radius);   // soft outer glow (box-shadow)
void pie(ImDrawList* dl, ImVec2 a, ImVec2 b, float frac, ImU32 col);          // conic sweep from 12 o'clock, clipped to the rect
void radial(ImDrawList* dl, ImVec2 c, ImVec2 r, ImU32 inner, ImU32 outer, float from = 0);   // elliptic gradient

// ─── icons (index.html's 24×24 stroke sprite) ────────────────────────────────
void svgIcon(ImDrawList* dl, const char* name, ImVec2 center, float size, ImU32 col);
void image(ImDrawList* dl, ImTextureID tex, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE);

// ─── widgets ─────────────────────────────────────────────────────────────────
// An invisible button with hover / press state; the caller draws it
struct Btn { bool clicked, hovered, held; ImVec2 a, b; };
Btn button(const char* id, ImVec2 size, bool enabled = true);
}  // namespace ui
