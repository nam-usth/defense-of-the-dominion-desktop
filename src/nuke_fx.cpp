#include "nuke_fx.h"

#include <string>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "log.h"
#include "render.h"

namespace {
const float PI_F = 3.14159265f;

// Value noise from integer hashes (no texture), fbm over it, and the fire ramp
const char* COMMON = R"(
float h12(vec2 p) { vec3 q = fract(vec3(p.xyx) * .1031); q += dot(q, q.yzx + 33.33); return fract((q.x + q.y) * q.z); }
float h13(vec3 p) { p = fract(p * .1031); p += dot(p, p.zyx + 31.32); return fract((p.x + p.y) * p.z); }
vec2 h22(vec2 p) { vec3 q = fract(vec3(p.xyx) * vec3(.1031, .1030, .0973)); q += dot(q, q.yzx + 33.33); return fract((q.xx + q.yz) * q.zy); }
float n2(vec2 x) {
  vec2 i = floor(x), f = fract(x); f = f * f * (3. - 2. * f);
  return mix(mix(h12(i), h12(i + vec2(1, 0)), f.x), mix(h12(i + vec2(0, 1)), h12(i + vec2(1, 1)), f.x), f.y);
}
float n3(vec3 x) {
  vec3 i = floor(x), f = fract(x); f = f * f * (3. - 2. * f);
  return mix(mix(mix(h13(i), h13(i + vec3(1, 0, 0)), f.x), mix(h13(i + vec3(0, 1, 0)), h13(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(h13(i + vec3(0, 0, 1)), h13(i + vec3(1, 0, 1)), f.x), mix(h13(i + vec3(0, 1, 1)), h13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm2(vec2 p, int o) { float s = 0., a = .5; for (int i = 0; i < 4; i++) { if (i >= o) break; s += a * n2(p); p = p * 2.03 + vec2(5.1, 1.3); a *= .5; } return s; }
float fbm3(vec3 p, int o) { float s = 0., a = .5; for (int i = 0; i < 4; i++) { if (i >= o) break; s += a * n3(p); p = p * 2.02 + vec3(5.1, 1.3, 7.7); a *= .5; } return s; }
// fire by temperature: soot, deep red, orange, yellow, white-hot; in HDR the hot end burns past 1 (it blooms)
vec3 fireCol(float T, bool hdr) {
  T = clamp(T, 0., 1.15);
  float k = min(T, 1.) * 4.;
  vec3 c = k < 1. ? mix(vec3(.18, .14, .12), vec3(.54, .16, .05), k) : k < 2. ? mix(vec3(.54, .16, .05), vec3(.94, .42, .09), k - 1.)
         : k < 3. ? mix(vec3(.94, .42, .09), vec3(1., .71, .23), k - 2.) : mix(vec3(1., .71, .23), vec3(1., .94, .75), k - 3.);
  return c * (hdr ? exp2(3. * T * T) : 1. + .25 * T);
}
)";

const char* MESH_VS = R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
uniform mat4 uView, uProj, uModel;
out vec3 vW; out vec3 vN; out vec2 vUV;
void main() {
  vec4 w = uModel * vec4(aPos, 1.);
  vW = w.xyz; vN = mat3(uModel) * aNormal; vUV = aUV;
  gl_Position = uProj * uView * w;
}
)";

// The scar: a charred pit and a ragged burn ring with a faint tint beyond; fissures run out
// of the pit (each forking once) and the pit itself is shattered. The cracks open behind the
// shock front, glow white-hot, cool through orange to dull red, then stay as black lines
// until the scar fades. Premultiplied: the burn darkens, the cracks glow (self-lit).
const char* SCAR_FS = R"(
in vec3 vW; out vec4 frag;
uniform vec3 uC; uniform float uAge, uSeed, uFront, uK; uniform bool uHdr;
uniform vec3 uFogCol; uniform float uFogNear, uFogFar;
const float TAU = 6.2831853;
float adiff(float a, float b) { return mod(a - b + 3.14159265, TAU) - 3.14159265; }
float ln1(float x) { float i = floor(x); return mix(h12(vec2(i, uSeed)), h12(vec2(i + 1., uSeed)), fract(x)); }   // linear: a polyline
// a crack's lateral wander (world units) at radius r: straight runs with sharp kinks, and a fine jag
float wander(float r, float i) { return (ln1(r * .9 + i * 7.31) - .5) * 1.1 + (ln1(r * 3.4 + i * 3.17 + 40.) - .5) * .28; }
void main() {
  vec2 d = vW.xz - uC.xz; float r = length(d);
  if (r > 11.5) discard;
  float ang = atan(d.y, d.x);
  float mott = fbm2(vW.xz * .45 + uSeed, 3);
  float pit = 1. - smoothstep(1.4, 3.2 + mott, r);
  float burn = 1. - smoothstep(5.0, 7.5 + 2. * mott, r);
  float tint = 1. - smoothstep(6.5, 11., r);
  float crack = 0.;
  for (int i = 0; i < 14; i++) {
    float fi = float(i);
    float len = 3.5 + 4.5 * h12(vec2(fi + 31., uSeed));
    if (r > len + .3) continue;
    float phi = (fi + .7 * h12(vec2(fi, uSeed))) * TAU / 14.;
    float da = adiff(ang, phi) * r;   // arc distance from the crack's base line
    if (abs(da) > 3.5) continue;
    float w = wander(r, fi);
    float taper = 1. - smoothstep(.5, len, r);
    float wid = .03 + .08 * taper;
    crack = max(crack, (1. - smoothstep(wid * .35, wid, abs(da - w))) * smoothstep(.5, 1.1, r) * sqrt(taper));
    float rb = 1.6 + .45 * len * h12(vec2(fi + 7., uSeed));   // the fork leaves at rb, bending off ~25 deg
    if (r > rb) {
      float side = h12(vec2(fi + 13., uSeed)) < .5 ? -1. : 1.;
      float tb = 1. - smoothstep(rb, rb + len * (.45 + .3 * h12(vec2(fi + 17., uSeed))), r);
      float xb = abs(da - w - side * (r - rb) * .5 - (ln1(r * 3.8 + fi * 5.1 + 90.) - .5) * .22);
      float wb = .025 + .06 * tb;
      crack = max(crack, (1. - smoothstep(wb * .35, wb, xb)) * tb);
    }
  }
  if (r < 4.5) {   // the shattered pit: Voronoi cell edges
    vec2 x = vW.xz * 1.3 + uSeed * 3.; vec2 g = floor(x), f = fract(x); float F1 = 8., F2 = 8.;
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
      vec2 o = vec2(i, j); vec2 dd = o + h22(g + o) - f; float q = dot(dd, dd);
      if (q < F1) { F2 = F1; F1 = q; } else if (q < F2) F2 = q;
    }
    crack = max(crack, (1. - smoothstep(.03, .1, sqrt(F2) - sqrt(F1))) * (1. - smoothstep(2.2, 4.4, r)));
  }
  crack *= (1. - smoothstep(uFront * .9 - .5, uFront * .9, r)) * (1. - smoothstep(5.5, 7.5, r));   // opened by the front, inside the burn
  float E = 1.1 * exp(-uAge / 2.0) + .3 * exp(-uAge / 6.);
  float T = E * (1.05 - .06 * r) * (.85 + .3 * n2(vW.xz * 2. + uAge * .7));
  vec3 glow = fireCol(.18 + .75 * clamp(T, 0., 1.), uHdr) * smoothstep(0., .35, T) * crack;   // lava: white-hot, then orange, then a dull red
  glow += fireCol(.3 + .6 * clamp(E, 0., 1.), uHdr) * .3 * (1. - smoothstep(0., 3., r)) * exp(-uAge / 1.2) * (.6 + .4 * mott);   // the molten pit
  float a = uK * (pit * .45 + burn * (.6 + .3 * mott) + tint * .15);
  a = clamp(a + crack * .5 * uK, 0., .95);
  vec3 ch = mix(vec3(.075, .06, .048), vec3(.022, .018, .015), max(pit, crack));
  ch = mix(ch, uFogCol, smoothstep(uFogNear, uFogFar, length(vW.xz)));
  frag = vec4(ch * a + glow * uK, a);
}
)";

// The shock front: a bright ragged ring racing over the ground, a luminous zone trailing it
const char* FRONT_FS = R"(
in vec3 vW; out vec4 frag;
uniform vec3 uC, uCol; uniform float uR, uW, uI, uTime, uSeed;
void main() {
  vec2 dv = vW.xz - uC.xz; float r = length(dv);
  vec2 dir = dv / max(r, 1e-4);
  // a Mach stem is ragged (periodic noise: sampled around a circle)
  float R = uR * (1. + .03 * (n2(dir * 3. + vec2(uSeed, uTime * 2.)) - .5) + .012 * (n2(dir * 11. + uSeed) - .5));
  float x = (r - R) / uW;
  float clump = .6 + .4 * n2(dir * 9. + vec2(uTime * 1.3, uSeed));
  float trail = .3 * smoothstep(-7., -.5, x) * step(x, 0.) * smoothstep(.5, .8, r / max(uR, 1e-3)) * (.5 + .5 * n2(dir * 14. + vec2(r * .8)));
  float k = uI * (exp(-x * x * 3.) * clump + trail);
  frag = vec4(uCol * k, clamp(.5 * k, 0., .85));   // premultiplied: it whitens what it passes over (additive would tint the grass)
}
)";

// The shock dome: a hemisphere of compressed air, seen at its rim
const char* DOME_FS = R"(
in vec3 vW; in vec3 vN; out vec4 frag;
uniform vec3 uCamPos, uCol; uniform float uA, uTime;
void main() {
  vec3 N = normalize(vN), V = normalize(uCamPos - vW);
  float k = pow(1. - abs(dot(N, V)), 3.) + .04;
  k *= .7 + .3 * n3(vW * .7 + vec3(uTime * 3.));
  frag = vec4(uCol * uA * k, 1.);
}
)";

// The fireball: a noise-displaced ball of boiling fire, white-hot at its thick centre, sooty
// as it cools, burning away (glowing edges) into the mushroom cloud's sprites
const char* FIRE_VS = R"(
layout(location=0) in vec3 aPos;
uniform mat4 uView, uProj; uniform vec3 uC, uS; uniform float uTime, uDisp, uSeed; uniform int uOct;
out vec3 vW; out vec3 vObj; out float vH;
void main() {
  vec3 n = normalize(aPos);
  vec3 q = n * 1.7 + vec3(uSeed, uSeed * .7 - .9 * uTime, 0.);   // the cells boil upward
  float h = fbm3(q + 1.2 * fbm3(q * .6 + vec3(.2 * uTime), 2), uOct);
  vH = h; vObj = n;
  vW = uC + n * (1. + uDisp * (h - .45) * 2.) * uS;
  gl_Position = uProj * uView * vec4(vW, 1.);
}
)";
const char* FIRE_FS = R"(
in vec3 vW; in vec3 vObj; in float vH; out vec4 frag;
uniform vec3 uCamPos, uS, uSunDir, uSunCol, uAmb; uniform float uTime, uHeat, uSoot, uDissolve, uSeed; uniform bool uHdr;
void main() {
  vec3 N = normalize(vObj / uS), V = normalize(uCamPos - vW);
  float fac = max(dot(N, V), 0.);
  float d = fbm3(vObj * 3.1 + vec3(uSeed, -1.4 * uTime, 0.), 3);
  float lanes = smoothstep(0., .3, abs(2. * n3(vObj * 4. + vec3(uSeed, -2.2 * uTime, 0.)) - 1.));   // darker lanes between the boiling cells
  float T = uHeat * (.4 + .6 * pow(fac, 1.5)) * (.55 + .9 * d) * (.85 + .15 * lanes) * (1. - .3 * smoothstep(.1, -.7, vObj.y));   // the thick centre is hottest
  float soot = smoothstep(uSoot, uSoot + .2, .6 * vH + .6 * d + .35 * (1. - fac));   // cooling from the limb in
  vec3 smoke = vec3(.17, .14, .12) * (uAmb + uSunCol * max(dot(N, uSunDir), 0.)) + fireCol(.45 * T, uHdr) * .12;
  vec3 col = mix(fireCol(T, uHdr), smoke, soot);
  float e = .7 * vH + .3 * d - uDissolve;
  if (e < 0.) discard;
  col += fireCol(min(1.1, uHeat + .35), uHdr) * .5 * (1. - smoothstep(0., .05, e)) * (1. - soot);   // burning rims
  frag = vec4(col, uDissolve > 0. ? smoothstep(0., .05, e) : 1.);
}
)";

