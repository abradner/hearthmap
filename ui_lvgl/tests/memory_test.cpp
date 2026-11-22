// The memory port (hearthmap/ui/memory.h, D-064): buffers, accounting, refusals, and that the
// port in use cannot be swapped from under memory it handed out.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "hearthmap/ui/memory.h"

using namespace hearthmap::ui;

namespace {

/// The system heap, refusing anything over `limit` bytes, and counting what it holds.
class LimitedMemory final : public BulkMemory {
 public:
  explicit LimitedMemory(std::size_t limit) : limit_(limit) {}
  void* allocate(std::size_t bytes, std::size_t align) override {
    if (bytes > limit_) return nullptr;
    void* p = system_bulk_memory().allocate(bytes, align);
    if (p != nullptr) held += bytes;
    return p;
  }
  void deallocate(void* p, std::size_t bytes) override {
    if (p == nullptr) return;
    held -= bytes;
    system_bulk_memory().deallocate(p, bytes);
  }
  std::size_t limit_;
  std::size_t held = 0;
};

/// Installs a port for one test and puts the system's back.
struct Installed {
  explicit Installed(BulkMemory* m) { REQUIRE(set_bulk_memory(m)); }
  ~Installed() { (void)set_bulk_memory(nullptr); }
};

}  // namespace

TEST_CASE("a BulkBuffer takes exactly what it holds from the port and gives it back") {
  LimitedMemory mem(1U << 20);
  Installed installed(&mem);
  {
    BulkBuffer<std::uint16_t> b;
    CHECK(b.empty());
    REQUIRE(b.assign(100, 7));
    CHECK(b.size() == 100);
    CHECK(b.bytes() == 200);
    CHECK(mem.held == 200);
    CHECK(bulk_stats().in_use == 200);
    CHECK(b[99] == 7);
    b[5] = 9;
    REQUIRE(b.resize(100));  // same size: kept, contents included
    CHECK(b[5] == 9);
    REQUIRE(b.resize(10));  // another size: exactly that much
    CHECK(mem.held == 20);
    BulkBuffer<std::uint16_t> moved(std::move(b));
    CHECK(b.empty());  // NOLINT: moved-from is empty by contract
    CHECK(moved.size() == 10);
    CHECK(mem.held == 20);
    BulkBuffer<std::uint16_t> other;
    REQUIRE(other.assign(4, 1));
    other = std::move(moved);  // the old four are given back first
    CHECK(mem.held == 20);
    other.release();
    CHECK(mem.held == 0);
    CHECK(other.empty());
  }
  CHECK(bulk_stats().in_use == 0);
  CHECK(bulk_stats().peak == 200);
}

TEST_CASE("a refused BulkBuffer is empty and counted; nothing comes from elsewhere") {
  LimitedMemory mem(64);
  Installed installed(&mem);
  BulkBuffer<std::uint8_t> b;
  REQUIRE(b.assign(32, 1));
  CHECK_FALSE(b.assign(65, 1));
  CHECK(b.empty());  // the old contents went back, and nothing replaced them
  CHECK(mem.held == 0);
  const BulkStats s = bulk_stats();
  CHECK(s.failures == 1);
  CHECK(s.last_failed_bytes == 65);
  CHECK(s.in_use == 0);
}

namespace {

/// The system heap, scribbling over every block it is given back, so a read of freed memory
/// sees the scribble instead of the old value.
class ScribblingMemory final : public BulkMemory {
 public:
  void* allocate(std::size_t bytes, std::size_t align) override {
    return system_bulk_memory().allocate(bytes, align);
  }
  void deallocate(void* p, std::size_t bytes) override {
    if (p != nullptr) std::memset(p, 0xAB, bytes);
    system_bulk_memory().deallocate(p, bytes);
  }
};

}  // namespace

TEST_CASE("assign() fills with a value taken from the buffer's own old storage") {
  ScribblingMemory mem;
  Installed installed(&mem);
  BulkBuffer<std::uint16_t> b;
  REQUIRE(b.assign(2, 7));
  REQUIRE(b.assign(4, b[0]));  // the old storage is freed before the fill
  CHECK(b[0] == 7);
  CHECK(b[3] == 7);
  REQUIRE(b.assign(4, b[1]));  // same size: kept in place
  CHECK(b[2] == 7);
}

TEST_CASE("BulkVector and make_bulk allocate through the port") {
  LimitedMemory mem(1U << 20);
  Installed installed(&mem);
  {
    BulkVector<std::uint32_t> v;
    v.reserve(16);
    CHECK(mem.held == 64);
    struct Thing {
      int a;
      BulkBuffer<std::uint8_t> px;
    };
    BulkPtr<Thing> t = make_bulk<Thing>();
    CHECK(mem.held == 64 + sizeof(Thing));
    REQUIRE(t->px.assign(10, 0));
    CHECK(mem.held == 64 + sizeof(Thing) + 10);
  }
  CHECK(mem.held == 0);
}

TEST_CASE("try_make_bulk returns nothing when refused, and counts it") {
  LimitedMemory mem(4);
  Installed installed(&mem);
  const std::uint32_t before = bulk_stats().failures;
  BulkPtr<std::uint64_t> p = try_make_bulk<std::uint64_t>(7U);
  CHECK_FALSE(p);
  CHECK(bulk_stats().failures == before + 1);
  BulkPtr<std::uint32_t> q = try_make_bulk<std::uint32_t>(9U);
  REQUIRE(q);
  CHECK(*q == 9U);
}

TEST_CASE("the port cannot be changed while its memory is held") {
  LimitedMemory a(1U << 20);
  LimitedMemory b(1U << 20);
  REQUIRE(set_bulk_memory(&a));
  {
    BulkBuffer<std::uint8_t> held;
    REQUIRE(held.assign(8, 0));
    CHECK_FALSE(set_bulk_memory(&b));  // would free these eight bytes to `b`
    CHECK(&bulk_memory() == &a);
  }
  CHECK(set_bulk_memory(&b));
  CHECK(set_bulk_memory(nullptr));
  CHECK(&bulk_memory() == &system_bulk_memory());
}
