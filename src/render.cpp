#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "gpu_particles.h"
#include "log.h"
#include "stb_image_write.h"

// ─── Geometry ────────────────────────────────────────────────────────────────
Geometry& Geometry::transform(const glm::mat4& m) {
  glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
  for (Vertex& x : v) {
    x.p = glm::vec3(m * glm::vec4(x.p, 1));
    glm::vec3 n = nm * x.n;
    float l = glm::length(n);
    x.n = l > 0 ? n / l : glm::vec3(0, 1, 0);
  }
  return *this;
}
Geometry Geometry::nonIndexed() const {
  if (idx.empty()) return *this;
  Geometry g;
  g.v.reserve(idx.size());
  for (uint32_t i : idx) g.v.push_back(v[i]);
  return g;
}
void Geometry::computeFlatNormals() {
  for (size_t i = 0; i + 2 < v.size(); i += 3) {
    glm::vec3 n = glm::cross(v[i + 1].p - v[i].p, v[i + 2].p - v[i].p);
    float l = glm::length(n);
    n = l > 0 ? n / l : glm::vec3(0, 1, 0);
    v[i].n = v[i + 1].n = v[i + 2].n = n;
  }
}
void Geometry::append(const Geometry& g) {
  if (!idx.empty() || !g.idx.empty()) {
    Geometry a = nonIndexed(), b = g.nonIndexed();
    a.v.insert(a.v.end(), b.v.begin(), b.v.end());
    *this = std::move(a);
    return;
  }
  v.insert(v.end(), g.v.begin(), g.v.end());
}
bool Geometry::bounds(glm::vec3& mn, glm::vec3& mx) const {
  if (v.empty()) return false;
  mn = mx = v[0].p;
  for (const Vertex& x : v) { mn = glm::min(mn, x.p); mx = glm::max(mx, x.p); }
  return true;
}

