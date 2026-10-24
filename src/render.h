// OpenGL 3.3 renderer: geometry, textures, the three.js-style materials the web
// version uses (Standard / Lambert / Phong / Basic with SC2 team colour and
// DXT5nm normal maps), sun shadow map, radial fog, instancing and particles.
#pragma once
#include <glad/gl.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "post.h"

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
  // RGBA8 rows bottom-up, mipmapped; owned like meshes. repeat: tiles (else clamped); aniso: anisotropic filtering (1 = off)
  static const Texture* create(int w, int h, const uint8_t* rgba, bool repeat = false, float aniso = 1);
};

enum class MatType { Standard, Lambert, Phong, Basic, Terrain };   // Terrain: the ground, drawn procedurally (LIT_FS, TERRAIN)
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
  bool fog = true;                        // false: self-lit (tracers, fireballs, blips) — shows through the night fog
  bool scenery = false;                   // rocks and the like: at night the fog leaves their own moonlit, colourless trace
  bool instAlpha = false;                 // soft decals: the instance colour's red is the alpha (the colour is the material's)
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
// A cone of light (three.js SpotLight's falloff): full inside cosInner, none past cosOuter
struct SpotLight {
  glm::vec3 pos{0}, dir{0, -1, 0};
  glm::vec3 color{1};
  float intensity = 0, distance = 0, decay = 1, cosOuter = 0.8f, cosInner = 0.9f;
};

// A small weak light that also lifts the night fog around it (a Marauder grenade in flight, its
// burst): lights what it falls on and shows it in the dark, out to `radius`
struct GlowLight {
  glm::vec3 pos{0}, color{0};
  float radius = 0, reveal = 0;   // reveal: how much of the night it lifts near it (0..1)
};
constexpr int MAX_GLOWS = 8;

struct SceneParams {
  glm::mat4 view{1}, proj{1};
  glm::vec3 camPos{0};
  glm::vec3 ambientColor{1};
  float ambientIntensity = 0;
  DirLight sun, fill;
  PointLight points[4];
  SpotLight spot;   // a crate runner's chest lamps at night
  GlowLight glows[MAX_GLOWS];
  int glowCount = 0;
  glm::vec3 fogColor{0};
  float fogNear = 1, fogFar = 100;
  bool shadows = true, msaa = true;
  int vpX = 0, vpY = 0, vpW = 1, vpH = 1;   // framebuffer pixels, origin bottom-left
  PostParams post;                          // HDR target, bloom, tone curve (post.h)
  // night: how much of the fog is darkness (the field fades to its moonlit value, not to the fog colour),
  // how much colour the ground and scenery lose; the ground's noise texture (TERRAIN); close-up detail
  float night = 0, nightDesat = 0;
  glm::vec3 moonTint{0.20f, 0.27f, 0.42f};
  const Texture* terrainTex = nullptr;
  bool hiDetail = false;
};

// Soft round sprites with per-particle size / colour / alpha (the web version's
// THREE.Points systems). Swap-remove keeps the arrays packed.
struct ParticleSystem {
  struct P {
    glm::vec3 pos, vel, c0, c1, col;
    float size, alpha, life, maxLife, grav, drag, s0, s1, a0;
    uint8_t fade;
    float rot = 0;   // textured puffs (tex): the atlas cell x 8 + the sprite's turn in radians; < 0: a plain soft disc
  };
  int cap = 0;
  const Texture* tex = nullptr;   // a 2x2 puff atlas (R density, GB normal, A detail): soft billows, gently lit by the sun
  float cover = 0;   // additive systems: how much of what lies behind a glow it also hides (0: pure light;
                     // some cover warms the grass instead of tinting it lime)
  std::vector<P> p;
  void init(int capacity) { cap = capacity; p.reserve(capacity); }
  // fade: 0 = linear fade-out, 1 = fade in then out (smoke)
  void emit(float x, float y, float z, float vx, float vy, float vz, float life, float s0, float s1, float a0,
            uint32_t c0, uint32_t c1, float grav, float drag, int fade);
  void update(float dt);
  void clear() { p.clear(); }
};

// What a custom draw (an effect with its own shaders: nuke_fx.cpp) gets of the frame
struct FrameCtx {
  glm::mat4 view{1}, proj{1};
  glm::vec3 camPos{0}, fogColor{0};
  float fogNear = 1, fogFar = 100;
  bool hdr = false;   // the scene goes to the HDR target (emitters may exceed 1)
};
using CustomDraw = std::function<void(const FrameCtx&)>;

class Renderer {
 public:
  bool init();
  void begin();   // drops last frame's draw list
  void draw(const Mesh* mesh, const Material* mat, const glm::mat4& model, bool castShadow, bool receiveShadow,
            const glm::vec3& color = glm::vec3(1), int renderOrder = 0);
  void drawInstanced(const Mesh* mesh, const Material* mat, const Instance* inst, int n, bool castShadow,
                     bool receiveShadow, int renderOrder = 0);
  void drawParticles(const ParticleSystem* ps, Blend blend, int renderOrder);
  // An effect with its own program, run in the transparent pass by order, then back to front at
  // sortAt. It sets its own blend, depth mask and cull; it leaves the depth test on (LEQUAL),
  // texture unit 3 (the shadow map) alone and VAO 0 bound.
  void drawCustom(int renderOrder, const glm::vec3& sortAt, CustomDraw fn);
  void render(const SceneParams& sp);
  int drawCount() const { return (int)cmds_.size(); }
  void setGpuParticles(class GpuParticles* g) { gpuParticles_ = g; }   // drawn after the CPU particles (null: none)
  // Pixel readback of the default framebuffer (screenshots for testing)
  static bool saveScreenshot(const std::string& path, int w, int h);
  bool postActive() const { return postActive_; }   // the last frame went through the HDR post passes

 private:
  PostFx post_;
  bool postOk_ = false, postActive_ = false;
  struct Cmd {
    const Mesh* mesh;
    const Material* mat;
    glm::mat4 model;
    int first, count;
    bool cast, receive;
    int order;
    const ParticleSystem* ps;
    Blend psBlend;
    int custom = -1;   // index into customs_
  };
  std::vector<Cmd> cmds_;
  struct Custom { glm::vec3 at; CustomDraw fn; };
  std::vector<Custom> customs_;
  std::vector<Instance> inst_;
  GLuint litProg_ = 0, terrainProg_ = 0, depthProg_ = 0, partProg_ = 0;   // terrainProg_: LIT_FS with TERRAIN (0: the ground falls back to Phong)
  GLuint instVbo_ = 0, shadowFbo_ = 0, shadowTex_ = 0;
  GLuint partVao_ = 0, partQuadVbo_ = 0, partInstVbo_ = 0;
  GLuint whiteTex_ = 0;
  class GpuParticles* gpuParticles_ = nullptr;
  void bindInstances(int first);
  void setMaterial(GLuint prog, const Material& m, bool receive, bool shadowsOn);
};

// defines: lines put after each source's #version line (a shader variant)
GLuint compileProgram(const char* vs, const char* fs, const char* name, const char* defines = nullptr);
