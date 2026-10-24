#include "weather.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "log.h"
#include "render.h"

namespace {
float uni(std::mt19937& rng, float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

// The rain's box: the field and the air over it up to 12, well under the overview camera (y 22):
// drops passing just under the lens smear across the whole view and all point at its centre
constexpr float BOX_HALF = 26, BOX_TOP = 12;
constexpr int DROPS = 24000, SPLASHES = 4000;   // at full rain (fewer as it eases; many are out of view)

// One program for both: each drop is two triangles (six vertices) placed from its index.
// Streaks fall (each at its own pace), wrap from the ground back to the top and drift with the
// wind; a splash is a ring on the ground for 0.18 s, somewhere new each time
const char* RAIN_VS = R"(#version 330 core
uniform mat4 uView, uProj;
uniform vec3 uCamPos;
uniform float uTime, uFall, uHalf, uTop, uSpeed, uLen, uWidth, uAlpha, uPx, uCount;   // uFall: how far the drops have fallen
uniform vec2 uWind, uBlown;                                                        // uBlown: how far the wind has carried them
uniform int uKind;   // 0 falling streaks · 1 splashes on the ground
uniform vec3 uLight, uLampPos, uLampCol; uniform float uLampR;
uniform float uFogNear, uFogFar;
out vec2 vQ; out float vA; out vec3 vCol;
vec4 h41(float n) {
  vec4 p = fract(vec4(n) * vec4(.1031, .1030, .0973, .1099));
  p += dot(p, p.wzxy + 33.33);
  return fract((p.xxyz + p.yzzw) * p.zywx);
}
void main() {
  int id = gl_VertexID / 6, k = gl_VertexID - id * 6;
  vec2 c = vec2(k == 1 || k == 2 || k == 4 ? 1.0 : 0.0, k == 2 || k == 4 || k == 5 ? 1.0 : 0.0);
  vec4 h = h41(float(id) + 0.5);
  vec3 P;
  float a = clamp(uCount - float(id), 0.0, 1.0);   // the last few fade as the rain eases
  if (uKind == 0) {
    float pace = 0.85 + 0.3 * h.w;
    float y = uTop - mod(uFall * pace + h.z * uTop, uTop);
    vec2 xz = mod(h.xy * 2.0 * uHalf + uBlown, 2.0 * uHalf) - uHalf;
    vec3 dir = normalize(vec3(uWind.x, -uSpeed * pace, uWind.y));
    P = vec3(xz.x, y, xz.y) - dir * (uLen * c.y);   // c.y: head (0) to tail (1)
    vec3 toCam = uCamPos - P;
    float dist = length(toCam), w = max(uWidth, dist * uPx);   // never thinner than a pixel: fainter instead (no shimmer far off)
    P += normalize(cross(dir, toCam)) * ((c.x - 0.5) * w);
    a *= uAlpha * uWidth / w * smoothstep(3.0, 8.0, dist);    // none close to the lens (the low close-up stands in the rain)
    vQ = vec2(c.x * 2.0 - 1.0, c.y);
  } else {
    float period = 0.5 + 0.6 * h.w, cyc = uTime / period + h.z * 17.0, life = fract(cyc) * period / 0.18;
    vec4 g = h41(float(id) * 7.13 + floor(cyc) * 131.7);
    float r = life < 1.0 ? 0.03 + 0.1 * sqrt(life) : 0.0;   // (spent: a point, nothing drawn)
    P = vec3((g.x * 2.0 - 1.0) * uHalf + (c.x * 2.0 - 1.0) * r, 0.025, (g.y * 2.0 - 1.0) * uHalf + (c.y * 2.0 - 1.0) * r);
    a *= 0.35 * (1.0 - life) * (1.0 - life);
    vQ = c * 2.0 - 1.0;
  }
  a *= 1.0 - smoothstep(0.75 * uHalf, uHalf, max(abs(P.x), abs(P.z)));   // no edge to the box (the low close-up sees it)
  a *= 1.0 - smoothstep(uFogNear, uFogFar, length(P.xz));                 // into the night's dark
  float l = clamp(1.0 - length(P - uLampPos) / uLampR, 0.0, 1.0);         // where the bunker's light catches them
  vCol = uLight + uLampCol * l * l;
  vA = a;
  gl_Position = uProj * uView * vec4(P, 1.0);
}
)";
const char* RAIN_FS = R"(#version 330 core
in vec2 vQ; in float vA; in vec3 vCol;
uniform int uKind;
out vec4 frag;
void main() {
  float a;
  if (uKind == 0) a = vA * (1.0 - vQ.x * vQ.x) * (1.0 - 0.75 * vQ.y);   // soft across, fading toward the tail
  else {
    float d = length(vQ);
    a = vA * smoothstep(0.45, 0.8, d) * (1.0 - smoothstep(0.8, 1.0, d));   // a thin ring
  }
  if (a < 0.003) discard;
  frag = vec4(vCol, a);
}
)";
}  // namespace

