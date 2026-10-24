#include "post.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/type_ptr.hpp>

#include "log.h"

GLuint compileProgram(const char* vs, const char* fs, const char* name, const char* defines = nullptr);   // render.cpp

namespace {
// One triangle over the whole viewport (no vertex buffer: gl_VertexID)
const char* FULL_VS = R"(#version 330 core
out vec2 vUV;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  vUV = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Bloom level 0 at half resolution: the 13-tap downsample of Jimenez (CoD AW), its
// four outer groups luma-weighted (Karis) so a lone hot spark does not flicker, the
// centre group plain so small emitters (LEDs, tracers) still bloom; then a soft
// threshold — the scene is display-referred, so only emitters exceed 1.
const char* PRE_FS = R"(#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uSrc; uniform vec2 uTexel; uniform vec4 uThr;   // T, T-K, 2K, 0.25/K
vec3 S(vec2 d) {
  vec3 c = min(texture(uSrc, vUV + d * uTexel).rgb, vec3(64.0));
  return any(isnan(c)) ? vec3(0.0) : c;
}
float W(vec3 c) { return 1.0 / (1.0 + 0.25 * dot(c, vec3(0.2126, 0.7152, 0.0722))); }
void main() {
  vec3 A = S(vec2(-1, -1)), B = S(vec2(1, -1)), C = S(vec2(-1, 1)), D = S(vec2(1, 1));
  vec3 E = S(vec2(-2, -2)), F = S(vec2(0, -2)), G = S(vec2(2, -2)), H = S(vec2(-2, 0)), I = S(vec2(0, 0));
  vec3 J = S(vec2(2, 0)), K = S(vec2(-2, 2)), L = S(vec2(0, 2)), M = S(vec2(2, 2));
  vec3 g0 = (A + B + C + D) * 0.25, g1 = (E + F + H + I) * 0.25, g2 = (F + G + I + J) * 0.25;
  vec3 g3 = (H + I + K + L) * 0.25, g4 = (I + J + L + M) * 0.25;
  float w1 = 0.125 * W(g1), w2 = 0.125 * W(g2), w3 = 0.125 * W(g3), w4 = 0.125 * W(g4);
  vec3 c = (g0 * 0.5 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (0.5 + w1 + w2 + w3 + w4);
  float br = max(c.r, max(c.g, c.b));
  float rq = clamp(br - uThr.y, 0.0, uThr.z);
  rq = uThr.w * rq * rq;
  o = vec4(c * max(rq, br - uThr.x) / max(br, 1e-4), 1.0);
}
)";

const char* DOWN_FS = R"(#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uSrc; uniform vec2 uTexel;
vec3 S(vec2 d) { return texture(uSrc, vUV + d * uTexel).rgb; }
void main() {
  vec3 c = (S(vec2(-1, -1)) + S(vec2(1, -1)) + S(vec2(-1, 1)) + S(vec2(1, 1))) * 0.125
         + (S(vec2(-2, -2)) + S(vec2(2, -2)) + S(vec2(-2, 2)) + S(vec2(2, 2))) * 0.03125
         + (S(vec2(0, -2)) + S(vec2(-2, 0)) + S(vec2(2, 0)) + S(vec2(0, 2))) * 0.0625
         + S(vec2(0, 0)) * 0.125;
  o = vec4(c, 1.0);
}
)";

// Upsample with a 3x3 tent, added into the next larger level
const char* UP_FS = R"(#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uSrc; uniform vec2 uTexel;
vec3 S(vec2 d) { return texture(uSrc, vUV + d * uTexel).rgb; }
void main() {
  vec3 c = S(vec2(0, 0)) * 4.0 + (S(vec2(1, 0)) + S(vec2(-1, 0)) + S(vec2(0, 1)) + S(vec2(0, -1))) * 2.0
         + S(vec2(1, 1)) + S(vec2(-1, 1)) + S(vec2(1, -1)) + S(vec2(-1, -1));
  o = vec4(c / 16.0, 1.0);
}
)";

