// Asset loading: DDS / PNG / JPG textures, OBJ and GLB (KHR_mesh_quantization)
// models with the web version's normalisation, baked walk / attack frames.
// Files are parsed on worker threads; only the GPU upload runs on the GL thread.
#pragma once
#include <string>
#include <vector>

#include "config.h"
#include "render.h"

bool findAssetRoot();                              // locates the folder that holds assets/
std::string assetPath(const std::string& rel);     // "assets/…" → absolute path
const Texture* loadTexture(const std::string& rel);   // cached; nullptr on failure (logged)
void prefetchTexture(const std::string& rel);         // decode on a worker now; loadTexture then only uploads

struct ModelPart {
  std::string name;
  Mesh* mesh = nullptr;
  Material mat;
  Geometry cpu;   // model space (centred and scaled): raycasts, bounds, hologram edges
};

// A loaded model. Its vertices are centred on the bind pose's box and scaled
// (k); wrapper() places it as the web version's wrapper group does: standing on
// the ground (defaultPos.y) and turned by the asset's rotY.
struct Model {
  const AssetDef* def = nullptr;
  std::vector<ModelPart> parts;
  glm::vec3 center{0}, size{0};   // raw bind-pose box centre · scaled size
  float k = 1;
  glm::vec3 defaultPos{0};
  float defaultRotY = 0;
  std::vector<Mesh*> walk, attack;   // baked frames, drawn with parts[0].mat
  glm::mat4 wrapper() const;
  glm::vec3 local(const glm::vec3& raw) const;   // a raw model-unit point → wrapper space
};

namespace models {
// Every asset is parsed and its textures decoded on the worker pool (jobs.h),
// LOAD_STAGES first; the GL thread then creates its meshes and textures.
void startLoading();
const char* loadNext();                 // GL thread: finishes one prepared asset; its key, nullptr if none is ready
bool allSettled();                      // every asset loaded or failed
const Model* get(const char* key);      // nullptr until loaded (or after a failure)
bool isReady(const char* key);
bool isSettled(const char* key);        // loaded or failed
void progress(int& done, int& total);   // asset files, as the web title screen counts them
const AssetDef* def(const char* key);
}

// three.js EdgesGeometry: segments where the faces meet at more than
// thresholdDeg, plus open edges. Output: pairs of endpoints, transformed by m.
std::vector<glm::vec3> featureEdges(const Geometry& g, const glm::mat4& m, float thresholdDeg);
