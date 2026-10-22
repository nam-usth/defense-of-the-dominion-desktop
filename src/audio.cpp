#include "audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "assets.h"
#include "config.h"
#include "log.h"
#include "miniaudio.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace audio {
namespace {
constexpr int RATE = 48000;
constexpr double PI = 3.14159265358979;

enum class Wave { Sine, Square, Saw, Triangle };
enum class Filt { Bandpass, Lowpass, Highpass };

// One scheduled sound primitive (Web Audio: a source → filter → envelope gain)
struct Voice {
  bool noise;
  int64_t start;      // sample index of t
  double dur;         // seconds of the envelope / ramps
  double f0, f1;      // frequency sweep (filter cutoff for noise, pitch for a tone)
  double gain;
  Wave wave = Wave::Square;
  Filt filt = Filt::Bandpass;
  double q = 1.2;
  double noiseLen = 1;   // the 1 s noise buffer at playbackRate r lasts 1/r s
  // state
  double phase = 0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0, b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

std::mutex g_mx;
std::vector<Voice> g_pending, g_active;   // pending: added by play(), taken by the audio thread
std::atomic<int64_t> g_clock{0};          // samples rendered so far
std::atomic<float> g_master{0.6f};        // volume (0 when muted)
float g_volume = 0.6f;
bool g_muted = false;
std::map<std::string, int64_t> g_last;    // per effect: last start (rate limit)
std::mt19937 g_rng{12345};
ma_device g_device;
std::atomic<bool> g_deviceOk{false};
std::thread g_initThread;

// compressor (threshold -16 dB, ratio 6, knee 30 dB, attack 3 ms, release 250 ms) + Web Audio-style makeup gain
double g_env = 0;
double compGainDb(double xdb) {
  const double T = -16, R = 6, W = 30;
  double y;
  if (2 * (xdb - T) < -W) y = xdb;
  else if (2 * std::abs(xdb - T) <= W) y = xdb + (1 / R - 1) * (xdb - T + W / 2) * (xdb - T + W / 2) / (2 * W);
  else y = T + (xdb - T) / R;
  return y - xdb;
}
const double g_makeup = std::pow(1.0 / std::pow(10.0, compGainDb(0) / 20), 0.6);

// ── Music (decoded on its own thread into a ring buffer) ──────────────────
struct Ring {
  std::vector<float> buf = std::vector<float>(RATE * 2 * 2);   // 2 s stereo
  size_t r = 0, w = 0, n = 0;
} g_ring;
std::mutex g_ringMx;
std::atomic<bool> g_musicRun{false}, g_musicOn{true}, g_suspended{false};
std::thread g_musicThread;
std::mutex g_reqMx;
std::string g_reqScene, g_scene;
bool g_reqNew = false;

double frand() { return std::uniform_real_distribution<double>(0, 1)(g_rng); }

void setBiquad(Voice& v, double f) {
  double w0 = 2 * PI * std::min(f, RATE * 0.49) / RATE, c = std::cos(w0), s = std::sin(w0);
  double b0, b1, b2, a0, a1, a2;
  if (v.filt == Filt::Bandpass) {   // constant 0 dB peak gain, Q linear
    double alpha = s / (2 * std::max(1e-4, v.q));
    b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
  } else {                          // lowpass / highpass: Q in dB (Web Audio)
    double alpha = s / (2 * std::pow(10.0, v.q / 20));
    if (v.filt == Filt::Lowpass) { b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; }
    else { b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; }
    a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
  }
  v.b0 = b0 / a0; v.b1 = b1 / a0; v.b2 = b2 / a0; v.a1 = a1 / a0; v.a2 = a2 / a0;
}

double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}

// renders voice v at absolute sample index i; returns false once it has ended
bool renderVoice(Voice& v, int64_t i, double& out) {
  if (i < v.start) return true;
  double t = (double)(i - v.start) / RATE;
  if (t > v.dur + 0.02) return false;
  double env;   // exponential attack (4 ms) then decay to 0.0001 at dur
  if (t < 0.004) env = 0.0001 * std::pow(v.gain / 0.0001, t / 0.004);
  else if (t < v.dur) env = v.gain * std::pow(0.0001 / v.gain, (t - 0.004) / std::max(1e-4, v.dur - 0.004));
  else env = 0.0001;
  double fr = t < v.dur ? v.f0 * std::pow(v.f1 / v.f0, t / v.dur) : v.f1;
  double s;
  if (v.noise) {
    if (t > v.noiseLen) return true;   // buffer played out: silence until the source stops
    if (((i - v.start) & 15) == 0) setBiquad(v, fr);
    double x = frand() * 2 - 1;
    s = v.b0 * x + v.b1 * v.x1 + v.b2 * v.x2 - v.a1 * v.y1 - v.a2 * v.y2;
    v.x2 = v.x1; v.x1 = x; v.y2 = v.y1; v.y1 = s;
  } else {
    double dt = fr / RATE;
    switch (v.wave) {
      case Wave::Sine: s = std::sin(2 * PI * v.phase); break;
      case Wave::Square: s = (v.phase < 0.5 ? 1 : -1) + polyBlep(v.phase, dt) - polyBlep(std::fmod(v.phase + 0.5, 1.0), dt); break;
      case Wave::Saw: s = 2 * v.phase - 1 - polyBlep(v.phase, dt); break;
      default: s = 4 * std::abs(v.phase - 0.5) - 1; break;
    }
    v.phase += dt;
    if (v.phase >= 1) v.phase -= 1;
  }
  out += s * env;
  return true;
}

void dataCallback(ma_device*, void* pOut, const void*, ma_uint32 frames) {
  float* out = (float*)pOut;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_pending.empty()) {
      g_active.insert(g_active.end(), g_pending.begin(), g_pending.end());
      g_pending.clear();
    }
  }
  int64_t base = g_clock.load();
  float master = g_master.load();
  // music block
  static std::vector<float> mus;
  mus.assign((size_t)frames * 2, 0.f);
  if (g_musicOn.load() && !g_suspended.load()) {
    std::lock_guard<std::mutex> lk(g_ringMx);
    size_t take = std::min<size_t>(frames, g_ring.n / 2);
    for (size_t k = 0; k < take * 2; k++) {
      mus[k] = g_ring.buf[g_ring.r];
      g_ring.r = (g_ring.r + 1) % g_ring.buf.size();
    }
    g_ring.n -= take * 2;
  }
  const double att = std::exp(-1.0 / (0.003 * RATE)), rel = std::exp(-1.0 / (0.25 * RATE));
  for (ma_uint32 f = 0; f < frames; f++) {
    double fx = 0;
    int64_t i = base + f;
    for (size_t k = 0; k < g_active.size();) {
      if (!renderVoice(g_active[k], i, fx)) { g_active[k] = g_active.back(); g_active.pop_back(); }
      else k++;
    }
    double l = (fx + mus[f * 2] * MUSIC_LEVEL) * master, r = (fx + mus[f * 2 + 1] * MUSIC_LEVEL) * master;
    double peak = std::max(std::abs(l), std::abs(r));
    g_env = peak > g_env ? att * g_env + (1 - att) * peak : rel * g_env + (1 - rel) * peak;
    double g = std::pow(10.0, compGainDb(20 * std::log10(std::max(g_env, 1e-6))) / 20) * g_makeup;
    out[f * 2] = (float)std::clamp(l * g, -1.0, 1.0);
    out[f * 2 + 1] = (float)std::clamp(r * g, -1.0, 1.0);
  }
  g_clock.store(base + frames);
}