namespace geo {
static const float PI = 3.14159265358979f;

Geometry box(float w, float h, float d) {
  Geometry g;
  const glm::vec3 half(w / 2, h / 2, d / 2);
  // normal, u axis, v axis (u x v = n)
  const glm::vec3 faces[6][3] = {
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
  };
  for (auto& f : faces) {
    glm::vec3 n = f[0], u = f[1], vv = f[2];
    glm::vec3 c = n * half, hu = u * half, hv = vv * half;
    uint32_t b = (uint32_t)g.v.size();
    g.v.push_back({c - hu - hv, n, {0, 0}});
    g.v.push_back({c + hu - hv, n, {1, 0}});
    g.v.push_back({c + hu + hv, n, {1, 1}});
    g.v.push_back({c - hu + hv, n, {0, 1}});
    g.idx.insert(g.idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
  }
  return g;
}

Geometry sphere(float r, int ws, int hs) {
  Geometry g;
  std::vector<std::vector<uint32_t>> grid;
  for (int iy = 0; iy <= hs; iy++) {
    std::vector<uint32_t> row;
    float vv = (float)iy / hs;
    for (int ix = 0; ix <= ws; ix++) {
      float u = (float)ix / ws;
      glm::vec3 p(-r * std::cos(u * 2 * PI) * std::sin(vv * PI), r * std::cos(vv * PI), r * std::sin(u * 2 * PI) * std::sin(vv * PI));
      glm::vec3 n = glm::length(p) > 0 ? glm::normalize(p) : glm::vec3(0, 1, 0);
      row.push_back((uint32_t)g.v.size());
      g.v.push_back({p, n, {u, 1 - vv}});
    }
    grid.push_back(row);
  }
  for (int iy = 0; iy < hs; iy++)
    for (int ix = 0; ix < ws; ix++) {
      uint32_t a = grid[iy][ix + 1], b = grid[iy][ix], c = grid[iy + 1][ix], d = grid[iy + 1][ix + 1];
      if (iy != 0) g.idx.insert(g.idx.end(), {a, b, d});
      if (iy != hs - 1) g.idx.insert(g.idx.end(), {b, c, d});
    }
  return g;
}

Geometry cylinder(float rTop, float rBottom, float h, int radial) {
  Geometry g;
  float hh = h / 2, slope = (rBottom - rTop) / h;
  std::vector<std::vector<uint32_t>> rows;
  for (int y = 0; y <= 1; y++) {
    std::vector<uint32_t> row;
    float radius = y * (rBottom - rTop) + rTop;
    for (int x = 0; x <= radial; x++) {
      float u = (float)x / radial, th = u * 2 * PI, s = std::sin(th), c = std::cos(th);
      row.push_back((uint32_t)g.v.size());
      g.v.push_back({{radius * s, -y * h + hh, radius * c}, glm::normalize(glm::vec3(s, slope, c)), {u, 1.f - y}});
    }
    rows.push_back(row);
  }
  for (int x = 0; x < radial; x++) {
    uint32_t a = rows[0][x], b = rows[1][x], c = rows[1][x + 1], d = rows[0][x + 1];
    g.idx.insert(g.idx.end(), {a, b, d, b, c, d});
  }
  for (int top = 1; top >= 0; top--) {
    float radius = top ? rTop : rBottom, sign = top ? 1.f : -1.f;
    if (radius <= 0) continue;
    uint32_t cs = (uint32_t)g.v.size();
    for (int x = 1; x <= radial; x++) g.v.push_back({{0, hh * sign, 0}, {0, sign, 0}, {.5f, .5f}});
    uint32_t ce = (uint32_t)g.v.size();
    for (int x = 0; x <= radial; x++) {
      float th = (float)x / radial * 2 * PI;
      g.v.push_back({{radius * std::sin(th), hh * sign, radius * std::cos(th)}, {0, sign, 0}, {0, 0}});
    }
    for (int x = 0; x < radial; x++) {
      uint32_t c = cs + x, i = ce + x;
      if (top) g.idx.insert(g.idx.end(), {i, i + 1, c});
      else g.idx.insert(g.idx.end(), {i + 1, i, c});
    }
  }
  return g;
}

Geometry cone(float r, float h, int radial) { return cylinder(0, r, h, radial); }

Geometry ring(float inner, float outer, int seg) {
  Geometry g;
  for (int j = 0; j <= 1; j++) {
    float radius = inner + j * (outer - inner);
    for (int i = 0; i <= seg; i++) {
      float a = (float)i / seg * 2 * PI;
      g.v.push_back({{radius * std::cos(a), radius * std::sin(a), 0}, {0, 0, 1}, {0, 0}});
    }
  }
  for (int i = 0; i < seg; i++) {
    uint32_t a = i, b = i + seg + 1, c = i + seg + 2, d = i + 1;
    g.idx.insert(g.idx.end(), {a, b, d, b, c, d});
  }
  return g;
}

Geometry plane(float w, float h) {
  Geometry g;
  g.v = {{{-w / 2, h / 2, 0}, {0, 0, 1}, {0, 1}}, {{w / 2, h / 2, 0}, {0, 0, 1}, {1, 1}},
         {{-w / 2, -h / 2, 0}, {0, 0, 1}, {0, 0}}, {{w / 2, -h / 2, 0}, {0, 0, 1}, {1, 0}}};
  g.idx = {0, 2, 1, 2, 3, 1};
  return g;
}

// Polyhedron on a sphere of radius r: flat (detail 0) or smooth
static Geometry polyhedron(const std::vector<glm::vec3>& verts, const std::vector<int>& ids, float r, int detail, bool smooth) {
  std::vector<glm::vec3> tris;
  for (size_t i = 0; i + 2 < ids.size(); i += 3) {
    glm::vec3 a = verts[ids[i]], b = verts[ids[i + 1]], c = verts[ids[i + 2]];
    int n = detail + 1;
    // subdivide: (n+1)^2 small triangles on a barycentric grid
    auto at = [&](int i2, int j2) { return a + (b - a) * ((float)i2 / n) + (c - a) * ((float)j2 / n); };
    for (int i2 = 0; i2 < n; i2++)
      for (int j2 = 0; j2 < n - i2; j2++) {
        tris.insert(tris.end(), {at(i2, j2), at(i2 + 1, j2), at(i2, j2 + 1)});
        if (j2 < n - i2 - 1) tris.insert(tris.end(), {at(i2 + 1, j2), at(i2 + 1, j2 + 1), at(i2, j2 + 1)});
      }
  }
  Geometry g;
  for (size_t i = 0; i + 2 < tris.size(); i += 3) {
    glm::vec3 p[3];
    for (int k = 0; k < 3; k++) p[k] = glm::normalize(tris[i + k]) * r;
    glm::vec3 fn = glm::normalize(glm::cross(p[1] - p[0], p[2] - p[0]));
    if (glm::dot(fn, p[0] + p[1] + p[2]) < 0) { std::swap(p[1], p[2]); fn = -fn; }   // outward winding
    for (int k = 0; k < 3; k++) g.v.push_back({p[k], smooth ? glm::normalize(p[k]) : fn, {0, 0}});
  }
  return g;
}

Geometry dodecahedron(float r) {
  const float t = (1 + std::sqrt(5.f)) / 2, q = 1 / t;
  std::vector<glm::vec3> v = {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, -1}, {-1, 1, 1}, {1, -1, -1}, {1, -1, 1}, {1, 1, -1},
                              {1, 1, 1},    {0, -q, -t}, {0, -q, t},  {0, q, -t},  {0, q, t},   {-q, -t, 0}, {-q, t, 0},
                              {q, -t, 0},   {q, t, 0},   {-t, 0, -q}, {t, 0, -q},  {-t, 0, q},  {t, 0, q}};
  std::vector<int> i = {3,  11, 7,  3,  7,  15, 3,  15, 13, 7,  19, 17, 7,  17, 6,  7,  6,  15, 17, 4,
                        8,  17, 8,  10, 17, 10, 6,  8,  0,  16, 8,  16, 2,  8,  2,  10, 0,  12, 1,  0,
                        1,  18, 0,  18, 16, 6,  10, 2,  6,  2,  13, 6,  13, 15, 2,  16, 18, 2,  18, 3,
                        2,  3,  13, 18, 1,  9,  18, 9,  11, 18, 11, 3,  4,  14, 12, 4,  12, 0,  4,  0,
                        8,  11, 9,  5,  11, 5,  19, 11, 19, 7,  19, 5,  14, 19, 14, 4,  19, 4,  17, 1,
                        12, 14, 1,  14, 5,  1,  5,  9};
  return polyhedron(v, i, r, 0, false);
}

Geometry icosahedron(float r, int detail) {
  const float t = (1 + std::sqrt(5.f)) / 2;
  std::vector<glm::vec3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                              {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  std::vector<int> i = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6,
                        7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6,
                        7, 9, 8, 1};
  return polyhedron(v, i, r, detail, detail > 0);
}
}  // namespace geo

// ─── Mesh ────────────────────────────────────────────────────────────────────
static std::vector<std::unique_ptr<Mesh>> g_meshes;
static std::vector<std::unique_ptr<Texture>> g_genTextures;   // Texture::create

