#include "gpu_particles.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/type_ptr.hpp>

#include "log.h"

// GL 4.3 entry points and enums (the bundled glad loader stops at 3.3)
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
typedef void(GLAD_API_PTR* PfnDispatchCompute)(GLuint, GLuint, GLuint);
typedef void(GLAD_API_PTR* PfnMemoryBarrier)(GLbitfield);
static PfnDispatchCompute pDispatchCompute = nullptr;
static PfnMemoryBarrier pMemoryBarrier = nullptr;

// std430 layouts: a particle is 5 vec4, a burst 8 vec4
static const int P_FLOATS = 20, B_FLOATS = 32;

static const char* COMMON = R"(
struct P { vec4 pos; vec4 vel; vec4 c0; vec4 c1; vec4 m; };
// pos.w life left · vel.w full life · c0 rgb+alpha · c1 rgb + size at birth · m: size at death, gravity, drag, fade
)";

static const char* SPAWN_CS = R"(#version 430
layout(local_size_x = 256) in;
%COMMON%
struct B { vec4 pos; vec4 vel; vec4 box; vec4 c0; vec4 c1; vec4 life; vec4 misc; uvec4 idx; };
// pos.xyz · vel: base xyz + speed · box: spread xyz + spawn radius · c0 rgb a0 · c1 rgb grav
// life: min, max, size0, size1 · misc: drag, up, fade, - · idx: first, count, seed, shape
layout(std430, binding = 0) buffer Parts { P parts[]; };
layout(std430, binding = 1) readonly buffer Bursts { B bursts[]; };
uniform uint uCap;
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float rnd(inout uint s) { s = hash(s); return float(s) * (1.0 / 4294967296.0); }
void main() {
  B b = bursts[gl_WorkGroupID.x];
  for (uint i = gl_LocalInvocationID.x; i < b.idx.y; i += 256u) {
    uint s = hash(b.idx.z ^ (i * 0x9e3779b9u));
    float u = rnd(s), v = rnd(s);
    vec3 dir;
    if (b.idx.w == 1u) { float a = u * 6.2831853; dir = vec3(cos(a), 0.05 * v, sin(a)); }                 // ring
    else if (b.idx.w == 2u) { float a = u * 6.2831853, r = 0.35 * sqrt(v); dir = normalize(vec3(r * cos(a), 1.0, r * sin(a))); }   // column
    else { float z = u * 2.0 - 1.0, a = v * 6.2831853, r = sqrt(max(0.0, 1.0 - z * z)); dir = vec3(r * cos(a), z, r * sin(a)); }  // sphere
    vec3 vel = b.vel.xyz;
    if (b.idx.w == 3u) vel += vec3((rnd(s) - 0.5) * 2.0 * b.box.x, rnd(s) * b.box.y, (rnd(s) - 0.5) * 2.0 * b.box.z);   // box (the CPU sparks' spread)
    else vel += dir * b.vel.w * (0.25 + 0.75 * rnd(s));
    vel.y += b.misc.y * (0.5 + rnd(s));
    vec3 pos = b.pos.xyz + dir * b.box.w * sqrt(rnd(s));
    float life = mix(b.life.x, b.life.y, rnd(s)), k = 0.6 + 0.8 * rnd(s);
    P p;
    p.pos = vec4(pos, life);
    p.vel = vec4(vel, life);
    p.c0 = b.c0;
    p.c1 = vec4(b.c1.rgb, b.life.z * k);
    p.m = vec4(b.life.w * k, b.c1.w, b.misc.x, b.misc.z);
    parts[(b.idx.x + i) % uCap] = p;
  }
}
)";

static const char* UPDATE_CS = R"(#version 430
layout(local_size_x = 256) in;
%COMMON%
layout(std430, binding = 0) buffer Parts { P parts[]; };
uniform uint uCap;
uniform float uDt;
void main() {
  uint i = gl_GlobalInvocationID.x;
  if (i >= uCap) return;
  P p = parts[i];
  if (p.pos.w <= 0.0) return;
  p.pos.w -= uDt;
  float k = max(0.0, 1.0 - p.m.z * uDt);
  p.vel.xz *= k;
  p.vel.y = p.vel.y * k - p.m.y * uDt;
  p.pos.xyz += p.vel.xyz * uDt;
  if (p.pos.y < 0.03 && p.vel.y < 0.0) {   // falling debris bounces, then settles on the ground
    p.pos.y = 0.03;
    p.vel.y *= -0.3;
    p.vel.xz *= 0.55;
  }
  parts[i] = p;
}
)";