void add(std::vector<Voice>& vs, Voice v) { vs.push_back(v); }
Voice noise(int64_t t, double dur, double f0, double f1, double gain, Filt type = Filt::Bandpass, double q = 1.2) {
  Voice v{};
  v.noise = true; v.start = t; v.dur = dur; v.f0 = f0; v.f1 = std::max(20.0, f1); v.gain = gain; v.filt = type; v.q = q;
  v.noiseLen = 1.0 / (0.8 + frand() * 0.4);
  return v;
}
Voice tone(int64_t t, double dur, double f0, double f1, double gain, Wave w = Wave::Square) {
  Voice v{};
  v.noise = false; v.start = t; v.dur = dur; v.f0 = f0; v.f1 = std::max(20.0, f1); v.gain = gain; v.wave = w;
  return v;
}
int64_t S(double sec) { return (int64_t)std::llround(sec * RATE); }

// The web version's SOUNDS table
bool build(const std::string& n, int64_t t, std::vector<Voice>& o) {
  using F = Filt; using W = Wave;
  if (n == "marine") { add(o, noise(t, .07, 1800, 900, .32)); add(o, tone(t, .05, 140, 60, .06)); }
  else if (n == "marauder") { add(o, tone(t, .18, 120, 45, .45, W::Sine)); add(o, noise(t, .14, 900, 200, .28, F::Lowpass)); }
  else if (n == "ghost") { add(o, tone(t, .12, 900, 180, .14, W::Saw)); add(o, noise(t, .05, 3000, 2500, .14, F::Highpass)); }
  else if (n == "shrike") { add(o, noise(t, .06, 1300, 650, .28)); add(o, tone(t, .06, 95, 50, .08)); }
  else if (n == "hit") add(o, noise(t, .03, 2600, 2000, .1, F::Highpass));
  else if (n == "zdie") { add(o, tone(t, .16, 650, 140, .09, W::Saw)); add(o, noise(t, .15, 700, 260, .2)); }
  else if (n == "rdie") { add(o, noise(t, .35, 600, 110, .38, F::Lowpass)); add(o, tone(t, .3, 80, 38, .28, W::Sine)); }
  else if (n == "boom") { add(o, noise(t, .7, 1200, 55, .75, F::Lowpass, .7)); add(o, tone(t, .6, 62, 30, .55, W::Sine)); }
  else if (n == "nukeWarn") {
    for (int i = 0; i < 4; i++) { add(o, tone(t + S(i * .5), .14, 880, 860, .09)); add(o, tone(t + S(i * .5 + .2), .14, 660, 650, .07)); }
  } else if (n == "nukeBoom") {
    add(o, noise(t, 2.2, 2200, 38, 1.0, F::Lowpass, .6)); add(o, tone(t, 2.0, 48, 24, .9, W::Sine));
    add(o, noise(t + S(.05), .5, 4000, 900, .35, F::Highpass));
  } else if (n == "wave") {
    add(o, tone(t, .55, 110, 108, .07, W::Saw)); add(o, tone(t, .55, 165, 162, .06, W::Saw)); add(o, tone(t + S(.3), .5, 220, 218, .05, W::Saw));
  } else if (n == "clear") {
    const double fq[4] = {523, 659, 784, 1046};
    for (int i = 0; i < 4; i++) add(o, tone(t + S(i * .09), .16, fq[i], fq[i], .12, W::Sine));
  } else if (n == "bite") { add(o, tone(t, .08, 230, 140, .1, W::Triangle)); add(o, noise(t, .06, 420, 300, .14)); }
  else if (n == "stim") { add(o, tone(t, .28, 260, 900, .07, W::Saw)); add(o, noise(t, .22, 1500, 4500, .1, F::Highpass)); }
  else if (n == "spit") { add(o, noise(t, .13, 700, 2400, .2, F::Bandpass, 2.5)); add(o, tone(t, .07, 320, 150, .05, W::Sine)); }
  else if (n == "acid") { add(o, noise(t, .3, 5200, 2400, .15, F::Highpass)); add(o, noise(t, .1, 650, 260, .12, F::Lowpass)); }
  else if (n == "buy") { add(o, tone(t, .07, 660, 660, .12, W::Sine)); add(o, tone(t + S(.07), .09, 990, 990, .12, W::Sine)); }
  else if (n == "deny") add(o, tone(t, .14, 190, 150, .09, W::Square));
  else if (n == "crate") add(o, tone(t, .16, 440, 880, .12, W::Sine));
  else if (n == "snipe") { add(o, tone(t, .25, 1600, 300, .16, W::Sine)); add(o, noise(t, .08, 3500, 3000, .16, F::Highpass)); }
  else if (n == "rocket") { add(o, noise(t, .45, 900, 2600, .2, F::Bandpass, 1.5)); add(o, tone(t, .1, 160, 70, .12, W::Sine)); }   // Viking missile off the rail
  else return false;
  return true;
}
const std::map<std::string, double> MIN_GAP = {{"hit", .045}, {"bite", .22}, {"spit", .07}, {"acid", .06}, {"marine", .03}, {"marauder", .05},
                                               {"ghost", .05}, {"shrike", .05}, {"zdie", .035}, {"rdie", .06}, {"boom", .07}, {"rocket", .06}};