GLuint program(const char* vs, const char* fs, const char* name) {
  std::string v = std::string("#version 330 core\n") + COMMON + vs, f = std::string("#version 330 core\n") + COMMON + fs;
  return compileProgram(v.c_str(), f.c_str(), name);
}

GLint U(GLuint p, const char* n) {   // cached per (program, name literal)
  static std::unordered_map<GLuint, std::unordered_map<const char*, GLint>> cache;
  auto& m = cache[p];
  auto it = m.find(n);
  return it != m.end() ? it->second : (m[n] = glGetUniformLocation(p, n));
}
void use(GLuint p, const FrameCtx& c, const glm::mat4& model) {
  glUseProgram(p);
  glUniformMatrix4fv(U(p, "uView"), 1, GL_FALSE, glm::value_ptr(c.view));
  glUniformMatrix4fv(U(p, "uProj"), 1, GL_FALSE, glm::value_ptr(c.proj));
  glUniformMatrix4fv(U(p, "uModel"), 1, GL_FALSE, glm::value_ptr(model));
}
void u1(GLuint p, const char* n, float v) { glUniform1f(U(p, n), v); }
void u3(GLuint p, const char* n, const glm::vec3& v) { glUniform3fv(U(p, n), 1, glm::value_ptr(v)); }

}  // namespace

