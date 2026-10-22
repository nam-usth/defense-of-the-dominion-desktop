#include "assets.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <deque>
#include <future>
#include <mutex>

#include <glm/gtc/matrix_transform.hpp>

#include "cgltf.h"
#include "jobs.h"
#include "log.h"
#include "stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

// ─── Asset root ──────────────────────────────────────────────────────────────
static fs::path g_root;

static fs::path exeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#endif
  return fs::current_path();
}

bool findAssetRoot() {
  std::vector<fs::path> starts = {exeDir(), fs::current_path()};
  for (fs::path p : starts) {
    for (int up = 0; up < 4 && !p.empty(); up++) {
      if (fs::exists(p / "assets" / "models" / "terran" / "bunker.obj")) {
        g_root = p;
        logInfo("assets: %s", (p / "assets").string().c_str());
        return true;
      }
      if (!p.has_parent_path() || p.parent_path() == p) break;
      p = p.parent_path();
    }
  }
  logError("assets folder not found next to the executable or in the working directory");
  return false;
}

std::string assetPath(const std::string& rel) { return (g_root / fs::path(rel)).string(); }

static bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  if (n < 0) return false;
  f.seekg(0);
  out.resize((size_t)n);
  f.read((char*)out.data(), n);
  return (bool)f;
}

// ─── Textures ────────────────────────────────────────────────────────────────
// Two steps: decode (any thread: file read, DDS header, PNG / JPG decode) and
// upload (GL thread). Decoded images are cached per path, so skins sharing a
// texture decode it once even when their loads run in parallel.
struct ImageData {
  bool ok = false;
  std::string err;
  bool dds = false;
  GLenum fmt = 0;                  // DDS: compressed format
  int w = 0, h = 0;
  std::vector<uint8_t> bytes;      // DDS file / RGBA8 pixels
  std::vector<std::pair<size_t, size_t>> levels;   // DDS mip levels: offset, size
};
using ImageP = std::shared_ptr<const ImageData>;

static std::mutex g_texMx;
static std::unordered_map<std::string, std::shared_future<ImageP>> g_decoded;
static std::unordered_map<std::string, std::unique_ptr<Texture>> g_textures;   // GL thread only
static std::set<std::string> g_texFailed;                                       // GL thread only
static std::mutex g_progressMx;
static std::set<std::string> g_filesDone;   // asset files read or failed (progress)
static void fileDone(const std::string& f) {
  std::lock_guard<std::mutex> lk(g_progressMx);
  g_filesDone.insert(f);
}

#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

static ImageP decodeImage(const std::string& rel) {
  auto img = std::make_shared<ImageData>();
  std::vector<uint8_t>& d = img->bytes;
  if (!readFile(assetPath(rel), d)) { img->err = "cannot read file"; fileDone(rel); return img; }
  std::string ext = fs::path(rel).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".dds") {
    // DDS rows are top-down and uploaded as they are; the shaders sample at
    // v' = 1 - v, as the web version's DDS texture transform does
    img->dds = true;
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) { img->err = "not a DDS file"; fileDone(rel); return img; }
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, d.data() + o, 4); return v; };
    uint32_t h = u32(12), w = u32(16), mips = std::max<uint32_t>(1, u32(28)), pfFlags = u32(80);
    char four[5] = {0};
    memcpy(four, d.data() + 84, 4);
    uint32_t block = 0;
    if (pfFlags & 4) {
      if (!strcmp(four, "DXT1")) { img->fmt = GL_COMPRESSED_RGB_S3TC_DXT1_EXT; block = 8; }   // RGB: alpha reads as 1, as three's DDSLoader
      else if (!strcmp(four, "DXT3")) { img->fmt = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; block = 16; }
      else if (!strcmp(four, "DXT5")) { img->fmt = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; block = 16; }
    }
    if (!img->fmt) { img->err = std::string("unsupported DDS format \"") + four + "\""; fileDone(rel); return img; }
    size_t off = 128;
    uint32_t mw = w, mh = h;
    for (uint32_t level = 0; level < mips; level++) {
      size_t sz = (size_t)std::max(1u, (mw + 3) / 4) * std::max(1u, (mh + 3) / 4) * block;
      if (off + sz > d.size()) break;   // truncated chain: keep the levels that are there
      img->levels.push_back({off, sz});
      off += sz;
      mw = std::max(1u, mw / 2);
      mh = std::max(1u, mh / 2);
    }
    if (img->levels.empty()) { img->err = "DDS data truncated"; fileDone(rel); return img; }
    img->w = (int)w;
    img->h = (int)h;
  } else {   // PNG / JPG: rows top-down like DDS (the shader flip then matches three's flipY)
    int w, h, n;
    unsigned char* px = stbi_load_from_memory(d.data(), (int)d.size(), &w, &h, &n, 4);
    if (!px) { img->err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed"; fileDone(rel); return img; }
    img->bytes.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    img->w = w;
    img->h = h;
  }
  img->ok = true;
  fileDone(rel);
  return img;
}