void applyMaster() { g_master.store(g_muted ? 0.f : g_volume); }

// ── Music decoding ────────────────────────────────────────────────────────
std::vector<std::string> listOf(const std::string& scene) {
  if (scene == "title") return {"main_title"};
  if (scene == "battle") return {"terran_one", "terran_two", "terran_three"};
  if (scene == "victory") return {"terran_victory"};
  if (scene == "defeat") return {"terran_defeat"};
  return {};
}

#ifdef _WIN32
struct Decoder {
  IMFSourceReader* reader = nullptr;
  int rate = 0, ch = 0;
  double pos = 0;                 // resampler read position in source frames
  std::vector<float> src;         // decoded source frames not yet resampled (interleaved, ch)
  bool eos = false;
  bool open(const std::string& path) {
    close();
    int wn = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(wn > 0 ? wn : 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, w.data(), wn);
    if (FAILED(MFCreateSourceReaderFromURL(w.c_str(), nullptr, &reader))) { reader = nullptr; return false; }
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    HRESULT hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
    t->Release();
    if (FAILED(hr)) { close(); return false; }
    IMFMediaType* cur = nullptr;
    if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) { close(); return false; }
    UINT32 r = 0, c = 0;
    cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
    cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
    cur->Release();
    rate = (int)r; ch = (int)c;
    pos = 0; src.clear(); eos = false;
    return rate > 0 && ch > 0;
  }
  void close() {
    if (reader) reader->Release();
    reader = nullptr;
  }
  bool readMore() {   // appends one decoded sample buffer to src
    if (!reader || eos) return false;
    DWORD flags = 0;
    IMFSample* s = nullptr;
    if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &s))) { eos = true; return false; }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) eos = true;
    if (!s) return !eos;
    IMFMediaBuffer* b = nullptr;
    if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
      BYTE* d = nullptr;
      DWORD len = 0;
      if (SUCCEEDED(b->Lock(&d, nullptr, &len))) {
        const float* f = (const float*)d;
        src.insert(src.end(), f, f + len / sizeof(float));
        b->Unlock();
      }
      b->Release();
    }
    s->Release();
    return true;
  }
  // produces up to `frames` stereo frames at RATE; returns how many
  size_t produce(float* out, size_t frames) {
    size_t made = 0;
    double step = (double)rate / RATE;
    while (made < frames) {
      size_t have = src.size() / ch;
      if ((size_t)pos + 1 >= have) {
        if (!readMore() && (eos || !reader)) break;
        continue;
      }
      size_t i0 = (size_t)pos;
      double fr = pos - i0;
      for (int c = 0; c < 2; c++) {
        int sc = std::min(c, ch - 1);
        float a = src[i0 * ch + sc], b = src[(i0 + 1) * ch + sc];
        out[made * 2 + c] = (float)(a + (b - a) * fr);
      }
      made++;
      pos += step;
      size_t drop = (size_t)pos;
      if (drop > 4096) {   // keep the source buffer short
        src.erase(src.begin(), src.begin() + drop * ch);
        pos -= drop;
      }
    }
    return made;
  }
};