static const char* DRAW_VS = R"(#version 430
layout(location = 0) in vec2 aCorner;
%COMMON%
layout(std430, binding = 0) readonly buffer Parts { P parts[]; };
uniform mat4 uView, uProj;
uniform uint uBase, uCap;
out vec2 vCoord; out vec3 vColor; out float vAlpha; out float vFogD;
void main() {
  P p = parts[(uBase + uint(gl_InstanceID)) % uCap];
  vCoord = aCorner + 0.5;
  if (p.pos.w <= 0.0) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vAlpha = 0.0; vColor = vec3(0.0); vFogD = 0.0; return; }   // dead: clipped
  float t = 1.0 - p.pos.w / p.vel.w;
  float size = mix(p.c1.w, p.m.x, t);
  vAlpha = p.c0.a * (p.m.w > 0.5 ? sin(3.14159265 * min(1.0, t * 1.15)) : 1.0 - t);
  vColor = mix(p.c0.rgb, p.c1.rgb, t);
  vFogD = length(p.pos.xz);
  vec4 mv = uView * vec4(p.pos.xyz, 1.0);
  mv.xy += aCorner * size;
  gl_Position = uProj * mv;
}
)";

static const char* DRAW_FS = R"(#version 430
in vec2 vCoord; in vec3 vColor; in float vAlpha; in float vFogD;
uniform float uFogNear, uFogFar;
out vec4 fragColor;
void main() {
  float d = length(vCoord - 0.5) * 2.0;
  float a = vAlpha * (1.0 - smoothstep(0.15, 1.0, d));   // soft round sprite, as the CPU particles
  a *= 1.0 - smoothstep(uFogNear, uFogFar, vFogD);
  if (a < 0.004) discard;
  fragColor = vec4(vColor, a);
}
)";

static std::string withCommon(const char* src) {
  std::string s = src;
  size_t at = s.find("%COMMON%");
  if (at != std::string::npos) s.replace(at, 8, COMMON);
  return s;
}

static GLuint compileCompute(const char* src, const char* name) {
  std::string code = withCommon(src);
  const char* c = code.c_str();
  GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
  glShaderSource(sh, 1, &c, nullptr);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char buf[4096];
    glGetShaderInfoLog(sh, sizeof buf, nullptr, buf);
    logError("compute shader %s failed to compile:\n%s", name, buf);
    glDeleteShader(sh);
    return 0;
  }
  GLuint p = glCreateProgram();
  glAttachShader(p, sh);
  glLinkProgram(p);
  glDeleteShader(sh);
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { logError("compute program %s failed to link", name); glDeleteProgram(p); return 0; }
  return p;
}

static GLuint makeBuffer(int cap) {
  GLuint b;
  glGenBuffers(1, &b);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
  std::vector<float> zero((size_t)cap * P_FLOATS, 0.f);   // life 0 = dead
  glBufferData(GL_SHADER_STORAGE_BUFFER, zero.size() * sizeof(float), zero.data(), GL_DYNAMIC_DRAW);
  return b;
}

bool GpuParticles::init(int glowCap, int smokeCap) {
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  if (major * 10 + minor < 43) { logWarn("GPU particles: OpenGL %d.%d has no compute shaders (4.3 needed) — CPU particles only", major, minor); return false; }
  pDispatchCompute = (PfnDispatchCompute)glfwGetProcAddress("glDispatchCompute");
  pMemoryBarrier = (PfnMemoryBarrier)glfwGetProcAddress("glMemoryBarrier");
  if (!pDispatchCompute || !pMemoryBarrier) { logWarn("GPU particles: compute entry points missing — CPU particles only"); return false; }
  spawnProg_ = compileCompute(SPAWN_CS, "spawn");
  updateProg_ = compileCompute(UPDATE_CS, "update");
  std::string vs = withCommon(DRAW_VS);
  drawProg_ = compileProgram(vs.c_str(), DRAW_FS, "gpu particles");
  if (!spawnProg_ || !updateProg_ || !drawProg_) return false;
  glow_.cap = glowCap;
  smoke_.cap = smokeCap;
  glow_.buf = makeBuffer(glowCap);
  smoke_.buf = makeBuffer(smokeCap);
  glGenBuffers(1, &burstBuf_);
  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);
  const float quad[12] = {-.5f, -.5f, .5f, -.5f, .5f, .5f, -.5f, -.5f, .5f, .5f, -.5f, .5f};
  glGenBuffers(1, &quad_);
  glBindBuffer(GL_ARRAY_BUFFER, quad_);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, nullptr);
  glBindVertexArray(0);
  ok_ = true;
  logInfo("GPU particles: %d glow + %d smoke, compute shaders on OpenGL %d.%d", glowCap, smokeCap, major, minor);
  return true;
}

