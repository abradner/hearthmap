// memory.h — the memory port for the UI's bulk storage (docs/decisions.md D-064).
//
// Every buffer the UI sizes by pixels, cells or pulses (sprite frames and captures, coverage,
// keyline and distance tables, the bake, the rasterizer's tables, photo and leaf images, and the
// containers that hold them) is allocated through one port the embedder owns, never through
// plain `new` or `malloc`. On the panel the port is PSRAM (heap_caps_malloc with
// MALLOC_CAP_SPIRAM), and a failed allocation is reported, never retried elsewhere: on the
// ESP32-P4, ESP-IDF's CONFIG_SPIRAM_USE_MALLOC puts every default-heap block under 16 KiB in
// internal RAM, and a UI making many 4-12 KiB buffers there starves the hosted Wi-Fi driver of
// the DMA-capable RAM it needs for its frames. On the host the port is plain malloc.
//
// Three ways to hold port memory:
//   - BulkBuffer<T>: an owning array of a trivially copyable T whose resize() and assign() return
//     false when the port has no memory, so a caller can degrade (a pulse that does not animate)
//     instead of stopping. Use it for anything sized by pixels.
//   - BulkVector<T>: std::vector over BulkAllocator, for the containers of structures that hold
//     those buffers. A vector cannot fail softly with exceptions off (D-002), so the allocator
//     reports through the failure handler and aborts, as `new` would; keep such vectors to
//     sizes known at construction.
//   - make_bulk<T>(...): one object in port memory (BulkPtr<T>), with the same failure rule;
//     try_make_bulk<T>(...) returns an empty BulkPtr instead, for objects made after start-up.
//
// The port is process-wide (LVGL is too). Install it before the first bulk allocation, keep it
// alive while anything holds its memory, and allocate from one task: set_bulk_memory() refuses
// a change while memory is held, so nothing is ever freed to a port that did not allocate it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace hearthmap::ui {

/// The embedder's memory. Implementations never fall back to another heap.
class BulkMemory {
 public:
  virtual ~BulkMemory() = default;
  /// `bytes` (> 0) aligned to `align` (a power of two), or nullptr.
  virtual void* allocate(std::size_t bytes, std::size_t align) = 0;
  /// `p` from allocate() with the same `bytes`; nullptr is ignored.
  virtual void deallocate(void* p, std::size_t bytes) = 0;
};

/// The system heap (aligned malloc and free): the host's port, and the one in place until an
/// embedder installs its own.
BulkMemory& system_bulk_memory();
/// The installed port.
BulkMemory& bulk_memory();
/// Installs `memory` (nullptr: system_bulk_memory()). Refused (false, nothing changes) while
/// any memory taken through the port is still held.
bool set_bulk_memory(BulkMemory* memory);

/// What has gone through the port since start-up (or the last set_bulk_memory()).
struct BulkStats {
  std::size_t in_use = 0;      ///< bytes held now
  std::size_t peak = 0;        ///< the most ever held at once
  std::uint32_t failures = 0;  ///< allocations the port refused
  std::size_t last_failed_bytes = 0;
};
BulkStats bulk_stats();

/// Through the installed port, counted in bulk_stats(); nullptr when refused.
void* bulk_allocate(std::size_t bytes, std::size_t align);
void bulk_free(void* p, std::size_t bytes);

/// Called with the size before an allocation that cannot fail softly aborts (BulkAllocator,
/// make_bulk). The embedder logs it; the default does nothing.
using BulkFailureHandler = void (*)(std::size_t bytes);
void set_bulk_failure_handler(BulkFailureHandler handler);
[[noreturn]] void bulk_exhausted(std::size_t bytes);

/// A stateless std allocator over the port; equal to every other.
template <class T>
struct BulkAllocator {
  using value_type = T;
  BulkAllocator() noexcept = default;
  template <class U>
  BulkAllocator(const BulkAllocator<U>& /*other*/) noexcept {}  // NOLINT: an allocator rebinds
  T* allocate(std::size_t n) {
    if (n > static_cast<std::size_t>(-1) / sizeof(T)) bulk_exhausted(static_cast<std::size_t>(-1));
    void* p = bulk_allocate(n * sizeof(T), alignof(T));
    if (p == nullptr) bulk_exhausted(n * sizeof(T));
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t n) noexcept { bulk_free(p, n * sizeof(T)); }
  template <class U>
  bool operator==(const BulkAllocator<U>& /*other*/) const noexcept {
    return true;
  }
  template <class U>
  bool operator!=(const BulkAllocator<U>& /*other*/) const noexcept {
    return false;
  }
};