const Texture* Texture::create(int w, int h, const uint8_t* rgba) {
  auto t = std::make_unique<Texture>();
  glGenTextures(1, &t->id);
  glBindTexture(GL_TEXTURE_2D, t->id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  t->w = w;
  t->h = h;
  g_genTextures.push_back(std::move(t));
  return g_genTextures.back().get();
}

Mesh* Mesh::create(const Geometry& g) {
  auto m = std::make_unique<Mesh>();
  g.bounds(m->bmin, m->bmax);
  glGenVertexArrays(1, &m->vao);
  glBindVertexArray(m->vao);
  glGenBuffers(1, &m->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
  glBufferData(GL_ARRAY_BUFFER, g.v.size() * sizeof(Vertex), g.v.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, p));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, n));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
  if (!g.idx.empty()) {
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, g.idx.size() * sizeof(uint32_t), g.idx.data(), GL_STATIC_DRAW);
    m->indexed = true;
    m->count = (GLsizei)g.idx.size();
  } else {
    m->count = (GLsizei)g.v.size();
  }
  // instance attributes 3..7 point into the renderer's instance buffer (set per draw)
  for (int i = 3; i <= 7; i++) { glEnableVertexAttribArray(i); glVertexAttribDivisor(i, 1); }
  glBindVertexArray(0);
  g_meshes.push_back(std::move(m));
  return g_meshes.back().get();
}

// ─── Particles ───────────────────────────────────────────────────────────────
void ParticleSystem::emit(float x, float y, float z, float vx, float vy, float vz, float life, float s0, float s1,
                          float a0, uint32_t c0, uint32_t c1, float grav, float drag, int fade) {
  if ((int)p.size() >= cap) return;
  P q;
  q.pos = {x, y, z};
  q.vel = {vx, vy, vz};
  q.life = q.maxLife = life;
  q.s0 = s0; q.s1 = s1; q.a0 = a0; q.grav = grav; q.drag = drag; q.fade = (uint8_t)fade;
  q.c0 = hexColor(c0); q.c1 = hexColor(c1);
  q.col = q.c0; q.size = s0; q.alpha = 0;   // set by the next update
  p.push_back(q);
}
// Serial on purpose: at the cap (3800 particles) this takes a few µs, less than
// waking worker threads costs (measured: a parallel version made the frame slower)
void ParticleSystem::update(float dt) {
  size_t i = 0;
  while (i < p.size()) {
    P& q = p[i];
    q.life -= dt;
    if (q.life <= 0) { q = p.back(); p.pop_back(); continue; }   // swap-remove keeps the buffer packed
    float t = 1 - q.life / q.maxLife, k = std::max(0.f, 1 - q.drag * dt);
    q.vel.x *= k; q.vel.z *= k; q.vel.y = q.vel.y * k - q.grav * dt;
    q.pos += q.vel * dt;
    q.size = q.s0 + (q.s1 - q.s0) * t;
    q.alpha = q.a0 * (q.fade ? std::sin(3.14159265f * std::min(1.f, t * 1.15f)) : 1 - t);
    q.col = q.c0 + (q.c1 - q.c0) * t;
    i++;
  }
}

// ─── Shaders ─────────────────────────────────────────────────────────────────
static const char* LIT_VS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in mat4 iModel;
layout(location=7) in vec3 iColor;
uniform mat4 uView, uProj, uModel, uLightVP;
uniform float uNormalBias;
out vec3 vWorld; out vec3 vNormal; out vec2 vUV; out vec3 vColor; out vec4 vShadowPos;
void main() {
  mat4 M = uModel * iModel;
  vec4 wp = M * vec4(aPos, 1.0);
  vWorld = wp.xyz;
  vNormal = mat3(M) * aNormal;
  vUV = vec2(aUV.x, 1.0 - aUV.y);   // DDS rows are top-down: sample v' = 1 - v (as the web version does)
  vColor = iColor;
  vec3 n = length(vNormal) > 0.0 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
  vShadowPos = uLightVP * vec4(wp.xyz + n * uNormalBias, 1.0);
  gl_Position = uProj * uView * wp;
}
)";

static const char* LIT_FS = R"(#version 330 core
in vec3 vWorld; in vec3 vNormal; in vec2 vUV; in vec3 vColor; in vec4 vShadowPos;
out vec4 fragColor;
uniform int uType;            // 0 standard · 1 lambert · 2 phong · 3 basic
uniform vec3 uColor; uniform float uOpacity;
uniform float uRoughness, uMetalness;
uniform vec3 uEmissive, uSpecular; uniform float uShininess;
uniform bool uHasMap, uHasEmissiveMap, uHasNormalMap, uTeamColorOn;
uniform sampler2D uMap, uEmissiveMap, uNormalMap;
uniform vec2 uNormalScale; uniform vec3 uTeamColor;
uniform vec3 uCamPos, uAmbient;
uniform vec3 uDirDir[2], uDirColor[2];
uniform vec3 uPtPos[4], uPtColor[4]; uniform float uPtDist[4], uPtDecay[4];
uniform vec3 uSpPos, uSpDir, uSpColor; uniform float uSpDist, uSpDecay; uniform vec2 uSpCos;   // cos outer, inner
uniform bool uReceiveShadow, uShadows;
uniform sampler2DShadow uShadowMap; uniform float uShadowBias, uShadowTexel;
uniform vec3 uFogColor; uniform float uFogNear, uFogFar; uniform bool uFog;
uniform vec4 uGlPos[8], uGlColor[8]; uniform int uGlCount;   // small lights: xyz + radius · rgb + reveal
const float PI = 3.141592653589793;

