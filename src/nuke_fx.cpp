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

// The scar: a small scorched pit of churned earth. Short cracks run out of it (each forking
// once) and its floor is shattered; they open behind the shock front and glow a deep
// red-orange through the soil, dimming over ~8 s, then the pit fades. Premultiplied: the
// earth darkens the grass, the cracks glow (self-lit).
const char* SCAR_FS = R"(
in vec3 vW; out vec4 frag;
uniform vec3 uC; uniform float uAge, uSeed, uFront, uK; uniform bool uHdr;
uniform vec3 uFogCol; uniform float uFogNear, uFogFar;
const float TAU = 6.2831853;
float adiff(float a, float b) { return mod(a - b + 3.14159265, TAU) - 3.14159265; }
float ln1(float x) { float i = floor(x); return mix(h12(vec2(i, uSeed)), h12(vec2(i + 1., uSeed)), fract(x)); }   // linear: a polyline
// a crack's lateral wander (world units) at radius r: straight runs with sharp kinks, and a fine jag
float wander(float r, float i) { return (ln1(r * 2.4 + i * 7.31) - .5) * .4 + (ln1(r * 7.5 + i * 3.17 + 40.) - .5) * .11; }
void main() {
  vec2 d = vW.xz - uC.xz; float r = length(d);
  if (r > 4.) discard;
  float ang = atan(d.y, d.x);
  float mott = fbm2(vW.xz * .8 + uSeed, 3);
  float pit = 1. - smoothstep(.5, 1.2 + .4 * mott, r);       // the churned centre
  float soil = 1. - smoothstep(1.6, 2.4 + .8 * mott, r);      // scorched earth
  float tint = 1. - smoothstep(2.3, 3.8, r);                  // a faint singe on the grass
  float crack = 0.;
  for (int i = 0; i < 10; i++) {
    float fi = float(i);
    float len = 1.3 + 1.2 * h12(vec2(fi + 31., uSeed));
    if (r > len + .2) continue;
    float phi = (fi + .7 * h12(vec2(fi, uSeed))) * TAU / 10.;
    float da = adiff(ang, phi) * r;   // arc distance from the crack's base line
    if (abs(da) > 1.4) continue;
    float w = wander(r, fi);
    float taper = 1. - smoothstep(.2, len, r);
    float wid = .025 + .045 * taper;
    crack = max(crack, (1. - smoothstep(wid * .35, wid, abs(da - w))) * smoothstep(.2, .45, r) * sqrt(taper));
    float rb = .6 + .4 * len * h12(vec2(fi + 7., uSeed));   // the fork leaves at rb, bending off ~25 deg
    if (r > rb) {
      float side = h12(vec2(fi + 13., uSeed)) < .5 ? -1. : 1.;
      float tb = 1. - smoothstep(rb, rb + len * (.35 + .25 * h12(vec2(fi + 17., uSeed))), r);
      float xb = abs(da - w - side * (r - rb) * .5 - (ln1(r * 6. + fi * 5.1 + 90.) - .5) * .12);
      float wb = .018 + .035 * tb;
      crack = max(crack, (1. - smoothstep(wb * .35, wb, xb)) * tb);
    }
  }
  if (r < 1.8) {   // the shattered floor: Voronoi cell edges
    vec2 x = vW.xz * 3.2 + uSeed * 3.; vec2 g = floor(x), f = fract(x); float F1 = 8., F2 = 8.;
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
      vec2 o = vec2(i, j); vec2 dd = o + h22(g + o) - f; float q = dot(dd, dd);
      if (q < F1) { F2 = F1; F1 = q; } else if (q < F2) F2 = q;
    }
    crack = max(crack, (1. - smoothstep(.03, .09, sqrt(F2) - sqrt(F1))) * (1. - smoothstep(.8, 1.7, r)));
  }
  crack *= 1. - smoothstep(uFront * .9 - .3, uFront * .9, r);   // opened by the front
  // a deep red-orange glow, brightest in the pit, flickering a little, dimming over ~8 s
  float E = .85 * exp(-uAge / 3.) + .15 * exp(-uAge / 8.);
  float T = E * (1. - .12 * r) * (.75 + .5 * n2(vW.xz * 3. + uAge * .6));
  float Tc = clamp(T, 0., 1.);
  vec3 hot = mix(vec3(.54, .1, .03), vec3(1., .23, .06), Tc) * (uHdr ? 1. + 1.2 * Tc : 1.), earth = vec3(.3, .19, .11);   // dull red .. glowing red
  float under = mix(.35, 1., smoothstep(1., 3., uAge));   // dim while the fireball sits on it (no veins through the fire)
  vec3 glow = mix(earth * .5, hot, .65) * crack * smoothstep(0., .25, T) * under * (uHdr ? 1.5 : 1.);   // red through the soil
  glow += hot * .22 * pit * E * under * (.5 + .5 * mott);   // the pit floor smoulders
  float a = uK * (pit * .35 + soil * (.45 + .25 * mott) + tint * .12);
  a = clamp(a + crack * .35 * uK, 0., .9);
  vec3 ch = mix(vec3(.16, .11, .07), vec3(.07, .05, .035), max(pit, crack));   // churned brown earth, darker in the pit and cracks
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
  ok_ = progScar_ && progFront_ && progDome_;
  if (!ok_) { logWarn("nuke effect shaders unavailable: the sprite cloud alone shows the blast"); return false; }
  const glm::mat4 flat = glm::rotate(glm::mat4(1), -PI_F / 2, {1, 0, 0});   // XY → XZ, facing up
  quad_ = build(geo::plane(1, 1).transform(flat));
  ring_ = build(geo::ring(0.5f, 1.12f, 192).transform(flat));
  dome_ = build(geo::sphere(1, 48, 24));
  return true;
}

void NukeRenderer::drawScar(Renderer& R, glm::vec2 p, float age, float seed, float front) const {
  if (!ok_) return;
  const float k = 1 - nk::ss(nk::SCAR_END - 5, nk::SCAR_END, age);
  if (k <= 0) return;
  const glm::vec3 c(p.x, 0.03f, p.y);
  R.drawCustom(-50, c, [=, this](const FrameCtx& f) {   // before every other transparent: it lies on the ground
    GLuint g = progScar_;
    use(g, f, glm::scale(glm::translate(glm::mat4(1), c), {8.5f, 1, 8.5f}));
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

  // shock front ring on the ground
  if (t < FRONT_END) {
    const float R0 = frontR(t), w = 0.45f + 0.04f * R0;   // a soft band of light, not a drawn line
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