// decoded once per path, by whichever thread asks first; the others wait for it
static ImageP decodeCached(const std::string& rel) {
  std::shared_future<ImageP> f;
  std::promise<ImageP> mine;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lk(g_texMx);
    auto it = g_decoded.find(rel);
    if (it != g_decoded.end()) f = it->second;
    else { f = mine.get_future().share(); g_decoded[rel] = f; owner = true; }
  }
  if (owner) mine.set_value(decodeImage(rel));
  return f.get();
}

// GL thread
static const Texture* uploadImage(const std::string& rel, const ImageP& img) {
  auto it = g_textures.find(rel);
  if (it != g_textures.end()) return it->second.get();
  if (g_texFailed.count(rel)) return nullptr;
  if (!img || !img->ok) {
    logWarn("texture \"%s\" failed to load (%s)", rel.c_str(), img ? img->err.c_str() : "not decoded");
    g_texFailed.insert(rel);
    return nullptr;
  }
  auto t = std::make_unique<Texture>();
  glGenTextures(1, &t->id);
  glBindTexture(GL_TEXTURE_2D, t->id);
  if (img->dds) {
    int mw = img->w, mh = img->h;
    for (size_t l = 0; l < img->levels.size(); l++) {
      glCompressedTexImage2D(GL_TEXTURE_2D, (GLint)l, img->fmt, mw, mh, 0, (GLsizei)img->levels[l].second, img->bytes.data() + img->levels[l].first);
      mw = std::max(1, mw / 2);
      mh = std::max(1, mh / 2);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)img->levels.size() - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, img->levels.size() > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  } else {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->bytes.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  t->w = img->w;
  t->h = img->h;
  return (g_textures[rel] = std::move(t)).get();
}

void prefetchTexture(const std::string& rel) {
  jobs::submit([rel] { decodeCached(rel); });
}

// GL thread: decode (or reuse a worker's decode) and upload
const Texture* loadTexture(const std::string& rel) {
  auto it = g_textures.find(rel);
  if (it != g_textures.end()) return it->second.get();
  if (g_texFailed.count(rel)) return nullptr;
  return uploadImage(rel, decodeCached(rel));
}

// ─── OBJ (as js/lib/obj-loader.js: o/g start a mesh, usemtl ignored, fan
//     triangulation, the file's normals when every corner has one) ─────────────
struct RawPart {
  std::string name;
  Geometry g;
};

static bool parseObj(const std::string& path, std::vector<RawPart>& out, std::string& err) {
  // hand-rolled scanner over the whole file (the stringstream version took ~190 ms on the bunker)
  std::vector<uint8_t> buf;
  if (!readFile(path, buf)) { err = "cannot read file"; return false; }
  buf.push_back(0);
  const char* p = (const char*)buf.data();
  std::vector<glm::vec3> verts, normals;
  std::vector<glm::vec2> uvs;
  std::string name;
  bool open = false;
  Geometry cur;
  bool allN = true;
  auto finish = [&]() {
    if (!open || cur.v.empty()) return;
    if (!allN) cur.computeFlatNormals();   // the file's normals only when every corner has one
    out.push_back({name, std::move(cur)});
    cur = Geometry();
    allN = true;
  };
  auto skipWs = [&]() { while (*p == ' ' || *p == '\t') p++; };
  auto nextLine = [&]() { while (*p && *p != '\n') p++; if (*p) p++; };
  auto flt = [&]() { skipWs(); char* e; float v = std::strtof(p, &e); p = e; return v; };
  struct FV { int v, vt, vn; };
  std::vector<FV> fv;
  while (*p) {
    skipWs();
    if (p[0] == 'v' && p[1] == ' ') { p += 2; float x = flt(), y = flt(), z = flt(); verts.push_back({x, y, z}); }
    else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') { p += 3; float x = flt(), y = flt(), z = flt(); normals.push_back({x, y, z}); }
    else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') { p += 3; float x = flt(), y = flt(); uvs.push_back({x, y}); }
    else if ((p[0] == 'o' || p[0] == 'g') && (p[1] == ' ' || p[1] == '\t')) {
      finish();
      p += 2;
      skipWs();
      const char* s = p;
      while (*p && *p != '\n' && *p != '\r') p++;
      const char* e = p;
      while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
      name.assign(s, e);
      open = true;
      cur = Geometry();
      allN = true;
    } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
      p += 2;
      open = true;
      fv.clear();
      for (;;) {
        skipWs();
        if (!*p || *p == '\n' || *p == '\r') break;
        FV x{0, 0, 0};
        char* e;
        x.v = (int)std::strtol(p, &e, 10);
        p = e;
        if (*p == '/') {
          p++;
          if (*p != '/') { x.vt = (int)std::strtol(p, &e, 10); p = e; }
          if (*p == '/') { p++; x.vn = (int)std::strtol(p, &e, 10); p = e; }
        }
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        fv.push_back(x);
      }
      for (size_t i = 1; i + 1 < fv.size(); i++) {   // fan triangulation
        for (const FV& c : {fv[0], fv[i], fv[i + 1]}) {
          Vertex vx;
          int vi = c.v > 0 ? c.v - 1 : (int)verts.size() + c.v;
          if (vi < 0 || vi >= (int)verts.size()) { err = "face index out of range"; return false; }
          vx.p = verts[vi];
          if (c.vt && !uvs.empty()) {
            int ti = c.vt > 0 ? c.vt - 1 : (int)uvs.size() + c.vt;
            if (ti >= 0 && ti < (int)uvs.size()) vx.uv = uvs[ti];
          }
          bool n = false;
          if (c.vn) {
            int ni = c.vn > 0 ? c.vn - 1 : (int)normals.size() + c.vn;
            if (ni >= 0 && ni < (int)normals.size()) { vx.n = normals[ni]; n = true; }
          }
          allN = allN && n;
          cur.v.push_back(vx);
        }
      }
    }
    nextLine();
  }
  finish();
  if (out.empty()) { err = "no meshes"; return false; }
  return true;
}