float sunShadow() {
  if (!uShadows || !uReceiveShadow) return 1.0;
  vec3 sc = vShadowPos.xyz / vShadowPos.w * 0.5 + 0.5;
  if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z > 1.0) return 1.0;
  sc.z += uShadowBias;
  float s = 0.0;
  for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++) s += texture(uShadowMap, vec3(sc.xy + vec2(x, y) * uShadowTexel, sc.z));
  return s / 9.0;
}
// three.js perturbNormal2Arb: tangent frame from screen-space derivatives (no tangents)
vec3 perturbNormal(vec3 pos, vec3 N, vec3 mapN, float faceDir) {
  vec3 q0 = dFdx(pos), q1 = dFdy(pos);
  vec2 st0 = dFdx(vUV), st1 = dFdy(vUV);
  vec3 q1perp = cross(q1, N), q0perp = cross(N, q0);
  vec3 T = q1perp * st0.x + q0perp * st1.x;
  vec3 B = q1perp * st0.y + q0perp * st1.y;
  float det = max(dot(T, T), dot(B, B));
  float scale = (det == 0.0) ? 0.0 : faceDir * inversesqrt(det);
  return normalize(T * (mapN.x * scale) + B * (mapN.y * scale) + mapN.z * N);
}
vec3 F_Schlick(vec3 f0, float dotLH) {
  float fr = exp2((-5.55473 * dotLH - 6.98316) * dotLH);
  return (1.0 - f0) * fr + f0;
}
vec3 specGGX(vec3 L, vec3 V, vec3 N, vec3 f0, float roughness) {
  float a = roughness * roughness, a2 = a * a;
  vec3 H = normalize(L + V);
  float dNL = clamp(dot(N, L), 0.0, 1.0), dNV = clamp(dot(N, V), 0.0, 1.0);
  float dNH = clamp(dot(N, H), 0.0, 1.0), dLH = clamp(dot(L, H), 0.0, 1.0);
  float gv = dNL * sqrt(a2 + (1.0 - a2) * dNV * dNV), gl = dNV * sqrt(a2 + (1.0 - a2) * dNL * dNL);
  float G = 0.5 / max(gv + gl, 1e-6);
  float den = dNH * dNH * (a2 - 1.0) + 1.0;
  float D = a2 / (PI * den * den);
  return F_Schlick(f0, dLH) * (G * D);
}
vec3 specBlinn(vec3 L, vec3 V, vec3 N, vec3 f0, float shin) {
  vec3 H = normalize(L + V);
  float dNH = clamp(dot(N, H), 0.0, 1.0), dLH = clamp(dot(L, H), 0.0, 1.0);
  return F_Schlick(f0, dLH) * (0.25 * (1.0 / PI) * (shin * 0.5 + 1.0) * pow(dNH, shin));
}
void main() {
  vec4 diffuse = vec4(uColor * vColor, uOpacity);
  if (uHasMap) {
    vec4 t = texture(uMap, vUV);
    if (uTeamColorOn) {   // SC2 team colour: the alpha masks the painted panels
      float m = smoothstep(0.98, 0.55, t.a);
      vec3 tc = uTeamColor * (0.3 + 1.2 * dot(t.rgb, vec3(0.299, 0.587, 0.114)));
      diffuse.rgb *= mix(t.rgb, tc, m);
    } else diffuse *= t;
  }
  vec3 outC;
  if (uType == 3) {
    outC = diffuse.rgb;
  } else {
    float faceDir = gl_FrontFacing ? 1.0 : -1.0;
    vec3 N = length(vNormal) > 0.0 ? normalize(vNormal) : vec3(0.0, 1.0, 0.0);
    if (uType == 0 && uHasNormalMap) {   // DXT5nm: X in alpha, Y in green, Z rebuilt
      vec4 nt = texture(uNormalMap, vUV);
      vec3 mapN = vec3(nt.a * 2.0 - 1.0, nt.g * 2.0 - 1.0, 0.0);
      mapN.z = sqrt(max(0.0, 1.0 - dot(mapN.xy, mapN.xy)));
      mapN.xy *= uNormalScale;
      N = perturbNormal(vWorld, N, mapN, faceDir);
    }
    vec3 V = normalize(uCamPos - vWorld);
    float sh = sunShadow();
    vec3 dif = diffuse.rgb, f0 = uSpecular;
    float rough = 1.0;
    if (uType == 0) {
      dif = diffuse.rgb * (1.0 - uMetalness);
      f0 = mix(vec3(0.04), diffuse.rgb, uMetalness);
      rough = clamp(max(uRoughness, 0.0525), 0.0, 1.0);
    }
    vec3 direct = vec3(0.0), spec = vec3(0.0);
    for (int i = 0; i < 7; i++) {   // 2 directional, 4 point, 1 spot
      vec3 L, col;
      if (i < 2) {
        L = uDirDir[i]; col = uDirColor[i];
        if (i == 0 && uType != 1) col *= sh;
      } else if (i < 6) {
        int k = i - 2;
        vec3 lv = uPtPos[k] - vWorld;
        float d = length(lv);
        L = lv / max(d, 1e-5);
        float att = 1.0;
        if (uPtDist[k] > 0.0 && uPtDecay[k] > 0.0) att = pow(clamp(1.0 - d / uPtDist[k], 0.0, 1.0), uPtDecay[k]);
        col = uPtColor[k] * att;
      } else {
        if (dot(uSpColor, uSpColor) == 0.0) continue;
        vec3 lv = uSpPos - vWorld;
        float d = length(lv);
        L = lv / max(d, 1e-5);
        float att = uSpDist > 0.0 ? pow(clamp(1.0 - d / uSpDist, 0.0, 1.0), uSpDecay) : 1.0;
        col = uSpColor * att * smoothstep(uSpCos.x, uSpCos.y, dot(-L, uSpDir));
      }
      if (dot(col, col) == 0.0) continue;
      float dNL = clamp(dot(N, L), 0.0, 1.0);
      vec3 irr = dNL * col;
      direct += irr;
      if (uType == 0) spec += irr * PI * specGGX(L, V, N, f0, rough);
      else if (uType == 2) spec += irr * PI * specBlinn(L, V, N, f0, uShininess);
    }
    if (uType == 1) direct *= sh;   // Lambert: the shadow mask darkens all direct light
    for (int i = 0; i < uGlCount; i++) {   // small lights: soft (half-wrapped), no highlight
      vec3 lv = uGlPos[i].xyz - vWorld;
      float d = length(lv);
      float att = pow(clamp(1.0 - d / uGlPos[i].w, 0.0, 1.0), 2.0);
      direct += uGlColor[i].rgb * att * (0.5 + 0.5 * dot(N, lv / max(d, 1e-5)));
    }
    // the lit term is clamped as the 8-bit target clamped it: lit surfaces look as before and never
    // bloom (smoke over a brightly lit hull stays as dense); only emitters exceed 1 in the HDR target
    outC = min(dif * (uAmbient + direct) + spec, vec3(1.0)) + uEmissive * (uHasEmissiveMap ? texture(uEmissiveMap, vUV).rgb : vec3(1.0));
  }
  if (uFog) {
    float fogF = smoothstep(uFogNear, uFogFar, length(vWorld.xz));   // radial: distance from the bunker
    float lift = 0.0;   // the small lights lift the night around them (full near, none at their reach)
    for (int i = 0; i < uGlCount; i++)
      lift = max(lift, uGlColor[i].a * (1.0 - smoothstep(0.35 * uGlPos[i].w, uGlPos[i].w, length(uGlPos[i].xyz - vWorld))));
    outC = mix(outC, uFogColor, fogF * (1.0 - lift));
  }
  fragColor = vec4(outC, diffuse.a);
}
)";