// Scene + bloom, exposure and flash, then a hue-preserving tone curve: the identity
// below the knee; above it the brightest channel rolls off and the others keep their
// ratio, so fire stays orange; only very hot light (> ~4) runs on to white.
const char* COMP_FS = R"(#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uScene, uBloom; uniform vec2 uBloomTexel;
uniform float uBloomStrength, uExposure, uKnee, uAspect;
uniform vec3 uFlashColor; uniform vec2 uFlashPos; uniform float uFlashRadius, uFlashGlobal;
uniform float uSaturation, uContrast, uVignette; uniform vec3 uLift;
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
void main() {
  vec3 c = texture(uScene, vUV).rgb;
  vec2 bt = uBloomTexel * 0.5;   // four bilinear taps: a tent over the half-resolution level
  vec3 b = texture(uBloom, vUV - bt).rgb + texture(uBloom, vUV + bt).rgb
         + texture(uBloom, vUV + vec2(bt.x, -bt.y)).rgb + texture(uBloom, vUV + vec2(-bt.x, bt.y)).rgb;
  c += b * (0.25 * uBloomStrength);
  vec2 fd = (vUV - uFlashPos) * vec2(uAspect, 1.0);
  c = c * uExposure + uFlashColor * (uFlashGlobal + exp(-dot(fd, fd) / max(uFlashRadius * uFlashRadius, 1e-4)));
  c = max(c, vec3(0.0));
  float m = max(c.r, max(c.g, c.b));
  if (m > uKnee) {
    float s = min((m - uKnee) / (1.0 - uKnee), 1e4);
    float mt = uKnee + (1.0 - uKnee) * s / (1.0 + s);
    c *= mt / m;
    c = mix(c, vec3(mt), clamp((m - 4.0) / 8.0, 0.0, 1.0));   // white-hot
  }
  c = c + uLift * (1.0 - c);                                    // lift tints the darks only
  c = max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, uSaturation), 0.0);
  c = clamp(c + uContrast * c * (1.0 - c) * (2.0 * c - 1.0), 0.0, 1.0);
  float r = length((vUV - 0.5) * vec2(uAspect, 1.0)) / length(vec2(0.5 * uAspect, 0.5));
  c *= 1.0 - uVignette * smoothstep(0.6, 1.1, r);
  c += (ign(gl_FragCoord.xy) - 0.5) / 255.0;                    // dither: no banding in fog and bloom tails
  o = vec4(c, 1.0);
}
)";

GLint U(GLuint p, const char* n) { return glGetUniformLocation(p, n); }

void texParams(GLenum filter) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
}  // namespace

bool PostFx::init() {
  progPre_ = compileProgram(FULL_VS, PRE_FS, "post prefilter");
  progDown_ = compileProgram(FULL_VS, DOWN_FS, "post down");
  progUp_ = compileProgram(FULL_VS, UP_FS, "post up");
  progComp_ = compileProgram(FULL_VS, COMP_FS, "post composite");
  if (!progPre_ || !progDown_ || !progUp_ || !progComp_) return false;
  glGenVertexArrays(1, &emptyVao_);
  return true;
}

void PostFx::release() {
  for (GLuint* f : {&msaaFbo_, &sceneFbo_}) if (*f) { glDeleteFramebuffers(1, f); *f = 0; }
  for (GLuint* r : {&msaaColor_, &msaaDepth_, &sceneDepth_}) if (*r) { glDeleteRenderbuffers(1, r); *r = 0; }
  if (sceneTex_) { glDeleteTextures(1, &sceneTex_); sceneTex_ = 0; }
  for (Level& l : bloom_) {
    if (l.fbo) glDeleteFramebuffers(1, &l.fbo);
    if (l.tex) glDeleteTextures(1, &l.tex);
    l = Level();
  }
  levels_ = 0;
}

