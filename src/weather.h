// The weather. The sky wanders at random between clear, clouded over and rain, and the ground
// follows it: it wets while it rains, puddles fill in the low ground with the rain that has
// fallen, and both seep away after. Weather (the state) is plain logic run on game time;
// WeatherRenderer draws the rain, falling streaks and splashes on the ground, made in the vertex
// shader from each drop's index (no buffers, nothing simulated on the CPU). The ground's side
// (wet soil, puddles, the rain's rings on them, what the water mirrors) is the lit shader's
// TERRAIN variant (render.cpp); the light under the clouds, Game::applyDNLights.
#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <random>

class Renderer;

struct Weather {
  enum class Sky { Clear, Overcast, Rain };
  Sky sky = Sky::Clear;
  float hold = 0;              // seconds left before the sky changes
  bool afterRain = false;      // this overcast spell follows a shower (it clears more often than it rains again)
  float showerMax = 0;         // how hard it rains now: a drizzle (0.15-0.35) or a downpour (0.7-1)
  float spell = 0;             // seconds before a shower may turn from one to the other
  float cloud = 0, rain = 0;   // now: cloud cover and rain, eased toward the sky's state (0..1)
  float wet = 0, puddle = 0;   // the ground: how wet it is, how full its puddles are (0..1)
  glm::vec2 wind{0};           // this shower's wind, m/s (the drops slant with it, the clouds drift with it)
  glm::vec2 drift{0};          // how far the cloud deck has drifted (the clouds the puddles mirror)
  glm::vec2 blown{0};          // how far the wind has carried the drops (m)
  double fall = 0;             // how far the drops have fallen (m): a drizzle falls slowly, a downpour fast
  double t = 0;                // its clock, seconds of game time: the splashes, the rain's rings
  bool held = false;           // a test state: the sky stays put

  void reset(std::mt19937& rng);                        // a new game: clear and dry, the first change a while off
  void update(float dt, std::mt19937& rng, bool on);    // off: the sky clears and the ground dries, faster
  void set(Sky s, std::mt19937& rng, bool instant);     // the rain cheat, the test option (instant: no easing in)
  void pour(bool heavy, std::mt19937& rng);             // the shower turns into a downpour, or eases to a drizzle
  bool heavy() const { return showerMax >= 0.5f; }
  float fallSpeed() const { return 5 + 7 * rain; }      // m/s: a drizzle's fine drops ~6, a downpour's 12
  const char* name() const;                             // "CLEAR", "OVERCAST", "DRIZZLE", "DOWNPOUR" (the log)
};

// The rain: streaks falling through a box over the field (fixed in the world, so they do not
// swim as the camera turns) and small rings where drops hit the ground
class WeatherRenderer {
 public:
  bool init();   // false: no rain drawn (the ground still wets)
  struct Look {
    glm::vec3 light{0.6f};                 // what lights the drops: the sky, the sun through the clouds, the moon
    glm::vec3 lampPos{0}, lampCol{0};      // the bunker's light at night: the drops near it glint warm
    float lampR = 1;
  };
  void draw(Renderer& R, const Weather& w, const Look& look) const;

 private:
  GLuint prog_ = 0, vao_ = 0;
};