static const char* DEPTH_VS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=3) in mat4 iModel;
uniform mat4 uLightVP, uModel;
void main() { gl_Position = uLightVP * uModel * iModel * vec4(aPos, 1.0); }
)";
static const char* DEPTH_FS = R"(#version 330 core
void main() {}
)";

static const char* PART_VS = R"(#version 330 core
layout(location=0) in vec2 aCorner;
layout(location=1) in vec3 iPos;
layout(location=2) in float iSize;
layout(location=3) in vec3 iColor;
layout(location=4) in float iAlpha;
layout(location=5) in float iRot;
uniform mat4 uView, uProj;
out vec2 vCoord; out vec3 vColor; out float vAlpha; out vec3 vPos; flat out float vRot; out float vWy, vNear;
void main() {
  vec4 mv = uView * vec4(iPos, 1.0);
  mv.xy += aCorner * iSize;   // world-space diameter = size (the web version's size attenuation)
  vWy = iPos.y + iSize * (aCorner.x * uView[1][0] + aCorner.y * uView[1][1]);   // this corner's height in the world
  vNear = smoothstep(1.0 + 0.5 * iSize, 2.0 + 1.6 * iSize, -mv.z);   // fades out as the lens comes within about its size
  vCoord = aCorner + 0.5;
  vColor = iColor; vAlpha = iAlpha;
  vPos = iPos; vRot = iRot;
  gl_Position = uProj * mv;
}
)";
static const char* PART_FS = R"(#version 330 core
in vec2 vCoord; in vec3 vColor; in float vAlpha; in vec3 vPos; flat in float vRot; in float vWy, vNear;
uniform float uFogNear, uFogFar;
uniform vec4 uGlPos[8], uGlColor[8]; uniform int uGlCount;   // small lights (the lit shader's)
uniform bool uPuffs; uniform sampler2D uPuff; uniform vec3 uLightV;   // textured puffs: the atlas, the sun in view space
uniform float uCover;   // < 0: blended over (straight alpha); else premultiplied light that hides this much of what is behind
out vec4 fragColor;
void main() {
  float d = length(vCoord - 0.5) * 2.0;
  float a;
  vec3 col = vColor;
  if (uPuffs && vRot >= 0.0) {   // a soft billow: a turned atlas cell, gently lit by the sun
    if (d > 1.0) discard;
    float cell = floor(vRot / 8.0), ang = vRot - cell * 8.0, s = sin(ang), co = cos(ang);
    vec2 c = mat2(co, s, -s, co) * (vCoord - 0.5);
    vec4 t = texture(uPuff, (vec2(mod(cell, 2.0), floor(cell / 2.0)) + c + 0.5) * 0.5);
    a = vAlpha * t.r;
    vec2 n = mat2(co, -s, s, co) * (t.gb * 2.0 - 1.0);   // the cell's normal, turned back to the screen
    vec3 N = vec3(n, sqrt(max(0.0, 1.0 - dot(n, n))));
    col *= 0.8 + 0.3 * max(dot(N, uLightV), 0.0) + 0.1 * (t.a - 0.5);
  } else a = vAlpha * (1.0 - smoothstep(0.15, 1.0, d));   // soft round sprite
  if (uPuffs) a *= smoothstep(-0.05, 0.6, vWy) * vNear;   // the nuke's: no hard cut where they meet the ground, none filling the view
  float lift = 0.0;   // fade out in the night fog, except near a small light
  for (int i = 0; i < uGlCount; i++)
    lift = max(lift, uGlColor[i].a * (1.0 - smoothstep(0.35 * uGlPos[i].w, uGlPos[i].w, length(uGlPos[i].xyz - vPos))));
  a *= 1.0 - smoothstep(uFogNear, uFogFar, length(vPos.xz)) * (1.0 - lift);
  if (a < 0.004) discard;
  fragColor = uCover < 0.0 ? vec4(col, a) : vec4(col * a, a * uCover);
}
)";