// samples > 1: draw into multisampled renderbuffers, resolve into sceneTex_; else draw into sceneTex_ itself
bool PostFx::allocate(int w, int h, int samples, GLenum colorFmt) {
  release();
  auto complete = [](const char* what) {
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) logWarn("post: %s framebuffer incomplete (0x%x)", what, st);
    return st == GL_FRAMEBUFFER_COMPLETE;
  };
  glGenTextures(1, &sceneTex_);
  glBindTexture(GL_TEXTURE_2D, sceneTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, colorFmt, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
  texParams(GL_LINEAR);
  glGenFramebuffers(1, &sceneFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneTex_, 0);
  if (samples <= 1) {   // LOW: the scene is drawn here directly, so it needs its own depth
    glGenRenderbuffers(1, &sceneDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneDepth_);
  }
  bool ok = complete("scene");
  if (ok && samples > 1) {
    glGenRenderbuffers(1, &msaaColor_);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaColor_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, colorFmt, w, h);
    glGenRenderbuffers(1, &msaaDepth_);
    glBindRenderbuffer(GL_RENDERBUFFER, msaaDepth_);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
    glGenFramebuffers(1, &msaaFbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, msaaFbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaaColor_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msaaDepth_);
    ok = complete("multisampled scene");
  }
  // bloom: half resolution down to 6 levels (HIGH) or 4 (LOW), at least 8 px
  int bw = std::max(1, (w + 1) / 2), bh = std::max(1, (h + 1) / 2), maxLv = samples > 1 ? 6 : 4;
  for (int i = 0; ok && i < maxLv && std::min(bw, bh) >= 8; i++) {
    Level& l = bloom_[i];
    l.w = bw; l.h = bh;
    glGenTextures(1, &l.tex);
    glBindTexture(GL_TEXTURE_2D, l.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, bw, bh, 0, GL_RGB, GL_FLOAT, nullptr);
    texParams(GL_LINEAR);
    glGenFramebuffers(1, &l.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, l.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
    ok = complete("bloom");
    levels_ = i + 1;
    bw = std::max(1, bw / 2);
    bh = std::max(1, bh / 2);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (!ok || levels_ == 0) { release(); return false; }
  return true;
}

bool PostFx::ensure(int w, int h, bool msaa) {
  if (w < 16 || h < 16) return false;   // minimised or tiny: no post this frame
  int samples = msaa ? 4 : 1;
  if (w == w_ && h == h_ && samples == samples_) return !failed_;
  w_ = w; h_ = h; samples_ = samples;
  // R11G11B10F (32 bits: nothing reads the scene's alpha; ~0.7 ms less than RGBA16F at 1600x720 4x MSAA
  // on an RTX 3050 Ti, no banding seen at night); RGBA16F if the driver refuses it
  failed_ = false;
  if (allocate(w, h, samples, GL_R11F_G11F_B10F)) { fmtName_ = "R11G11B10F"; }
  else if (allocate(w, h, samples, GL_RGBA16F)) { fmtName_ = "RGBA16F"; }
  else { failed_ = true; logWarn("post: no HDR target at %dx%d (%d samples): drawing without post", w, h, samples); return false; }
  logInfo("post: %dx%d %s, %d sample%s, %d bloom levels", w, h, fmtName_, samples, samples > 1 ? "s" : "", levels_);
  return true;
}

void PostFx::finish(const PostParams& p, int vpX, int vpY, int vpW, int vpH, bool low) {
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);
  if (msaaFbo_) {   // resolve: same size, same format
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaaFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sceneFbo_);
    glBlitFramebuffer(0, 0, w_, h_, 0, 0, w_, h_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  glBindVertexArray(emptyVao_);
  glActiveTexture(GL_TEXTURE0);
  auto pass = [&](GLuint prog, const Level& dst, GLuint src, int srcW, int srcH) {
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glUseProgram(prog);
    glUniform1i(U(prog, "uSrc"), 0);
    glUniform2f(U(prog, "uTexel"), 1.f / srcW, 1.f / srcH);
    glBindTexture(GL_TEXTURE_2D, src);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  };
  // bloom: prefilter into level 0, down the chain, then back up adding each level into the larger one
  float T = p.bloomThreshold, K = std::max(1e-3f, p.bloomKnee);
  glUseProgram(progPre_);
  glUniform4f(U(progPre_, "uThr"), T, T - K, 2 * K, 0.25f / K);
  pass(progPre_, bloom_[0], sceneTex_, w_, h_);
  for (int i = 1; i < levels_; i++) pass(progDown_, bloom_[i], bloom_[i - 1].tex, bloom_[i - 1].w, bloom_[i - 1].h);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  for (int i = levels_ - 1; i >= 1; i--) pass(progUp_, bloom_[i - 1], bloom_[i].tex, bloom_[i].w, bloom_[i].h);
  glDisable(GL_BLEND);

  // composite into the default framebuffer, at the canvas (the HUD is drawn over it later)
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(vpX, vpY, vpW, vpH);
  glUseProgram(progComp_);
  GLuint c = progComp_;
  glUniform1i(U(c, "uScene"), 0);
  glUniform1i(U(c, "uBloom"), 1);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, sceneTex_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, bloom_[0].tex);
  glUniform2f(U(c, "uBloomTexel"), 1.f / bloom_[0].w, 1.f / bloom_[0].h);
  // the up chain sums every level into level 0: normalise by their number
  glUniform1f(U(c, "uBloomStrength"), p.bloomStrength / std::max(1, levels_ - 1) * (low ? 1.3f : 1.f));
  glUniform1f(U(c, "uExposure"), p.exposure);
  glUniform1f(U(c, "uKnee"), std::clamp(p.knee, 0.f, 0.99f));
  glUniform1f(U(c, "uAspect"), (float)vpW / std::max(1, vpH));
  glUniform3fv(U(c, "uFlashColor"), 1, glm::value_ptr(p.flashColor));
  glUniform2fv(U(c, "uFlashPos"), 1, glm::value_ptr(p.flashPos));
  glUniform1f(U(c, "uFlashRadius"), p.flashRadius);
  glUniform1f(U(c, "uFlashGlobal"), p.flashGlobal);
  glUniform1f(U(c, "uSaturation"), p.saturation);
  glUniform1f(U(c, "uContrast"), p.contrast);
  glUniform1f(U(c, "uVignette"), p.vignette);
  glUniform3fv(U(c, "uLift"), 1, glm::value_ptr(p.lift));
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glUseProgram(0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glDepthMask(GL_TRUE);
}
