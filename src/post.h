// Post-processing: the 3D scene renders into an HDR target (RGBA16F, 4x MSAA on
// GFX HIGH), then a bloom chain and a composite pass (tone curve, exposure, a
// radial flash, grading, vignette, dither) write the canvas into the default
// framebuffer. The ImGui HUD is drawn afterwards, untouched.
//
// The scene stays display-referred (lighting as today, lit surfaces clamped at
// 1): only emitters — fire, tracers, lamps, additive sprite stacks — exceed 1.0,
// and only they bloom. The tone curve is the identity below its knee.
#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>

struct PostParams {
  bool enabled = true;
  float exposure = 1, knee = 0.9f;                 // tone curve: identity below the knee, shoulder above
  float bloomStrength = 0.6f, bloomThreshold = 1.0f, bloomKnee = 0.15f;
  glm::vec3 flashColor{0};                         // a radial flash (the nuke's), added before the tone curve
  glm::vec2 flashPos{0.5f};                        // canvas uv
  float flashRadius = 0.35f, flashGlobal = 0;
  float saturation = 1, contrast = 0, vignette = 0;
  glm::vec3 lift{0};
};

class PostFx {
 public:
  bool init();                                     // programs; false: post off for this run
  // (Re)allocates the targets when the canvas size or the sample count changes; false: post off this frame
  bool ensure(int w, int h, bool msaa);
  GLuint drawFbo() const { return msaaFbo_ ? msaaFbo_ : sceneFbo_; }   // where the scene is drawn
  int width() const { return w_; }
  int height() const { return h_; }
  // Resolves the scene, runs the bloom chain and composites into framebuffer 0 at the canvas viewport
  void finish(const PostParams& p, int vpX, int vpY, int vpW, int vpH, bool low);
  const char* format() const { return fmtName_; }

 private:
  int w_ = 0, h_ = 0, samples_ = -1;
  bool failed_ = false;                            // an allocation failed at this size: do not retry every frame
  GLuint msaaFbo_ = 0, msaaColor_ = 0, msaaDepth_ = 0;   // HIGH: multisampled renderbuffers
  GLuint sceneFbo_ = 0, sceneTex_ = 0, sceneDepth_ = 0;  // resolve target (HIGH) or the render target itself (LOW)
  struct Level { GLuint fbo = 0, tex = 0; int w = 0, h = 0; };
  Level bloom_[6];
  int levels_ = 0;
  GLuint progPre_ = 0, progDown_ = 0, progUp_ = 0, progComp_ = 0, emptyVao_ = 0;
  const char* fmtName_ = "";
  void release();
  bool allocate(int w, int h, int samples, GLenum colorFmt);
};