static GLuint compileShader(GLenum type, const char* src, const char* name) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char buf[4096];
    glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
    logError("shader %s (%s) failed to compile:\n%s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", buf);
    glDeleteShader(s);
    return 0;
  }
  return s;
}
GLuint compileProgram(const char* vs, const char* fs, const char* name) {
  GLuint v = compileShader(GL_VERTEX_SHADER, vs, name), f = compileShader(GL_FRAGMENT_SHADER, fs, name);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char buf[4096];
    glGetProgramInfoLog(p, sizeof buf, nullptr, buf);
    logError("program %s failed to link:\n%s", name, buf);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static const int SHADOW_SIZE = 2048;

bool Renderer::init() {
  litProg_ = compileProgram(LIT_VS, LIT_FS, "lit");
  depthProg_ = compileProgram(DEPTH_VS, DEPTH_FS, "depth");
  partProg_ = compileProgram(PART_VS, PART_FS, "particles");
  if (!litProg_ || !depthProg_ || !partProg_) return false;
  glGenBuffers(1, &instVbo_);

  // Sun shadow map: 2048², hardware depth compare (PCF in the shader)
  glGenTextures(1, &shadowTex_);
  glBindTexture(GL_TEXTURE_2D, shadowTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_SIZE, SHADOW_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
  glGenFramebuffers(1, &shadowFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (st != GL_FRAMEBUFFER_COMPLETE) { logError("shadow framebuffer incomplete (0x%x)", st); return false; }

  // 1×1 white texture for unbound samplers
  glGenTextures(1, &whiteTex_);
  glBindTexture(GL_TEXTURE_2D, whiteTex_);
  const uint8_t white[4] = {255, 255, 255, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);

  // Particle quad + instance buffer (pos3 size1 col3 alpha1)
  glGenVertexArrays(1, &partVao_);
  glBindVertexArray(partVao_);
  const float quad[12] = {-.5f, -.5f, .5f, -.5f, .5f, .5f, -.5f, -.5f, .5f, .5f, -.5f, .5f};
  glGenBuffers(1, &partQuadVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, partQuadVbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
  glGenBuffers(1, &partInstVbo_);
  glBindBuffer(GL_ARRAY_BUFFER, partInstVbo_);
  const int st8 = 9 * sizeof(float);
  glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st8, (void*)0); glVertexAttribDivisor(1, 1);
  glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, st8, (void*)(3 * sizeof(float))); glVertexAttribDivisor(2, 1);
  glEnableVertexAttribArray(3); glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, st8, (void*)(4 * sizeof(float))); glVertexAttribDivisor(3, 1);
  glEnableVertexAttribArray(4); glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, st8, (void*)(7 * sizeof(float))); glVertexAttribDivisor(4, 1);
  glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, st8, (void*)(8 * sizeof(float))); glVertexAttribDivisor(5, 1);
  glBindVertexArray(0);
  postOk_ = post_.init();
  if (!postOk_) logWarn("post-processing unavailable: drawing straight to the window");
  return true;
}

void Renderer::begin() {
  cmds_.clear();
  inst_.clear();
  customs_.clear();
}

void Renderer::draw(const Mesh* mesh, const Material* mat, const glm::mat4& model, bool cast, bool receive,
                    const glm::vec3& color, int order) {
  if (!mesh || !mat) return;
  int first = (int)inst_.size();
  inst_.push_back({glm::mat4(1), color});
  cmds_.push_back({mesh, mat, model, first, 1, cast, receive, order, nullptr, Blend::Normal});
}
void Renderer::drawInstanced(const Mesh* mesh, const Material* mat, const Instance* inst, int n, bool cast, bool receive,
                             int order) {
  if (!mesh || !mat || n <= 0) return;
  int first = (int)inst_.size();
  inst_.insert(inst_.end(), inst, inst + n);
  cmds_.push_back({mesh, mat, glm::mat4(1), first, n, cast, receive, order, nullptr, Blend::Normal});
}
void Renderer::drawParticles(const ParticleSystem* ps, Blend blend, int order) {
  if (!ps || ps->p.empty()) return;
  cmds_.push_back({nullptr, nullptr, glm::mat4(1), 0, (int)ps->p.size(), false, false, order, ps, blend});
}
void Renderer::drawCustom(int order, const glm::vec3& at, CustomDraw fn) {
  if (!fn) return;
  cmds_.push_back({nullptr, nullptr, glm::mat4(1), 0, 0, false, false, order, nullptr, Blend::Normal, (int)customs_.size()});
  customs_.push_back({at, std::move(fn)});
}

void Renderer::bindInstances(int first) {
  glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
  const GLsizei st = sizeof(Instance);
  const char* base = (const char*)(size_t)(first * sizeof(Instance));
  for (int c = 0; c < 4; c++) glVertexAttribPointer(3 + c, 4, GL_FLOAT, GL_FALSE, st, base + offsetof(Instance, m) + c * 16);
  glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, st, base + offsetof(Instance, c));
}

// uniform locations, looked up once per (program, name literal)
static GLint U(GLuint p, const char* n) {
  static std::unordered_map<GLuint, std::unordered_map<const char*, GLint>> cache;
  auto& m = cache[p];
  auto it = m.find(n);
  if (it != m.end()) return it->second;
  return m[n] = glGetUniformLocation(p, n);
}

void Renderer::setMaterial(const Material& m, bool receive, bool shadowsOn) {
  GLuint p = litProg_;
  glUniform1i(U(p, "uType"), (int)m.type);
  glUniform3fv(U(p, "uColor"), 1, glm::value_ptr(m.color));
  glUniform1f(U(p, "uOpacity"), m.transparent ? m.opacity : 1.f);
  glUniform1f(U(p, "uRoughness"), m.roughness);
  glUniform1f(U(p, "uMetalness"), m.metalness);
  glUniform3fv(U(p, "uEmissive"), 1, glm::value_ptr(m.emissive));
  glUniform3fv(U(p, "uSpecular"), 1, glm::value_ptr(m.specular));
  glUniform1f(U(p, "uShininess"), m.shininess);
  glUniform1i(U(p, "uHasMap"), m.map != nullptr);
  glUniform1i(U(p, "uHasEmissiveMap"), m.emissiveMap != nullptr);
  glUniform1i(U(p, "uHasNormalMap"), m.normalMap != nullptr);
  glUniform1i(U(p, "uTeamColorOn"), m.teamColorOn);
  glUniform3fv(U(p, "uTeamColor"), 1, glm::value_ptr(m.teamColor));
  glUniform2fv(U(p, "uNormalScale"), 1, glm::value_ptr(m.normalScale));
  glUniform1i(U(p, "uReceiveShadow"), receive && shadowsOn);
  glUniform1i(U(p, "uFog"), m.fog);
  glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m.map ? m.map->id : whiteTex_);
  glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m.emissiveMap ? m.emissiveMap->id : whiteTex_);
  glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m.normalMap ? m.normalMap->id : whiteTex_);
  if (m.doubleSide) glDisable(GL_CULL_FACE);
  else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
  if (m.transparent) {
    glEnable(GL_BLEND);
    if (m.blend == Blend::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(m.depthWrite ? GL_TRUE : GL_FALSE);
  } else {
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
  }
}

