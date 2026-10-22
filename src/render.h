// OpenGL 3.3 renderer: geometry, textures, the three.js-style materials the web
// version uses (Standard / Lambert / Phong / Basic with SC2 team colour and
// DXT5nm normal maps), sun shadow map, radial fog, instancing and particles.
#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

inline glm::vec3 hexColor(uint32_t h) {
  return {((h >> 16) & 255) / 255.f, ((h >> 8) & 255) / 255.f, (h & 255) / 255.f};
}

struct Vertex {
  glm::vec3 p{0};
  glm::vec3 n{0, 1, 0};
  glm::vec2 uv{0};
};

// CPU triangle list; idx empty = non-indexed
struct Geometry {
  std::vector<Vertex> v;
  std::vector<uint32_t> idx;
  Geometry& transform(const glm::mat4& m);   // positions by m, normals by its normal matrix
  Geometry nonIndexed() const;
  void computeFlatNormals();                 // non-indexed: one normal per triangle
  void append(const Geometry& g);            // both non-indexed
  bool bounds(glm::vec3& mn, glm::vec3& mx) const;
  size_t triCount() const { return (idx.empty() ? v.size() : idx.size()) / 3; }
  glm::vec3 corner(size_t tri, int k) const { return v[idx.empty() ? tri * 3 + k : idx[tri * 3 + k]].p; }
};

// three.js r128 primitive shapes (same parameters, same axes)
namespace geo {
Geometry box(float w, float h, float d);
Geometry sphere(float r, int ws = 32, int hs = 16);
Geometry cylinder(float rTop, float rBottom, float h, int radial = 8);
Geometry cone(float r, float h, int radial = 8);
Geometry ring(float inner, float outer, int seg = 32);   // XY plane, normal +Z
Geometry plane(float w, float h);                          // XY plane, normal +Z
Geometry dodecahedron(float r);                            // flat shaded
Geometry icosahedron(float r, int detail);                 // smooth
}

struct Mesh {
  GLuint vao = 0, vbo = 0, ibo = 0;
  GLsizei count = 0;
  bool indexed = false;
  glm::vec3 bmin{0}, bmax{0};
  static Mesh* create(const Geometry& g);   // owned by the renderer's arena for the program's lifetime
};

struct Texture {
  GLuint id = 0;
  int w = 0, h = 0;
};

enum class MatType { Standard, Lambert, Phong, Basic };
enum class Blend { Normal, Additive };

struct Material {
  MatType type = MatType::Standard;
  glm::vec3 color{1};
  float opacity = 1;
  float roughness = 1, metalness = 0;     // Standard
  glm::vec3 emissive{0};
  glm::vec3 specular{0.0667f};            // Phong
  float shininess = 30;
  const Texture* map = nullptr;
  const Texture* emissiveMap = nullptr;
  const Texture* normalMap = nullptr;     // SC2 DXT5nm: X in alpha, Y in green
  glm::vec2 normalScale{1, 1};
  bool teamColorOn = false;               // SC2 team-colour mask in the diffuse alpha
  glm::vec3 teamColor{1};
  bool transparent = false;
  Blend blend = Blend::Normal;
  bool depthWrite = true;
  bool doubleSide = false;
};

struct Instance {
  glm::mat4 m{1};
  glm::vec3 c{1};
};

struct DirLight {
  glm::vec3 pos{0, 1, 0};   // shines from pos toward the origin
  glm::vec3 color{1};
  float intensity = 0;
};
struct PointLight {
  glm::vec3 pos{0};
  glm::vec3 color{1};
  float intensity = 0, distance = 0, decay = 1;
};

struct SceneParams {
  glm::mat4 view{1}, proj{1};
  glm::vec3 camPos{0};
  glm::vec3 ambientColor{1};
  float ambientIntensity = 0;
  DirLight sun, fill;
  PointLight points[3];
  glm::vec3 fogColor{0};
  float fogNear = 1, fogFar = 100;
  bool shadows = true, msaa = true;
  int vpX = 0, vpY = 0, vpW = 1, vpH = 1;   // framebuffer pixels, origin bottom-left
};

// Soft round sprites with per-particle size / colour / alpha (the web version's
// THREE.Points systems). Swap-remove keeps the arrays packed.
struct ParticleSystem {
  struct P {
    glm::vec3 pos, vel, c0, c1, col;
    float size, alpha, life, maxLife, grav, drag, s0, s1, a0;
    uint8_t fade;
  };
  int cap = 0;
  std::vector<P> p;
  void init(int capacity) { cap = capacity; p.reserve(capacity); }
  // fade: 0 = linear fade-out, 1 = fade in then out (smoke)
  void emit(float x, float y, float z, float vx, float vy, float vz, float life, float s0, float s1, float a0,
            uint32_t c0, uint32_t c1, float grav, float drag, int fade);
  void update(float dt);
  void clear() { p.clear(); }
};

class Renderer {
 public:
  bool init();
  void begin();   // drops last frame's draw list
  void draw(const Mesh* mesh, const Material* mat, const glm::mat4& model, bool castShadow, bool receiveShadow,
            const glm::vec3& color = glm::vec3(1), int renderOrder = 0);
  void drawInstanced(const Mesh* mesh, const Material* mat, const Instance* inst, int n, bool castShadow,
                     bool receiveShadow, int renderOrder = 0);
  void drawParticles(const ParticleSystem* ps, Blend blend, int renderOrder);
  void render(const SceneParams& sp);
  int drawCount() const { return (int)cmds_.size(); }
  void setGpuParticles(class GpuParticles* g) { gpuParticles_ = g; }   // drawn after the CPU particles (null: none)
  // Pixel readback of the default framebuffer (screenshots for testing)
  static bool saveScreenshot(const std::string& path, int w, int h);

 private:
  struct Cmd {
    const Mesh* mesh;
    const Material* mat;
    glm::mat4 model;
    int first, count;
    bool cast, receive;
    int order;
    const ParticleSystem* ps;
    Blend psBlend;
  };
  std::vector<Cmd> cmds_;
  std::vector<Instance> inst_;
  GLuint litProg_ = 0, depthProg_ = 0, partProg_ = 0;
  GLuint instVbo_ = 0, shadowFbo_ = 0, shadowTex_ = 0;
  GLuint partVao_ = 0, partQuadVbo_ = 0, partInstVbo_ = 0;
  GLuint whiteTex_ = 0;
  class GpuParticles* gpuParticles_ = nullptr;
  void bindInstances(int first);
  void setMaterial(const Material& m, bool receive, bool shadowsOn);
};

GLuint compileProgram(const char* vs, const char* fs, const char* name);
