#include "internal_ram_guard.h"

#include <algorithm>

namespace esphome::hearthmap_panel {

InternalRamGuard::InternalRamGuard(std::size_t floor_bytes, std::uint32_t warn_interval_ms)
    : floor_(floor_bytes), warn_interval_ms_(warn_interval_ms) {}

std::size_t InternalRamGuard::margin() const {
  return std::max<std::size_t>(floor_ / 4U, 8U * 1024U);
}

InternalRamGuard::Verdict InternalRamGuard::update(std::uint32_t now_ms,
                                                   std::size_t internal_free) {
  Verdict v;
  if (floor_ == 0) return v;
  if (!low_) {
    if (internal_free >= floor_) {
      last_ok_free_ = internal_free;
      return v;
    }
    low_ = true;
    ++spells_;
    last_warn_ms_ = now_ms;
    v.pause = true;
    v.warn = true;
    v.fell_from = last_ok_free_;
    return v;
  }
  if (internal_free >= floor_ + margin()) {
    low_ = false;
    last_ok_free_ = internal_free;
    v.recovered = true;
    return v;
  }
  v.pause = true;
  v.fell_from = last_ok_free_;
  if (now_ms - last_warn_ms_ >= warn_interval_ms_) {
    last_warn_ms_ = now_ms;
    v.warn = true;
  }
  return v;
}

bool InternalRamGuard::affords(std::size_t internal_free, std::size_t bytes) const {
  if (internal_free < bytes) return false;
  const std::size_t left = internal_free - bytes;
  return floor_ == 0 || left >= floor_ + margin();
}

}  // namespace esphome::hearthmap_panel
