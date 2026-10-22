#include "jobs.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "log.h"

namespace jobs {
namespace {
std::vector<std::thread> g_threads;
std::deque<std::function<void()>> g_queue;
std::mutex g_mx;
std::condition_variable g_cv;
bool g_stop = false;

void workerLoop() {
  for (;;) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lk(g_mx);
      g_cv.wait(lk, [] { return g_stop || !g_queue.empty(); });
      if (g_stop && g_queue.empty()) return;
      fn = std::move(g_queue.front());
      g_queue.pop_front();
    }
    fn();
  }
}
}  // namespace

// Capped at 4: with 19 workers the asset loads contended (the allocator, the
// laptop's efficiency cores) and each one took several times longer
void init(int threads) {
  if (!g_threads.empty()) return;
  if (threads < 0) threads = std::clamp((int)std::thread::hardware_concurrency() - 1, 1, 4);
  g_stop = false;
  for (int i = 0; i < threads; i++) g_threads.emplace_back(workerLoop);
  logInfo("jobs: %d worker threads", threads);
}

void shutdown() {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_stop = true;
    g_queue.clear();   // pending background work is dropped on exit
  }
  g_cv.notify_all();
  for (auto& t : g_threads) t.join();
  g_threads.clear();
}

int workers() { return (int)g_threads.size(); }

void submit(std::function<void()> fn) {
  if (g_threads.empty()) { fn(); return; }   // no workers: run inline
  {
    std::lock_guard<std::mutex> lk(g_mx);
    g_queue.push_back(std::move(fn));
  }
  g_cv.notify_one();
}
}  // namespace jobs