// ─── GLB (tools/m3-to-glb.mjs output) ────────────────────────────────────────
// Mesh nodes "region_N" (bind pose), "Walk_k" / "Attack_k" (baked frames).
// Quantized positions are int16 inside the node's box: expanded with the
// node's translation + scale, as the web loader does.
struct GlbData {
  bool ok = false;
  std::string err;
  std::vector<RawPart> nodes;
};
static void parseGlbInto(const std::string& rel, GlbData& out) {
  fileDone(rel);
  std::string path = assetPath(rel);
  cgltf_options opt = {};
  cgltf_data* data = nullptr;
  cgltf_result r = cgltf_parse_file(&opt, path.c_str(), &data);
  if (r != cgltf_result_success) { out.err = "parse error " + std::to_string((int)r); return; }
  r = cgltf_load_buffers(&opt, data, path.c_str());
  if (r != cgltf_result_success) { out.err = "buffer error " + std::to_string((int)r); cgltf_free(data); return; }
  const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
  if (!scene) { out.err = "no scene"; cgltf_free(data); return; }
  for (size_t ni = 0; ni < scene->nodes_count; ni++) {
    const cgltf_node* node = scene->nodes[ni];
    if (!node->mesh || !node->mesh->primitives_count) continue;
    if (node->mesh->primitives_count > 1) logWarn("%s: node \"%s\" has %d primitives, using the first", rel.c_str(), node->name ? node->name : "", (int)node->mesh->primitives_count);
    const cgltf_primitive& prim = node->mesh->primitives[0];
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
    for (size_t a = 0; a < prim.attributes_count; a++) {
      const cgltf_attribute& at = prim.attributes[a];
      if (at.type == cgltf_attribute_type_position) pos = at.data;
      else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
      else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
    }
    if (!pos) continue;
    glm::vec3 t(0), s(1);
    if (node->has_translation) t = {node->translation[0], node->translation[1], node->translation[2]};
    if (node->has_scale) s = {node->scale[0], node->scale[1], node->scale[2]};
    RawPart rp;
    rp.name = node->name ? node->name : "";
    rp.g.v.resize(pos->count);
    for (size_t i = 0; i < pos->count; i++) {
      float p[3] = {0, 0, 0};
      cgltf_accessor_read_float(pos, i, p, 3);
      glm::vec3 q(p[0], p[1], p[2]);
      if (pos->normalized) q = glm::max(q, glm::vec3(-1));
      rp.g.v[i].p = q * s + t;
      if (nrm && i < nrm->count) {
        float n[3] = {0, 1, 0};
        cgltf_accessor_read_float(nrm, i, n, 3);
        glm::vec3 nn = glm::max(glm::vec3(n[0], n[1], n[2]), glm::vec3(-1));
        float l = glm::length(nn);
        rp.g.v[i].n = l > 0 ? nn / l : glm::vec3(0, 1, 0);
      }
      if (uv && i < uv->count) {
        float u[2] = {0, 0};
        cgltf_accessor_read_float(uv, i, u, 2);
        rp.g.v[i].uv = {u[0], u[1]};
      }
    }
    if (prim.indices) {
      rp.g.idx.resize(prim.indices->count);
      for (size_t i = 0; i < prim.indices->count; i++) rp.g.idx[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);
    }
    if (!nrm) { rp.g = rp.g.nonIndexed(); rp.g.computeFlatNormals(); }
    out.nodes.push_back(std::move(rp));
  }
  cgltf_free(data);
  out.ok = !out.nodes.empty();
  if (!out.ok) out.err = "no mesh nodes";
}