// ─── The sky's state ─────────────────────────────────────────────────────────
void Weather::reset(std::mt19937& rng) {
  *this = Weather();
  hold = uni(rng, 45, 100);
  const float a = uni(rng, 0, 6.2832f);
  wind = glm::vec2(std::cos(a), std::sin(a)) * 1.2f;
}

void Weather::pour(bool hard, std::mt19937& rng) {
  showerMax = hard ? uni(rng, 0.7f, 1) : uni(rng, 0.15f, 0.35f);
  spell = uni(rng, 15, 30);
}

void Weather::set(Sky s, std::mt19937& rng, bool instant) {
  sky = s;
  afterRain = false;
  if (s == Sky::Rain) {   // a shower: a drizzle or a downpour (even odds), its wind (harder in a downpour), how long it lasts
    pour(uni(rng, 0, 1) < 0.5f, rng);
    const float a = uni(rng, 0, 6.2832f);
    wind = glm::vec2(std::cos(a), std::sin(a)) * (heavy() ? uni(rng, 1.2f, 3.2f) : uni(rng, 0.3f, 1.2f));
    hold = uni(rng, 40, 90);
  } else hold = s == Sky::Clear ? uni(rng, 80, 170) : uni(rng, 25, 45);
  if (instant) {
    cloud = s == Sky::Clear ? 0.f : 1.f;
    rain = s == Sky::Rain ? showerMax : 0.f;
  }
}

// Clear (80-170 s) → overcast (25-45 s) → a shower about half the time (40-90 s), else clear
// again; after a shower it stays overcast a while and mostly clears (rains again a quarter of
// the time). Rain about a fifth of the time, in spells of a minute or so. A shower is a drizzle
// or a downpour, and every 15-30 s it may turn from one to the other (two times in five)
void Weather::update(float dt, std::mt19937& rng, bool on) {
  t += dt;
  if (on && !held && sky == Sky::Rain && (spell -= dt) <= 0) {
    if (uni(rng, 0, 1) < 0.4f) pour(!heavy(), rng);
    else spell = uni(rng, 15, 30);
  }
  if (on && !held && (hold -= dt) <= 0) {
    switch (sky) {
      case Sky::Clear:
        sky = Sky::Overcast;
        afterRain = false;
        hold = uni(rng, 25, 45);
        break;
      case Sky::Overcast:
        if (uni(rng, 0, 1) < (afterRain ? 0.25f : 0.5f)) set(Sky::Rain, rng, false);
        else set(Sky::Clear, rng, false);
        break;
      case Sky::Rain:
        sky = Sky::Overcast;
        afterRain = true;
        hold = uni(rng, 20, 40);
        break;
    }
  }
  // the sky: the clouds roll in (and out) over ~18 s; the rain starts once they are in, comes up
  // over ~6 s (a drizzle swells into a downpour as fast) and stops over ~4 s, wavering a little
  // as it falls
  const float cloudT = on && sky != Sky::Clear ? 1.f : 0.f;
  cloud += std::clamp(cloudT - cloud, -dt / 18, dt / 18);
  const float waver = 0.88f + 0.12f * std::sin(0.31f * (float)t) * std::sin(0.13f * (float)t + 1),
              rainT = on && sky == Sky::Rain && cloud > 0.75f ? showerMax * waver : 0.f;
  rain += std::clamp(rainT - rain, -dt / 4, dt / 6);
  drift += wind * (1.5f * dt);
  blown += wind * dt;
  fall += fallSpeed() * dt;
  // the ground: soaked after ~12 s of a downpour, the puddles full after ~45 s more; once the rain
  // stops it dries in about a minute under the sun (two under clouds), the puddles seep away slower.
  // Switched off, four times as fast
  const float off = on ? 1.f : 4.f;
  if (rain > 0.02f) {
    wet = std::min(1.f, wet + dt * rain / 12);
    puddle = std::min(1.f, puddle + dt * rain * wet / 45);
  } else {
    wet = std::max(0.f, wet - dt * off / (50 + 70 * cloud));
    puddle = std::max(0.f, puddle - dt * off / (100 + 100 * cloud));
  }
}

