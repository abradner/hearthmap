// The worker port's hand-off (hearthmap/ui/worker.h, D-069): inline and on a real second thread.
// The threaded cases are what ThreadSanitizer checks (the `tsan` CI job runs this file's cases):
// a job's writes must be visible once poll() says Finished, and nothing may touch a job's
// buffers on the worker after it is back.
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "hearthmap/ui/worker.h"
#include "hearthmap/ui/worker_thread.h"

using namespace hearthmap::ui;

namespace {

/// Fills `out` with its index plus `seed`, `chunk` elements per step.
struct FillJob final : WorkJob {
  std::vector<std::uint32_t>* out = nullptr;
  std::uint32_t seed = 0;
  std::size_t chunk = 64;
  std::size_t next = 0;
  /// For the tests only: how many steps ran (the worker writes, the test reads while it runs).
  std::atomic<std::uint32_t> steps{0};
  bool step() override {
    steps.fetch_add(1, std::memory_order_relaxed);
    const std::size_t end = std::min(out->size(), next + chunk);
    for (; next < end; ++next) (*out)[next] = static_cast<std::uint32_t>(next) + seed;
    return next < out->size();
  }
};

/// Polls until the lane is back (the test's main loop; it yields rather than spins hot).
Worker::Poll wait_back(Worker& w, WorkLane lane, bool* completed) {
  for (;;) {
    const Worker::Poll p = w.poll(lane, completed);
    if (p != Worker::Poll::Busy) return p;
    std::this_thread::yield();
  }
}

}  // namespace

TEST_CASE("an inline worker runs one step per poll, on the caller, until the job is done") {
  InlineWorker w;
  CHECK_FALSE(w.threaded());
  std::vector<std::uint32_t> out(256, 0);
  FillJob job;
  job.out = &out;
  CHECK(w.poll(WorkLane::Sprites) == Worker::Poll::Idle);
  REQUIRE(w.submit(WorkLane::Sprites, job));
  CHECK(w.busy(WorkLane::Sprites));
  CHECK_FALSE(w.submit(WorkLane::Sprites, job));  // one job per lane
  CHECK(job.steps == 0);                          // nothing runs until polled
  int polls = 0;
  bool completed = false;
  Worker::Poll p = Worker::Poll::Busy;
  while (p == Worker::Poll::Busy) {
    p = w.poll(WorkLane::Sprites, &completed);
    ++polls;
  }
  CHECK(p == Worker::Poll::Finished);
  CHECK(completed);
  CHECK(polls == 4);  // 256 / 64: one step per poll
  CHECK(out[255] == 255);
  CHECK_FALSE(w.busy(WorkLane::Sprites));
  CHECK(w.poll(WorkLane::Sprites) == Worker::Poll::Idle);
  CHECK(w.stats().completed == 1);
}

TEST_CASE("an inline job cancelled before its next step comes back unfinished") {
  InlineWorker w;
  std::vector<std::uint32_t> out(256, 0);
  FillJob job;
  job.out = &out;
  REQUIRE(w.submit(WorkLane::Heat, job));
  CHECK(w.poll(WorkLane::Heat) == Worker::Poll::Busy);  // one step ran
  w.cancel(WorkLane::Heat);
  bool completed = true;
  CHECK(w.poll(WorkLane::Heat, &completed) == Worker::Poll::Finished);
  CHECK_FALSE(completed);
  CHECK(job.steps == 1);  // no step after the cancel
  CHECK(out[64] == 0);
  // The lane is free again, and a new job on it starts clean (not cancelled).
  FillJob again;
  again.out = &out;
  REQUIRE(w.submit(WorkLane::Heat, again));
  CHECK(wait_back(w, WorkLane::Heat, &completed) == Worker::Poll::Finished);
  CHECK(completed);
  CHECK(out[255] == 255);
}

TEST_CASE("worker: a threaded job's writes are all visible once it is polled back") {
  ThreadWorker w;
  CHECK(w.threaded());
  std::vector<std::uint32_t> out(64 * 1024, 0);
  for (std::uint32_t round = 1; round <= 200; ++round) {
    FillJob job;
    job.out = &out;
    job.seed = round;
    job.chunk = 4096;
    REQUIRE(w.submit(WorkLane::Sprites, job));
    bool completed = false;
    REQUIRE(wait_back(w, WorkLane::Sprites, &completed) == Worker::Poll::Finished);
    REQUIRE(completed);
    bool all = true;
    for (std::size_t i = 0; i < out.size(); ++i) all = all && out[i] == i + round;
    REQUIRE(all);
    // The caller owns the buffer again: it may write it while the worker idles.
    out[0] = 0;
  }
  CHECK(w.stats().completed == 200);
}

TEST_CASE(
    "worker: a threaded job cancelled while building is returned before its buffer is reused") {
  ThreadWorker w;
  for (int round = 0; round < 100; ++round) {
    std::vector<std::uint32_t> out(1U << 20, 0);
    FillJob job;
    job.out = &out;
    job.chunk = 64;  // many steps: the cancel lands while it runs
    REQUIRE(w.submit(WorkLane::Heat, job));
    while (job.steps.load(std::memory_order_relaxed) < 2) std::this_thread::yield();
    w.cancel(WorkLane::Heat);
    // Busy until the worker gives it back; then nothing of the job's runs again.
    bool completed = true;
    REQUIRE(wait_back(w, WorkLane::Heat, &completed) == Worker::Poll::Finished);
    const std::uint32_t steps = job.steps.load(std::memory_order_relaxed);
    CHECK_FALSE(completed);
    CHECK(steps < (1U << 20) / 64U);
    // Reused at once (a sanitiser sees a write racing the worker if it still ran); freed when the
    // round ends.
    for (auto& v : out) v = 7;
    std::this_thread::yield();
    CHECK(job.steps.load(std::memory_order_relaxed) == steps);
  }
}

TEST_CASE("worker: two lanes run side by side on one thread, each handed back on its own") {
  ThreadWorker w;
  std::vector<std::uint32_t> a(200000, 0), b(300000, 0);
  FillJob ja, jb;
  ja.out = &a;
  ja.seed = 1;
  jb.out = &b;
  jb.seed = 2;
  REQUIRE(w.submit(WorkLane::Sprites, ja));
  REQUIRE(w.submit(WorkLane::Heat, jb));
  bool ca = false, cb = false;
  REQUIRE(wait_back(w, WorkLane::Sprites, &ca) == Worker::Poll::Finished);
  REQUIRE(wait_back(w, WorkLane::Heat, &cb) == Worker::Poll::Finished);
  CHECK(ca);
  CHECK(cb);
  CHECK(a.back() == a.size() - 1 + 1);
  CHECK(b.back() == b.size() - 1 + 2);
  CHECK(w.stats().busy_us > 0);
}
