// The nuke's blast, drawn with its own shaders as the renderer's custom draws: a white-hot
// fireball that rises, flattens and burns out into the mushroom cloud's sprites; the shock
// front (a ragged ring of light racing over the ground under a fading shock dome); and the
// scar it leaves (a burn with glowing cracks that cool to black, then fade). The base surge
// (a ring of dust with a burning leading edge) is the cloud's sprites (game.cpp).
// The timeline is closed-form curves (nk::), so every view and every frame reads the same blast.
#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

class Renderer;
struct Geometry;

namespace nk {   // t = seconds since detonation (game time)
inline float ss(float a, float b, float x) {
  float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
  return t * t * (3 - 2 * t);
}
// shock front: 33 u/s at the blast, 15 u/s as it passes the kill radius (16 at 0.69 s)
constexpr float S_MAX = 30, S_TAU = 0.9f;
inline float frontR(float t) { return t <= 0 ? 0 : S_MAX * (1 - std::exp(-t / S_TAU)); }
inline float frontT(float r) { return -S_TAU * std::log(1 - std::min(r, S_MAX - 0.01f) / S_MAX); }
// base surge: rolls out just behind the front, past the kill radius at ~1.3 s, stalls near 22
inline float surgeR(float t) { return std::max(0.f, std::min(frontR(t) - 0.6f, 1.5f + 21 * (1 - std::exp(-t / 1.2f)))); }
inline float surgeH(float t) { return (0.5f + 3.1f * (1 - std::exp(-t / 0.7f))) * (1 - 0.5f * ss(2, 6, t)); }
// fireball: a white-hot hemisphere (r 2.6 at 0.1 s) that lifts off, cools and flattens
inline float fireR(float t) { return 3.3f * std::sqrt(1 - std::exp(-t / 0.16f)) + 0.12f * t; }
inline float fireY(float t) { return 0.42f * fireR(t) + 4.6f * (1 - std::exp(-std::max(0.f, t - 0.3f) / 1.2f)); }
inline float heat(float t) { return std::exp(-t / 2.4f); }
// the flash: up in 25 ms, mostly gone by 0.3 s
inline float flashK(float t) { return t < 0 ? 0 : t < 0.025f ? t / 0.025f : std::exp(-(t - 0.025f) / 0.09f); }
// exposure: x6 in the flash, then dazzled eyes (x0.8 at 0.45-0.9 s), back to 1 by 3 s
inline float exposure(float t) {
  float dip = 0.2f * ss(0.15f, 0.45f, t) * (1 - ss(0.9f, 3, t));
  return (1 + 5 * flashK(t)) * (1 - dip);
}
constexpr float FIRE_END = 3.8f;    // the fireball has burnt out into the cloud
constexpr float FRONT_END = 2.0f;   // the front ring has faded out past the screen
constexpr float SURGE_END = 6.5f;   // the surge (the cloud's sprites: game.cpp) has settled
constexpr float SCAR_END = 16;      // the scar has faded
}

// The GPU side: programs and meshes (one set for every blast)
class NukeRenderer {
 public:
  bool init();   // false: no effect programs (the sprite cloud still shows the blast)
  bool ok() const { return ok_; }
  struct Blast {
    glm::vec3 p{0};      // ground zero
    float t = 0, seed = 0;
    bool low = false;    // GFX LOW: fewer noise octaves
  };
  void draw(Renderer& R, const Blast& b) const;   // queues the fireball, the front and the dome
  void drawScar(Renderer& R, glm::vec2 p, float age, float seed, float front) const;   // front: the shock front's radius (cracks open behind it)

 private:
  struct FxMesh { GLuint vao = 0, vbo = 0, ibo = 0; GLsizei count = 0; };
  bool ok_ = false;
  GLuint progScar_ = 0, progFront_ = 0, progDome_ = 0, progFire_ = 0;
  FxMesh quad_, ring_, dome_, ball_;
  static FxMesh build(const Geometry& g);
};