NukeRenderer::FxMesh NukeRenderer::build(const Geometry& g) {
  FxMesh m;
  glGenVertexArrays(1, &m.vao);
  glBindVertexArray(m.vao);
  glGenBuffers(1, &m.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
  glBufferData(GL_ARRAY_BUFFER, g.v.size() * sizeof(Vertex), g.v.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
  if (!g.idx.empty()) {
    glGenBuffers(1, &m.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, g.idx.size() * sizeof(uint32_t), g.idx.data(), GL_STATIC_DRAW);
    m.count = (GLsizei)g.idx.size();
  } else m.count = (GLsizei)g.v.size();
  glBindVertexArray(0);
  return m;
}

static void drawMesh(GLuint vao, GLuint ibo, GLsizei count) {
  glBindVertexArray(vao);
  if (ibo) glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, nullptr);
  else glDrawArrays(GL_TRIANGLES, 0, count);
  glBindVertexArray(0);
}

bool NukeRenderer::init() {
  progScar_ = program(MESH_VS, SCAR_FS, "nuke scar");
  progFront_ = program(MESH_VS, FRONT_FS, "nuke front");
  progDome_ = program(MESH_VS, DOME_FS, "nuke dome");
  progFire_ = program(FIRE_VS, FIRE_FS, "nuke fireball");
  ok_ = progScar_ && progFront_ && progDome_ && progFire_;
  if (!ok_) { logWarn("nuke effect shaders unavailable: the sprite cloud alone shows the blast"); return false; }
  const glm::mat4 flat = glm::rotate(glm::mat4(1), -PI_F / 2, {1, 0, 0});   // XY → XZ, facing up
  quad_ = build(geo::plane(1, 1).transform(flat));
  ring_ = build(geo::ring(0.5f, 1.12f, 192).transform(flat));
  dome_ = build(geo::sphere(1, 48, 24));
  ball_ = build(geo::icosahedron(1, 14));
  return true;
}

void NukeRenderer::drawScar(Renderer& R, glm::vec2 p, float age, float seed, float front) const {
  if (!ok_) return;
  const float k = 1 - nk::ss(nk::SCAR_END - 5, nk::SCAR_END, age);
  if (k <= 0) return;
  const glm::vec3 c(p.x, 0.03f, p.y);
  R.drawCustom(-50, c, [=, this](const FrameCtx& f) {   // before every other transparent: it lies on the ground
    GLuint g = progScar_;
    use(g, f, glm::scale(glm::translate(glm::mat4(1), c), {23, 1, 23}));
    u3(g, "uC", c);
    u1(g, "uAge", age);
    u1(g, "uSeed", seed);
    u1(g, "uFront", front);
    u1(g, "uK", k);
    glUniform1i(U(g, "uHdr"), f.hdr);
    u3(g, "uFogCol", f.fogColor);
    u1(g, "uFogNear", f.fogNear);
    u1(g, "uFogFar", f.fogFar);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    drawMesh(quad_.vao, quad_.ibo, quad_.count);
  });
}

void NukeRenderer::draw(Renderer& R, const Blast& b) const {
  if (!ok_) return;
  using namespace nk;
  const float t = b.t;
  const glm::vec3 c = b.p;

  // fireball: drawn first of the transparent pass (it writes depth: the cloud's sprites behind it hide)
  if (t < FIRE_END) {
    const float fr = fireR(t), g = ss(0.6f, 2.6f, t), sy = 1 - 0.38f * g, sxz = 1 + 0.22f * g;
    // never swallowing the bunker: the ball's horizontal reach stops short of it
    const float db = glm::length(glm::vec2(c.x, c.z)), rx = std::min(fr * sxz, std::max(2.4f, db - 0.5f));
    const glm::vec3 S(rx, fr * sy * glm::clamp(fr * sxz / rx, 1.f, 1.3f), rx), C(c.x, fireY(t), c.z);
    const float heatU = std::min(1.2f, 1.7f * std::exp(-t / 2.8f)), soot = 1.3f - 0.95f * ss(0.4f, 2.8f, t), dis = ss(2.0f, FIRE_END, t);
    const int oct = b.low ? 2 : 4;
    R.drawCustom(-100, C, [=, this](const FrameCtx& f) {
      // the camera inside the ball sees nothing of it (the close-up near ground zero)
      glm::vec3 q = (f.camPos - C) / (S * 1.3f);
      if (glm::dot(q, q) < 1) return;
      GLuint g = progFire_;
      use(g, f, glm::mat4(1));
      u3(g, "uC", C);
      u3(g, "uS", S);
      u1(g, "uTime", t);
      u1(g, "uDisp", 0.22f);
      u1(g, "uSeed", b.seed);
      glUniform1i(U(g, "uOct"), oct);
      u3(g, "uCamPos", f.camPos);
      u3(g, "uSunDir", f.sunDir);
      u3(g, "uSunCol", f.sunCol);
      u3(g, "uAmb", f.ambient);
      u1(g, "uHeat", heatU);
      u1(g, "uSoot", soot);
      u1(g, "uDissolve", dis);
      glUniform1i(U(g, "uHdr"), f.hdr);
      glDisable(GL_BLEND);
      glDepthMask(GL_TRUE);
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
      if (f.msaa && dis > 0) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
      drawMesh(ball_.vao, ball_.ibo, ball_.count);
      glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    });
  }

  // shock front ring on the ground
  if (t < FRONT_END) {
    const float R0 = frontR(t), w = 0.35f + 0.035f * R0;
    const float I = (6 * std::exp(-t / 0.3f) + 0.9f) * (1 - ss(1.2f, FRONT_END, t));
    R.drawCustom(1, c, [=, this](const FrameCtx& f) {
      GLuint g = progFront_;
      const float sc = std::max(0.05f, R0 + 2 * w) / 1.12f;   // the mesh (r 0.5-1.12) out past the ring's glow
      use(g, f, glm::scale(glm::translate(glm::mat4(1), c + glm::vec3(0, 0.12f, 0)), {sc, 1, sc}));
      u3(g, "uC", c);
      u3(g, "uCol", glm::vec3(1, 0.86f, 0.62f));
      u1(g, "uR", R0);
      u1(g, "uW", w);
      u1(g, "uI", f.hdr ? I : std::min(I, 1.2f));
      u1(g, "uTime", t);
      u1(g, "uSeed", b.seed);
      glDisable(GL_CULL_FACE);
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      glDepthMask(GL_FALSE);
      drawMesh(ring_.vao, ring_.ibo, ring_.count);
    });
  }

  // shock dome: the hemisphere of compressed air over the front, gone by 0.8 s
  if (t < 0.8f) {
    const float R0 = frontR(t), A = 2.2f * ss(0, 0.04f, t) * (1 - ss(0.2f, 0.8f, t));
    R.drawCustom(3, c, [=, this](const FrameCtx& f) {
      GLuint g = progDome_;
      use(g, f, glm::scale(glm::translate(glm::mat4(1), c), glm::vec3(std::max(0.05f, R0))));
      u3(g, "uCamPos", f.camPos);
      u3(g, "uCol", glm::vec3(1, 0.95f, 0.88f));
      u1(g, "uA", f.hdr ? A : std::min(A, 0.8f));
      u1(g, "uTime", t);
      glEnable(GL_CULL_FACE);
      glCullFace(GL_BACK);
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE);
      glDepthMask(GL_FALSE);
      drawMesh(dome_.vao, dome_.ibo, dome_.count);
    });
  }

}