void GpuParticles::burst(const Burst& b) {
  if (!ok_ || b.count <= 0) return;
  Sys& s = b.smoke ? smoke_ : glow_;
  int n = std::min(b.count, s.cap);
  glm::vec3 c0 = hexColor(b.c0), c1 = hexColor(b.c1);
  static uint32_t seed = 0x1234567u;
  seed = seed * 1664525u + 1013904223u;
  uint32_t first = (uint32_t)(s.head % (uint64_t)s.cap);
  float f[B_FLOATS] = {b.pos.x, b.pos.y, b.pos.z, 0,
                       b.vel.x, b.vel.y, b.vel.z, b.speed,
                       b.box.x, b.box.y, b.box.z, b.radius,
                       c0.r, c0.g, c0.b, b.a0,
                       c1.r, c1.g, c1.b, b.grav,
                       b.life0, b.life1, b.s0, b.s1,
                       b.drag, b.up, b.fade ? 1.f : 0.f, 0};
  uint32_t u[4] = {first, (uint32_t)n, seed, (uint32_t)b.shape};
  memcpy(&f[28], u, sizeof u);   // the uvec4 shares the float array's bits
  s.pending.insert(s.pending.end(), f, f + B_FLOATS);
  s.pendingCount++;
  s.head += n;
  s.maxLife = std::max(s.maxLife, b.life1);
}

void GpuParticles::spawn(Sys& s) {
  if (!s.pendingCount) return;
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, burstBuf_);
  glBufferData(GL_SHADER_STORAGE_BUFFER, s.pending.size() * sizeof(float), s.pending.data(), GL_STREAM_DRAW);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s.buf);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, burstBuf_);
  glUseProgram(spawnProg_);
  glUniform1ui(glGetUniformLocation(spawnProg_, "uCap"), (GLuint)s.cap);
  pDispatchCompute((GLuint)s.pendingCount, 1, 1);   // one work group per burst
  pMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  s.pending.clear();
  s.pendingCount = 0;
}

void GpuParticles::update(float dt) {
  if (!ok_) return;
  for (Sys* s : {&glow_, &smoke_}) spawn(*s);
  if (dt > 0) {
    simT_ += dt;
    glUseProgram(updateProg_);
    glUniform1f(glGetUniformLocation(updateProg_, "uDt"), dt);
    for (Sys* s : {&glow_, &smoke_}) {
      if (s->history.empty() && s->head == 0) continue;   // nothing ever spawned
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s->buf);
      glUniform1ui(glGetUniformLocation(updateProg_, "uCap"), (GLuint)s->cap);
      pDispatchCompute((GLuint)((s->cap + 255) / 256), 1, 1);
    }
    pMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
  }
  for (Sys* s : {&glow_, &smoke_}) {   // what may still be alive: spawned within the longest lifetime
    s->history.push_back({simT_, s->head});
    while (s->history.size() > 1 && s->history[1].first < simT_ - s->maxLife - 0.1) s->history.pop_front();
  }
  glUseProgram(0);
}

// The history keeps one entry older than the longest lifetime: everything
// spawned up to it is dead, so drawing starts there (not at the whole ring)
uint64_t GpuParticles::Sys::oldestLive(double now) const {
  uint64_t h = (!history.empty() && history.front().first < now - maxLife - 0.1) ? history.front().second : 0;
  return std::max<uint64_t>(h, head > (uint64_t)cap ? head - cap : 0);
}

int GpuParticles::live() const {
  return (int)((glow_.head - glow_.oldestLive(simT_)) + (smoke_.head - smoke_.oldestLive(simT_)));
}

void GpuParticles::drawSys(const Sys& s, const glm::mat4& view, const glm::mat4& proj, float fogNear, float fogFar, bool additive) {
  uint64_t from = s.oldestLive(simT_);
  int count = (int)std::min<uint64_t>(s.head - from, (uint64_t)s.cap);
  if (count <= 0) return;
  glUseProgram(drawProg_);
  glUniformMatrix4fv(glGetUniformLocation(drawProg_, "uView"), 1, GL_FALSE, glm::value_ptr(view));
  glUniformMatrix4fv(glGetUniformLocation(drawProg_, "uProj"), 1, GL_FALSE, glm::value_ptr(proj));
  glUniform1f(glGetUniformLocation(drawProg_, "uFogNear"), fogNear);
  glUniform1f(glGetUniformLocation(drawProg_, "uFogFar"), fogFar);
  glUniform1ui(glGetUniformLocation(drawProg_, "uBase"), (GLuint)(from % (uint64_t)s.cap));
  glUniform1ui(glGetUniformLocation(drawProg_, "uCap"), (GLuint)s.cap);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s.buf);
  glBindVertexArray(vao_);
  glEnable(GL_BLEND);
  if (additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
  else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glDrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
}

void GpuParticles::draw(const glm::mat4& view, const glm::mat4& proj, float fogNear, float fogFar) {
  if (!ok_) return;
  drawSys(smoke_, view, proj, fogNear, fogFar, false);   // glow over smoke
  drawSys(glow_, view, proj, fogNear, fogFar, true);
  glBindVertexArray(0);
}

void GpuParticles::clear() {
  if (!ok_) return;
  for (Sys* s : {&glow_, &smoke_}) {
    std::vector<float> zero((size_t)s->cap * P_FLOATS, 0.f);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s->buf);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, zero.size() * sizeof(float), zero.data());
    s->pending.clear();
    s->pendingCount = 0;
    s->history.clear();
    s->head = 0;
    s->maxLife = 0;
  }
}