void musicThread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(MFStartup(MF_VERSION))) { logWarn("music: Media Foundation unavailable — no music"); CoUninitialize(); return; }
  Decoder dec;
  std::vector<std::string> list;
  size_t idx = 0;
  bool loop = false, openOk = false;
  std::vector<float> chunk(4096 * 2);
  auto openTrack = [&]() {
    std::string rel = std::string(AUDIO_DIR) + list[idx] + ".aac";
    openOk = dec.open(assetPath(rel));
    if (!openOk) logWarn("music: \"%s\" could not be decoded", rel.c_str());
  };
  while (g_musicRun.load()) {
    {
      std::lock_guard<std::mutex> lk(g_reqMx);
      if (g_reqNew) {
        g_reqNew = false;
        g_scene = g_reqScene;
        list = listOf(g_scene);
        idx = 0;
        loop = g_scene == "title" || g_scene == "battle";
        { std::lock_guard<std::mutex> rk(g_ringMx); g_ring.r = g_ring.w = g_ring.n = 0; }
        dec.close();
        openOk = false;
        if (!list.empty()) openTrack();
      }
    }
    size_t space;
    { std::lock_guard<std::mutex> rk(g_ringMx); space = (g_ring.buf.size() - g_ring.n) / 2; }
    if (!openOk || list.empty() || space < 4096) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
    size_t got = dec.produce(chunk.data(), 4096);
    if (got) {
      std::lock_guard<std::mutex> rk(g_ringMx);
      for (size_t k = 0; k < got * 2; k++) { g_ring.buf[g_ring.w] = chunk[k]; g_ring.w = (g_ring.w + 1) % g_ring.buf.size(); }
      g_ring.n += got * 2;
    }
    if (got < 4096 && dec.eos) {   // track ended: next in the list, loop, or stop
      if (idx + 1 < list.size()) idx++;
      else if (loop) idx = 0;
      else { list.clear(); dec.close(); openOk = false; continue; }
      openTrack();
    }
  }
  dec.close();
  MFShutdown();
  CoUninitialize();
}
#else
void musicThread() { logWarn("music: AAC decoding is only implemented on Windows"); }
#endif
}  // namespace