template <class T>
using BulkVector = std::vector<T, BulkAllocator<T>>;

/// An owning array of `size()` elements in port memory, exactly as many as asked for (so its
/// size is what it holds). Move-only. Elements are trivially copyable and never constructed or
/// destroyed one by one.
template <class T>
class BulkBuffer {
  static_assert(std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value,
                "BulkBuffer holds plain data");

 public:
  BulkBuffer() noexcept = default;
  ~BulkBuffer() { release(); }
  BulkBuffer(BulkBuffer&& o) noexcept : p_(o.p_), n_(o.n_) {
    o.p_ = nullptr;
    o.n_ = 0;
  }
  BulkBuffer& operator=(BulkBuffer&& o) noexcept {
    if (this != &o) {
      release();
      p_ = o.p_;
      n_ = o.n_;
      o.p_ = nullptr;
      o.n_ = 0;
    }
    return *this;
  }
  BulkBuffer(const BulkBuffer&) = delete;
  BulkBuffer& operator=(const BulkBuffer&) = delete;

  /// Exactly `n` elements; their values are unspecified unless the size was already `n`, when
  /// they are kept. False, leaving the buffer empty, when the port refuses.
  [[nodiscard]] bool resize(std::size_t n) {
    if (n == n_) return true;
    release();
    if (n == 0) return true;
    if (n > static_cast<std::size_t>(-1) / sizeof(T)) return false;
    void* p = bulk_allocate(n * sizeof(T), alignof(T));
    if (p == nullptr) return false;
    p_ = static_cast<T*>(p);
    n_ = n;
    return true;
  }
  /// resize(n), then every element `value`. By value: `value` may be one of this buffer's own
  /// elements, which resize() frees.
  [[nodiscard]] bool assign(std::size_t n, T value) {
    if (!resize(n)) return false;
    std::fill_n(p_, n_, value);
    return true;
  }
  /// Gives the memory back to the port.
  void release() noexcept {
    if (p_ != nullptr) bulk_free(p_, n_ * sizeof(T));
    p_ = nullptr;
    n_ = 0;
  }
  void swap(BulkBuffer& o) noexcept {
    std::swap(p_, o.p_);
    std::swap(n_, o.n_);
  }

  T* data() noexcept { return p_; }
  const T* data() const noexcept { return p_; }
  std::size_t size() const noexcept { return n_; }
  std::size_t bytes() const noexcept { return n_ * sizeof(T); }
  bool empty() const noexcept { return n_ == 0; }
  T* begin() noexcept { return p_; }
  T* end() noexcept { return p_ + n_; }
  const T* begin() const noexcept { return p_; }
  const T* end() const noexcept { return p_ + n_; }
  T& operator[](std::size_t i) noexcept { return p_[i]; }
  const T& operator[](std::size_t i) const noexcept { return p_[i]; }

 private:
  T* p_ = nullptr;
  std::size_t n_ = 0;
};

template <class T>
struct BulkDelete {
  void operator()(T* p) const noexcept {
    if (p == nullptr) return;
    p->~T();
    bulk_free(p, sizeof(T));
  }
};
template <class T>
using BulkPtr = std::unique_ptr<T, BulkDelete<T>>;

/// One T in port memory (reported and aborted, like `new`, when the port refuses).
template <class T, class... Args>
BulkPtr<T> make_bulk(Args&&... args) {
  void* m = bulk_allocate(sizeof(T), alignof(T));
  if (m == nullptr) bulk_exhausted(sizeof(T));
  return BulkPtr<T>(new (m) T(std::forward<Args>(args)...));
}

/// One T in port memory, or an empty BulkPtr when the port refuses (counted, not aborted): for
/// objects made after start-up, whose caller has a fallback.
template <class T, class... Args>
BulkPtr<T> try_make_bulk(Args&&... args) {
  void* m = bulk_allocate(sizeof(T), alignof(T));
  if (m == nullptr) return BulkPtr<T>();
  return BulkPtr<T>(new (m) T(std::forward<Args>(args)...));
}

}  // namespace hearthmap::ui
