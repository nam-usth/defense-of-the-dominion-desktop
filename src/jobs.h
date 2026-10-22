// Worker thread pool for background work that must not stall a frame: asset
// parsing and texture decoding, hologram edge extraction. (Per-frame loops stay
// serial: measured, splitting them across threads cost more than it saved.)
#pragma once
#include <functional>

namespace jobs {
void init(int threads = -1);  // -1 = min(4, hardware threads − 1); 0 = none: tasks run inline
void shutdown();
int workers();
// fire-and-forget; runs on a worker, FIFO (submission order = priority)
void submit(std::function<void()> fn);
}