// cached per file: skins share one parse even when they load in parallel
static std::mutex g_glbMx;
static std::unordered_map<std::string, std::shared_future<std::shared_ptr<const GlbData>>> g_glb;
static std::shared_ptr<const GlbData> parseGlb(const std::string& rel) {
  std::shared_future<std::shared_ptr<const GlbData>> f;
  std::promise<std::shared_ptr<const GlbData>> mine;
  bool owner = false;
  {
    std::lock_guard<std::mutex> lk(g_glbMx);
    auto it = g_glb.find(rel);
    if (it != g_glb.end()) f = it->second;
    else { f = mine.get_future().share(); g_glb[rel] = f; owner = true; }
  }
  if (owner) {
    auto d = std::make_shared<GlbData>();
    parseGlbInto(rel, *d);
    mine.set_value(d);
  }
  return f.get();
}

// ─── Models ──────────────────────────────────────────────────────────────────
glm::mat4 Model::wrapper() const {
  return glm::rotate(glm::translate(glm::mat4(1), defaultPos), defaultRotY, glm::vec3(0, 1, 0));
}
glm::vec3 Model::local(const glm::vec3& raw) const { return glm::vec3(wrapper() * glm::vec4(k * (raw - center), 1)); }

namespace models {
// A model prepared on a worker thread (files parsed, geometry normalised,
// textures decoded); the GL thread turns it into meshes and textures.
struct Prepared {
  const AssetDef* def = nullptr;
  bool ok = false;
  std::string err;
  std::vector<RawPart> parts;
  std::vector<Geometry> walk, attack;
  glm::vec3 center{0}, size{0};
  float k = 1;
  double prepareMs = 0;
  std::vector<std::pair<std::string, ImageP>> images;
};

static std::unordered_map<std::string, std::unique_ptr<Model>> g_models;   // GL thread
static std::set<std::string> g_settled;                                     // GL thread
static std::mutex g_doneMx;
static std::deque<std::unique_ptr<Prepared>> g_done;   // prepared, waiting for the GL thread
static int g_total = 0, g_finished = 0;

const AssetDef* def(const char* key) {
  for (const AssetDef& d : ASSETS) if (!strcmp(d.key, key)) return &d;
  return nullptr;
}

static int frameNo(const std::string& n) {
  size_t u = n.rfind('_');
  return u == std::string::npos ? 0 : std::atoi(n.c_str() + u + 1);
}

// worker thread: everything but GL
static std::unique_ptr<Prepared> prepare(const AssetDef& d) {
  auto t0 = std::chrono::steady_clock::now();
  auto p = std::make_unique<Prepared>();
  p->def = &d;
  std::vector<RawPart>& bind = p->parts;
  std::shared_ptr<const GlbData> glb;
  if (d.glb) {
    glb = parseGlb(d.glb);
    if (!glb->ok) { p->err = std::string(d.glb) + ": " + glb->err; return p; }
    static const std::regex region("^region_\\d+$");
    for (const RawPart& r : glb->nodes) if (std::regex_match(r.name, region)) bind.push_back(r);
    if (bind.empty()) { p->err = std::string("no \"region_N\" meshes in ") + d.glb; return p; }
  } else if (d.obj) {
    fileDone(d.obj);
    if (!parseObj(assetPath(d.obj), bind, p->err)) { p->err = std::string(d.obj) + ": " + p->err; return p; }
  } else {
    p->err = "no model file";
    return p;
  }
  // normalise: centre on the bind pose's box; scale = world units per model unit
  glm::vec3 mn(1e30f), mx(-1e30f);
  for (const RawPart& r : bind) {
    glm::vec3 a, b;
    if (r.g.bounds(a, b)) { mn = glm::min(mn, a); mx = glm::max(mx, b); }
  }
  glm::vec3 size = mx - mn;
  float maxDim = std::max({size.x, size.y, size.z});
  if (maxDim <= 0) maxDim = 1;
  p->center = (mn + mx) * 0.5f;
  p->k = d.unitScale > 0 ? d.unitScale : d.scale / maxDim;
  p->size = size * p->k;
  glm::mat4 norm = glm::translate(glm::scale(glm::mat4(1), glm::vec3(p->k)), -p->center);
  for (RawPart& r : bind) r.g.transform(norm);
  if (glb) {
    for (const char* anim : {"walk", "attack"}) {
      std::regex re(std::string("^") + anim + "_[0-9]+$", std::regex::icase);
      std::vector<const RawPart*> fr;
      for (const RawPart& r : glb->nodes) if (std::regex_match(r.name, re)) fr.push_back(&r);
      std::sort(fr.begin(), fr.end(), [](const RawPart* a, const RawPart* b) { return frameNo(a->name) < frameNo(b->name); });
      auto& dst = !strcmp(anim, "walk") ? p->walk : p->attack;
      for (const RawPart* r : fr) {
        dst.push_back(r->g);
        dst.back().transform(norm);
      }
    }
  }
  for (const TexDef* t : {&d.tex, &d.meshTex})
    for (const char* f : {t->diffuse, t->emissive, t->normal})
      if (f) p->images.push_back({f, decodeCached(f)});
  p->ok = true;
  p->prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return p;
}

// GL thread
static const Texture* texOf(const Prepared& p, const char* f) {
  if (!f) return nullptr;
  for (auto& [path, img] : p.images) if (path == f) return uploadImage(path, img);
  return nullptr;
}
static Material makeMaterial(const Prepared& p, const TexDef& t) {
  Material m;
  m.type = MatType::Standard;
  m.color = glm::vec3(1);
  m.roughness = 0.55f;   // moderate metalness: with no environment map a metallic surface renders black
  m.metalness = 0.3f;
  m.map = texOf(p, t.diffuse);
  m.emissiveMap = texOf(p, t.emissive);
  m.normalMap = texOf(p, t.normal);
  if (m.emissiveMap) m.emissive = glm::vec3(0.85f);
  if (t.normalY) m.normalScale.y = t.normalY;
  if (p.def->hasTeamColor) { m.teamColorOn = true; m.teamColor = hexColor(p.def->teamColor); }
  return m;
}
static void finish(Prepared& p) {
  auto t0 = std::chrono::steady_clock::now();
  const AssetDef& d = *p.def;
  auto m = std::make_unique<Model>();
  m->def = &d;
  m->center = p.center;
  m->k = p.k;
  m->size = p.size;
  m->defaultPos = glm::vec3(0, d.yOff + m->size.y * 0.5f, 0);
  m->defaultRotY = d.rotY;
  bool textured = d.tex.diffuse || d.tex.emissive || d.tex.normal;
  Material base, own;
  if (textured) {
    base = makeMaterial(p, d.tex);
    if (d.meshTexName) own = makeMaterial(p, d.meshTex);
  } else {
    base.type = MatType::Lambert;   // OBJLoader's default grey
    base.color = hexColor(0x888888);
  }
  for (RawPart& r : p.parts) {
    ModelPart mp;
    mp.name = r.name;
    mp.cpu = std::move(r.g);
    mp.mesh = Mesh::create(mp.cpu);
    mp.mat = (d.meshTexName && mp.name == d.meshTexName) ? own : base;
    m->parts.push_back(std::move(mp));
  }
  for (auto& g : p.walk) m->walk.push_back(Mesh::create(g));
  for (auto& g : p.attack) m->attack.push_back(Mesh::create(g));
  g_models[d.key] = std::move(m);
  logInfo("asset %-16s prepared in %6.1f ms (worker), uploaded in %5.1f ms (GL thread)", d.key, p.prepareMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void startLoading() {
  std::vector<const AssetDef*> order;
  std::set<std::string> seen;
  for (const auto& stage : LOAD_STAGES)
    for (const char* k : stage)
      if (k && seen.insert(k).second && def(k)) order.push_back(def(k));
  for (const AssetDef& d : ASSETS)
    if (seen.insert(d.key).second) order.push_back(&d);
  g_total = (int)order.size();
  for (const AssetDef* d : order)   // FIFO pool: LOAD_STAGES order is the start order
    jobs::submit([d] {
      auto p = prepare(*d);
      std::lock_guard<std::mutex> lk(g_doneMx);
      g_done.push_back(std::move(p));
    });
}

const char* loadNext() {
  std::unique_ptr<Prepared> p;
  {
    std::lock_guard<std::mutex> lk(g_doneMx);
    if (g_done.empty()) return nullptr;
    p = std::move(g_done.front());
    g_done.pop_front();
  }
  if (p->ok) finish(*p);
  else logWarn("\"%s\" failed to load — drawn with simple shapes instead (%s)", p->def->key, p->err.c_str());
  g_settled.insert(p->def->key);
  g_finished++;
  return p->def->key;
}
bool allSettled() { return g_finished >= g_total; }

const Model* get(const char* key) {
  auto it = g_models.find(key);
  return it == g_models.end() ? nullptr : it->second.get();
}
bool isReady(const char* key) { return g_models.count(key) > 0; }
bool isSettled(const char* key) { return g_settled.count(key) > 0; }

void progress(int& done, int& total) {
  std::set<std::string> files, doneSet;
  {
    std::lock_guard<std::mutex> lk(g_progressMx);
    doneSet = g_filesDone;
  }
  for (const AssetDef& d : ASSETS) {
    std::vector<const char*> own = {d.glb ? d.glb : d.obj};
    for (const TexDef* t : {&d.tex, &d.meshTex}) own.insert(own.end(), {t->diffuse, t->emissive, t->normal});
    bool failed = isSettled(d.key) && !isReady(d.key);
    for (const char* f : own) {
      if (!f) continue;
      files.insert(f);
      if (failed) doneSet.insert(f);
    }
  }
  total = (int)files.size();
  done = 0;
  for (const std::string& f : files) if (doneSet.count(f)) done++;
}
}  // namespace models

// ─── Feature edges (three.js EdgesGeometry) ──────────────────────────────────
// Vertices are matched by position rounded to 1e-4, as three.js does (integer
// keys instead of its strings); runs on a worker for the hologram.
std::vector<glm::vec3> featureEdges(const Geometry& g, const glm::mat4& m, float thresholdDeg) {
  const float precision = 1e4f, thresholdDot = std::cos(thresholdDeg * 3.14159265f / 180.f);
  struct Key {
    int64_t a[6];
    bool operator==(const Key& o) const { return !memcmp(a, o.a, sizeof a); }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t h = 1469598103934665603ull;
      for (int64_t v : k.a) { h ^= (uint64_t)v; h *= 1099511628211ull; }
      return (size_t)h;
    }
  };
  struct Edge { glm::vec3 a, b, n; bool live; };
  std::unordered_map<Key, size_t, KeyHash> where;
  std::vector<Edge> edges;
  std::vector<glm::vec3> out;
  where.reserve(g.triCount() * 2);
  for (size_t t = 0; t < g.triCount(); t++) {
    glm::vec3 p[3];
    int64_t h[3][3];
    for (int k = 0; k < 3; k++) {
      p[k] = glm::vec3(m * glm::vec4(g.corner(t, k), 1));
      h[k][0] = std::llround(p[k].x * precision);
      h[k][1] = std::llround(p[k].y * precision);
      h[k][2] = std::llround(p[k].z * precision);
    }
    auto same = [&](int i, int j) { return !memcmp(h[i], h[j], sizeof h[i]); };
    if (same(0, 1) || same(1, 2) || same(2, 0)) continue;   // degenerate
    glm::vec3 n = glm::cross(p[2] - p[1], p[0] - p[1]);
    float l = glm::length(n);
    n = l > 0 ? n / l : glm::vec3(0);
    for (int j = 0; j < 3; j++) {
      int jn = (j + 1) % 3;
      Key key, rev;
      memcpy(key.a, h[j], sizeof h[j]);
      memcpy(key.a + 3, h[jn], sizeof h[jn]);
      memcpy(rev.a, h[jn], sizeof h[jn]);
      memcpy(rev.a + 3, h[j], sizeof h[j]);
      auto r = where.find(rev);
      if (r != where.end() && edges[r->second].live) {
        if (glm::dot(n, edges[r->second].n) <= thresholdDot) { out.push_back(p[j]); out.push_back(p[jn]); }
        edges[r->second].live = false;
      } else if (!where.count(key)) {
        where[key] = edges.size();
        edges.push_back({p[j], p[jn], n, true});
      }
    }
  }
  for (const Edge& e : edges) if (e.live) { out.push_back(e.a); out.push_back(e.b); }
  return out;
}
