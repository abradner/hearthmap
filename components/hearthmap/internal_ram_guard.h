// internal_ram_guard.h — warns, and pauses sprite builds, when the panel's internal RAM falls
// below a floor (docs/decisions.md D-064). It cannot hold RAM above the floor: anything else may
// keep taking it.
//
// On the ESP32-P4 the hosted Wi-Fi driver takes a DMA-capable frame from internal RAM for every
// packet it sends or receives. When internal RAM runs out the link dies with nothing in the log,
// and the panel keeps running as if nothing were wrong. The UI keeps its bulk storage in PSRAM
// (hearthmap/ui/memory.h), so it should never be the cause; this guard is the check that it is
// not, and the brake if something is: below the floor the component logs a warning (at once,
// then at most once per interval while it lasts) and stops building pulse sprites, so animation
// degrades and the network does not. It resumes once free RAM is back above the floor plus a
// margin (no flapping at the line).
//
// Pure: no ESPHome, no heap calls. The component samples heap_caps and acts on the verdict.
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::hearthmap_panel {

class InternalRamGuard {
 public:
  /// What to do after a sample.
  struct Verdict {
    bool pause = false;      ///< Sprite builds should be paused now.
    bool warn = false;       ///< Log the low-memory warning now (rate-limited).
    bool recovered = false;  ///< Free RAM came back above floor + margin: say so, once.
    /// Internal free at the last sample before it went low (0 if none): what it fell from.
    std::size_t fell_from = 0;
  };

  /// `floor_bytes` 0 disables the guard (it never pauses or warns). Warnings repeat at most
  /// every `warn_interval_ms` while free RAM stays below the floor.
  explicit InternalRamGuard(std::size_t floor_bytes, std::uint32_t warn_interval_ms = 60000);

  Verdict update(std::uint32_t now_ms, std::size_t internal_free);

  bool low() const { return low_; }
  std::size_t floor() const { return floor_; }
  /// Free RAM must reach floor() + margin() to end a low spell.
  std::size_t margin() const;
  /// Low spells since start-up.
  std::uint32_t spells() const { return spells_; }

 private:
  std::size_t floor_;
  std::uint32_t warn_interval_ms_;
  bool low_ = false;
  std::uint32_t last_warn_ms_ = 0;
  std::size_t last_ok_free_ = 0;
  std::uint32_t spells_ = 0;
};

}  // namespace esphome::hearthmap_panel