void Renderer::render(const SceneParams& sp) {
  // one instance upload per frame; draws address their range via attribute offsets
  glBindBuffer(GL_ARRAY_BUFFER, instVbo_);
  glBufferData(GL_ARRAY_BUFFER, inst_.size() * sizeof(Instance), inst_.data(), GL_STREAM_DRAW);

  glm::vec3 sunDir = glm::normalize(sp.sun.pos);
  glm::mat4 lightVP = glm::ortho(-26.f, 26.f, -26.f, 26.f, 1.f, 70.f) *
                      glm::lookAt(sp.sun.pos, glm::vec3(0), std::abs(sunDir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0));
  bool shadows = sp.shadows && sp.sun.intensity > 0;
  // the scene goes into the HDR target (post.h), drawn at its origin; without post, into the window at the canvas
  postActive_ = postOk_ && sp.post.enabled && post_.ensure(sp.vpW, sp.vpH, sp.msaa);
  const GLuint target = postActive_ ? post_.drawFbo() : 0;
  const int tx = postActive_ ? 0 : sp.vpX, ty = postActive_ ? 0 : sp.vpY;

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  if (shadows) {
    // three.js draws shadow casters with the opposite side (back faces of front-side materials)
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, SHADOW_SIZE, SHADOW_SIZE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glUseProgram(depthProg_);
    glUniformMatrix4fv(U(depthProg_, "uLightVP"), 1, GL_FALSE, glm::value_ptr(lightVP));
    for (const Cmd& c : cmds_) {
      if (!c.cast || !c.mesh) continue;
      if (c.mat->doubleSide) glDisable(GL_CULL_FACE);
      else { glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); }
      glUniformMatrix4fv(U(depthProg_, "uModel"), 1, GL_FALSE, glm::value_ptr(c.model));
      glBindVertexArray(c.mesh->vao);
      bindInstances(c.first);
      if (c.mesh->indexed) glDrawElementsInstanced(GL_TRIANGLES, c.mesh->count, GL_UNSIGNED_INT, nullptr, c.count);
      else glDrawArraysInstanced(GL_TRIANGLES, 0, c.mesh->count, c.count);
    }
  }

  // bound on every frame, shadows or not (no sun at night, none on LOW)
  glBindFramebuffer(GL_FRAMEBUFFER, target);
  glViewport(tx, ty, sp.vpW, sp.vpH);
  glScissor(tx, ty, sp.vpW, sp.vpH);   // glClear and the resolve obey the scissor: it must cover the target
  glEnable(GL_SCISSOR_TEST);
  if (sp.msaa) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE);
  glClearColor(sp.fogColor.r, sp.fogColor.g, sp.fogColor.b, 1);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  glUseProgram(litProg_);
  GLuint p = litProg_;
  glUniformMatrix4fv(U(p, "uView"), 1, GL_FALSE, glm::value_ptr(sp.view));
  glUniformMatrix4fv(U(p, "uProj"), 1, GL_FALSE, glm::value_ptr(sp.proj));
  glUniformMatrix4fv(U(p, "uLightVP"), 1, GL_FALSE, glm::value_ptr(lightVP));
  glUniform1f(U(p, "uNormalBias"), 0.02f);
  glUniform1f(U(p, "uShadowBias"), -0.0005f);
  glUniform1f(U(p, "uShadowTexel"), 1.f / SHADOW_SIZE);
  glUniform3fv(U(p, "uCamPos"), 1, glm::value_ptr(sp.camPos));
  glm::vec3 amb = sp.ambientColor * sp.ambientIntensity;
  glUniform3fv(U(p, "uAmbient"), 1, glm::value_ptr(amb));
  glm::vec3 dd[2] = {sunDir, glm::normalize(sp.fill.pos)};
  glm::vec3 dc[2] = {sp.sun.color * sp.sun.intensity, sp.fill.color * sp.fill.intensity};
  glUniform3fv(U(p, "uDirDir"), 2, glm::value_ptr(dd[0]));
  glUniform3fv(U(p, "uDirColor"), 2, glm::value_ptr(dc[0]));
  glm::vec3 pp[4], pc[4];
  float pd[4], pdec[4];
  for (int i = 0; i < 4; i++) {
    pp[i] = sp.points[i].pos;
    pc[i] = sp.points[i].color * sp.points[i].intensity;
    pd[i] = sp.points[i].distance;
    pdec[i] = sp.points[i].decay;
  }
  glUniform3fv(U(p, "uPtPos"), 4, glm::value_ptr(pp[0]));
  glUniform3fv(U(p, "uPtColor"), 4, glm::value_ptr(pc[0]));
  glUniform1fv(U(p, "uPtDist"), 4, pd);
  glUniform1fv(U(p, "uPtDecay"), 4, pdec);
  glm::vec3 spc = sp.spot.color * sp.spot.intensity, spd = glm::length(sp.spot.dir) > 0 ? glm::normalize(sp.spot.dir) : glm::vec3(0, -1, 0);
  glUniform3fv(U(p, "uSpPos"), 1, glm::value_ptr(sp.spot.pos));
  glUniform3fv(U(p, "uSpDir"), 1, glm::value_ptr(spd));
  glUniform3fv(U(p, "uSpColor"), 1, glm::value_ptr(spc));
  glUniform1f(U(p, "uSpDist"), sp.spot.distance);
  glUniform1f(U(p, "uSpDecay"), sp.spot.decay);
  glUniform2f(U(p, "uSpCos"), sp.spot.cosOuter, sp.spot.cosInner);
  glUniform1i(U(p, "uShadows"), shadows);
  glUniform3fv(U(p, "uFogColor"), 1, glm::value_ptr(sp.fogColor));
  glUniform1f(U(p, "uFogNear"), sp.fogNear);
  glUniform1f(U(p, "uFogFar"), sp.fogFar);
  glm::vec4 glP[MAX_GLOWS], glC[MAX_GLOWS];   // the small lights (lit shader and sprites)
  const int glN = std::clamp(sp.glowCount, 0, MAX_GLOWS);
  for (int i = 0; i < glN; i++) {
    glP[i] = glm::vec4(sp.glows[i].pos, sp.glows[i].radius);
    glC[i] = glm::vec4(sp.glows[i].color, sp.glows[i].reveal);
  }
  auto glowUniforms = [&](GLuint prog) {
    glUniform1i(U(prog, "uGlCount"), glN);
    if (glN == 0) return;
    glUniform4fv(U(prog, "uGlPos"), glN, glm::value_ptr(glP[0]));
    glUniform4fv(U(prog, "uGlColor"), glN, glm::value_ptr(glC[0]));
  };
  glowUniforms(p);
  glUniform1i(U(p, "uMap"), 0);
  glUniform1i(U(p, "uEmissiveMap"), 1);
  glUniform1i(U(p, "uNormalMap"), 2);
  glUniform1i(U(p, "uShadowMap"), 3);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, shadowTex_);

  auto drawCmd = [&](const Cmd& c) {
    setMaterial(*c.mat, c.receive, shadows);
    glUniformMatrix4fv(U(p, "uModel"), 1, GL_FALSE, glm::value_ptr(c.model));
    glBindVertexArray(c.mesh->vao);
    bindInstances(c.first);
    if (c.mesh->indexed) glDrawElementsInstanced(GL_TRIANGLES, c.mesh->count, GL_UNSIGNED_INT, nullptr, c.count);
    else glDrawArraysInstanced(GL_TRIANGLES, 0, c.mesh->count, c.count);
  };
  // opaque
  for (const Cmd& c : cmds_) if (c.mesh && !c.mat->transparent) drawCmd(c);

  // transparent + particles: by render order, then back to front
  std::vector<std::pair<float, const Cmd*>> tr;
  for (const Cmd& c : cmds_) {
    if (c.ps || c.custom >= 0 || (c.mesh && c.mat->transparent)) {
      glm::vec3 pos = c.custom >= 0 ? customs_[c.custom].at : c.ps ? glm::vec3(0) : glm::vec3(c.model * inst_[c.first].m[3]);
      tr.push_back({(sp.view * glm::vec4(pos, 1)).z, &c});
    }
  }
  std::stable_sort(tr.begin(), tr.end(), [](auto& a, auto& b) {
    if (a.second->order != b.second->order) return a.second->order < b.second->order;
    return a.first < b.first;   // farther (more negative view z) first
  });
  std::vector<float> pbuf;
  FrameCtx ctx;
  ctx.view = sp.view;
  ctx.proj = sp.proj;
  ctx.camPos = sp.camPos;
  ctx.fogColor = sp.fogColor;
  ctx.fogNear = sp.fogNear;
  ctx.fogFar = sp.fogFar;
  ctx.hdr = postActive_;
  for (auto& [z, c] : tr) {
    if (c->custom >= 0) {
      customs_[c->custom].fn(ctx);
      continue;
    }
    if (!c->ps) {
      glUseProgram(litProg_);
      drawCmd(*c);
      continue;
    }
    pbuf.clear();
    pbuf.reserve(c->ps->p.size() * 9);
    for (const auto& q : c->ps->p) pbuf.insert(pbuf.end(), {q.pos.x, q.pos.y, q.pos.z, q.size, q.col.r, q.col.g, q.col.b, q.alpha, q.rot});
    glUseProgram(partProg_);
    glUniformMatrix4fv(U(partProg_, "uView"), 1, GL_FALSE, glm::value_ptr(sp.view));
    glUniformMatrix4fv(U(partProg_, "uProj"), 1, GL_FALSE, glm::value_ptr(sp.proj));
    glUniform1f(U(partProg_, "uFogNear"), sp.fogNear);
    glUniform1f(U(partProg_, "uFogFar"), sp.fogFar);
    glUniform1i(U(partProg_, "uPuffs"), c->ps->tex != nullptr);
    if (c->ps->tex) {
      glm::vec3 lv = glm::normalize(glm::mat3(sp.view) * sunDir);
      glUniform3fv(U(partProg_, "uLightV"), 1, glm::value_ptr(lv));
      glUniform1i(U(partProg_, "uPuff"), 0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, c->ps->tex->id);
    }
    glowUniforms(partProg_);
    glBindVertexArray(partVao_);
    glBindBuffer(GL_ARRAY_BUFFER, partInstVbo_);
    glBufferData(GL_ARRAY_BUFFER, pbuf.size() * sizeof(float), pbuf.data(), GL_STREAM_DRAW);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glUniform1f(U(partProg_, "uCover"), c->psBlend == Blend::Additive ? c->ps->cover : -1.f);
    if (c->psBlend == Blend::Additive) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied: cover 0 is plain additive
    else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)c->ps->p.size());
  }
  if (gpuParticles_) gpuParticles_->draw(sp.view, sp.proj, sp.fogNear, sp.fogFar);
  glBindVertexArray(0);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glActiveTexture(GL_TEXTURE0);
  if (postActive_) post_.finish(sp.post, sp.vpX, sp.vpY, sp.vpW, sp.vpH, !sp.msaa);   // ends on framebuffer 0
}

bool Renderer::saveScreenshot(const std::string& path, int w, int h) {
  std::vector<uint8_t> px((size_t)w * h * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  stbi_flip_vertically_on_write(1);
  return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}
