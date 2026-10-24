// Particles simulated on the GPU (OpenGL 4.3 compute shaders): the CPU only
// queues bursts (where, how many, colours, speeds); a compute pass creates the
// particles from a hash RNG into a ring buffer, a second one moves them
// (gravity, drag, bouncing on the ground), and the vertex shader reads them
// straight from the buffer. Tens of thousands of particles cost the CPU nothing.
#pragma once
#include <deque>
#include <vector>

#include "render.h"

class GpuParticles {
 public:
  enum Shape { Sphere = 0, Ring = 1, Column = 2, Box = 3 };
  struct Burst {
    glm::vec3 pos{0};
    int count = 0;
    glm::vec3 vel{0};      // base velocity
    float speed = 0;       // Sphere / Ring / Column: speed along the random direction (25–100%)
    glm::vec3 box{0};      // Box: velocity spread (x, z: ±; y: 0..)
    float radius = 0;      // spawn positions within this distance of pos
    uint32_t c0 = 0xffffff, c1 = 0xffffff;
    float a0 = 1;
    float life0 = .4f, life1 = .7f;
    float s0 = .2f, s1 = .08f;   // size at birth / death (each particle ×0.6–1.4)
    float grav = 0, drag = 0, up = 0;   // up: extra upward speed ×(0.5–1.5)
    Shape shape = Sphere;
    bool smoke = false;    // normal blending (else additive glow)
    bool fade = false;     // fade in then out (smoke) instead of out only
  };
  // false when the context has no compute shaders (logged); the game then keeps its CPU particles
  bool init(int glowCap, int smokeCap);
  bool ok() const { return ok_; }
  void burst(const Burst& b);
  void update(float dt);   // spawn this frame's bursts, then move every particle (dt 0 = paused: spawn only)
  // inside the scene's transparent pass; glPos / glCol: the small lights (SceneParams::glows) that lift the night fog
  void draw(const glm::mat4& view, const glm::mat4& proj, float fogNear, float fogFar, const glm::vec4* glPos, const glm::vec4* glCol, int glN);
  void clear();
  int live() const;        // upper bound: particles spawned within the longest lifetime

 private:
  struct Sys {
    GLuint buf = 0;
    int cap = 0;
    uint64_t head = 0;     // total spawned (ring position = head % cap)
    float maxLife = 0;
    std::deque<std::pair<double, uint64_t>> history;   // (sim time, head) per update: what may still be alive
    std::vector<float> pending;                        // packed bursts for the next spawn pass
    int pendingCount = 0;
    uint64_t oldestLive(double now) const;
  };
  Sys glow_, smoke_;
  bool ok_ = false;
  double simT_ = 0;
  GLuint spawnProg_ = 0, updateProg_ = 0, drawProg_ = 0, burstBuf_ = 0, vao_ = 0, quad_ = 0;
  void spawn(Sys& s);
  void drawSys(const Sys& s, const glm::mat4& view, const glm::mat4& proj, float fogNear, float fogFar, const glm::vec4* glPos, const glm::vec4* glCol,
               int glN, bool additive);
};