const char* Weather::name() const {
  return sky == Sky::Rain ? (heavy() ? "DOWNPOUR" : "DRIZZLE") : sky == Sky::Overcast ? "OVERCAST" : "CLEAR";
}

// ─── The rain ────────────────────────────────────────────────────────────────
static GLint U(GLuint p, const char* n) {   // cached per name literal (one program)
  static std::unordered_map<const char*, GLint> cache;
  auto it = cache.find(n);
  return it != cache.end() ? it->second : (cache[n] = glGetUniformLocation(p, n));
}

bool WeatherRenderer::init() {
  prog_ = compileProgram(RAIN_VS, RAIN_FS, "rain");
  if (!prog_) { logWarn("rain shader unavailable: no rain drawn (the ground still wets)"); return false; }
  glGenVertexArrays(1, &vao_);   // no attributes: the vertex shader places every drop from gl_VertexID
  return true;
}

void WeatherRenderer::draw(Renderer& R, const Weather& w, const Look& look) const {
  if (!prog_ || w.rain < 0.005f) return;
  const float t = (float)std::fmod(w.t, 2048.0), rain = w.rain, fall = (float)w.fall, speed = w.fallSpeed();
  const glm::vec2 wind = w.wind, blown = glm::mod(w.blown, glm::vec2(2 * BOX_HALF));   // (the box repeats: no jump)
  // a drizzle's drops: fine, short and faint; a downpour's: long streaks, thicker, brighter
  const float len = 0.2f + 0.35f * rain, width = 0.01f + 0.008f * rain, alpha = 0.4f + 0.25f * rain;
  auto pass = [=, this](const FrameCtx& f, int kind, int max) {
    const float count = max * rain;
    const GLuint p = prog_;
    glUseProgram(p);
    glUniformMatrix4fv(U(p, "uView"), 1, GL_FALSE, &f.view[0][0]);
    glUniformMatrix4fv(U(p, "uProj"), 1, GL_FALSE, &f.proj[0][0]);
    glUniform3fv(U(p, "uCamPos"), 1, &f.camPos[0]);
    glUniform1f(U(p, "uTime"), t);
    glUniform1f(U(p, "uFall"), fall);
    glUniform1f(U(p, "uHalf"), BOX_HALF);
    glUniform1f(U(p, "uTop"), BOX_TOP);
    glUniform1f(U(p, "uSpeed"), speed);
    glUniform1f(U(p, "uLen"), len);
    glUniform1f(U(p, "uWidth"), width);
    glUniform1f(U(p, "uAlpha"), alpha);
    glUniform1f(U(p, "uPx"), 2.f / (f.proj[1][1] * std::max(1, f.vpH)));   // world size of a pixel, per unit of distance
    glUniform1f(U(p, "uCount"), count);
    glUniform2fv(U(p, "uWind"), 1, &wind[0]);
    glUniform2fv(U(p, "uBlown"), 1, &blown[0]);
    glUniform1i(U(p, "uKind"), kind);
    glUniform3fv(U(p, "uLight"), 1, &look.light[0]);
    glUniform3fv(U(p, "uLampPos"), 1, &look.lampPos[0]);
    glUniform3fv(U(p, "uLampCol"), 1, &look.lampCol[0]);
    glUniform1f(U(p, "uLampR"), look.lampR);
    glUniform1f(U(p, "uFogNear"), f.fogNear);
    glUniform1f(U(p, "uFogFar"), f.fogFar);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6 * (int)std::ceil(count));
    glBindVertexArray(0);
  };
  // the splashes lie on the ground: before the other see-through things (the nuke's scar is at -50);
  // the streaks hang through the whole view: after them (sorting one box of rain against them gains nothing)
  R.drawCustom(-40, glm::vec3(0), [=](const FrameCtx& f) { pass(f, 1, SPLASHES); });
  R.drawCustom(5, glm::vec3(0), [=](const FrameCtx& f) { pass(f, 0, DROPS); });
}