// Opening the output device (WASAPI) takes over a second on some machines, so
// it happens on its own thread: the window and title screen appear meanwhile,
// and effects start once the device runs. Music decoding starts at once.
void init(float vol, bool mute, bool musicOnV) {
  g_volume = std::clamp(vol, 0.f, 1.f);
  g_muted = mute;
  g_musicOn = musicOnV;
  applyMaster();
  g_musicRun = true;
  g_musicThread = std::thread(musicThread);
  g_initThread = std::thread([] {
    auto t0 = std::chrono::steady_clock::now();
    ma_device_config c = ma_device_config_init(ma_device_type_playback);
    c.playback.format = ma_format_f32;
    c.playback.channels = 2;
    c.sampleRate = RATE;
    c.dataCallback = dataCallback;
    if (ma_device_init(nullptr, &c, &g_device) != MA_SUCCESS) { logWarn("audio: no output device — the game runs silent"); return; }
    if (ma_device_start(&g_device) != MA_SUCCESS) { logWarn("audio: the output device would not start"); ma_device_uninit(&g_device); return; }
    g_deviceOk = true;
    logInfo("audio: output device ready after %.0f ms (on its own thread)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  });
}

void shutdown() {
  if (g_initThread.joinable()) g_initThread.join();
  g_musicRun = false;
  if (g_musicThread.joinable()) g_musicThread.join();
  if (g_deviceOk) ma_device_uninit(&g_device);
  g_deviceOk = false;
}

void play(const char* name) {
  if (!g_deviceOk || g_muted) return;
  int64_t t = g_clock.load() + S(0.01);   // a little ahead of the block being rendered
  auto gap = MIN_GAP.find(name);
  if (gap != MIN_GAP.end()) {
    auto l = g_last.find(name);
    if (l != g_last.end() && t - l->second < S(gap->second)) return;
  }
  std::vector<Voice> vs;
  if (!build(name, t, vs)) { logWarn("audio: unknown effect \"%s\"", name); return; }
  g_last[name] = t;
  std::lock_guard<std::mutex> lk(g_mx);
  g_pending.insert(g_pending.end(), vs.begin(), vs.end());
}

void setVolume(float v) {
  g_volume = std::clamp(v, 0.f, 1.f);
  if (g_volume > 0) g_muted = false;
  applyMaster();
}
void setMuted(bool m) { g_muted = m; applyMaster(); }
float volume() { return g_volume; }
bool muted() { return g_muted; }

void music(const char* scene) {
  std::lock_guard<std::mutex> lk(g_reqMx);
  if (g_scene == scene && !g_reqNew) return;   // a scene already playing keeps going
  if (g_reqNew && g_reqScene == scene) return;
  g_reqScene = scene;
  g_reqNew = true;
}
void setMusicOn(bool on) { g_musicOn = on; }
bool musicOn() { return g_musicOn.load(); }
void setSuspended(bool s) { g_suspended = s; }
}  // namespace audio
