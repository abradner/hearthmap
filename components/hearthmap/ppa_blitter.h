// ppa_blitter.h — hm::ui::Blitter on the ESP32-P4's Pixel Processing Accelerator (PPA).
//
// Target-specific: compiled only for the P4 (USE_ESP32_VARIANT_ESP32P4); every other target, and
// the host, uses ui_lvgl's SoftwareBlitter. Selected by the YAML option `blitter: ppa`; the
// default is `software` (docs/decisions.md D-053).
//
// Each operation goes to the PPA only when the bench showed it wins and when it is allowed, else
// to SoftwareBlitter (examples/bench/README.md has the numbers behind each threshold):
//   - fill: at least kFillMinPx destination pixels (the PPA costs about 46 us a call, then fills
//     at 178 Mpx/s; the CPU fills at 25-70 Mpx/s);
//   - blend: at least kBlendMinPx (PPA 19-44 Mpx/s against 6-8 for the C loop);
//   - copy: at least kCopyMinPx, through the blend engine with a fully transparent foreground
//     (44-48 Mpx/s from PSRAM; the CPU and the PPA's scale-rotate engine both manage about
//     21, and the CPU wins below this size because small sources sit in the cache);
//   - scale2x: never. The PPA's scaler filters between pixels (up to 21 steps per channel away
//     from pixel doubling on the bench), which breaks the Blitter contract.
// Every PPA call also needs a destination buffer whose start and size are whole 64-byte cache
// lines, and buffers in internal RAM or PSRAM (the PPA reads flash, where generated photos live,
// at about 4 Mpx/s).
// Cache: the driver writes back its inputs but only invalidates an output it does not also read,
// so CPU writes not yet written back in the output rows, outside the block, would be lost (the
// bench lost 16,104 of 16,384 such pixels through a scale-rotate call). A fill writes back and
// invalidates; a blend reads its destination as the background, so it is written back. A copy
// (destination not read) writes the destination rows back itself first.
#pragma once

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <cstdint>

#include "driver/ppa.h"
#include "hearthmap/ui/blitter.h"

namespace esphome::hearthmap_panel {

class PpaBlitter final : public ::hearthmap::ui::SoftwareBlitter {
 public:
  /// Smallest blocks (destination pixels) that go to the PPA; below them the CPU is faster.
  static constexpr uint32_t kFillMinPx = 16384;
  static constexpr uint32_t kBlendMinPx = 4096;
  static constexpr uint32_t kCopyMinPx = 65536;

  /// `force_hw`: send every call the PPA allows to it, whatever its size (bench only).
  explicit PpaBlitter(bool force_hw = false) : force_hw_(force_hw) {}
  ~PpaBlitter() override;
  PpaBlitter(const PpaBlitter&) = delete;
  PpaBlitter& operator=(const PpaBlitter&) = delete;

  /// Registers the PPA clients. False (and every call stays in software) if the driver refuses.
  bool begin();

  void copy(const ::hearthmap::ui::Surface565& dst, int x, int y,
            const ::hearthmap::ui::View565& src) override;
  void fill(const ::hearthmap::ui::Surface565& dst, int x, int y, int w, int h,
            std::uint16_t colour) override;
  void blend(const ::hearthmap::ui::Surface565& dst, int x, int y,
             const ::hearthmap::ui::View565& src, std::uint8_t alpha) override;
  void scale2x(const ::hearthmap::ui::Surface565& dst, int x, int y,
               const ::hearthmap::ui::View565& src) override;

  uint32_t hw_calls() const { return hw_calls_; }
  uint32_t sw_calls() const { return sw_calls_; }

 private:
  bool dst_ok_(const ::hearthmap::ui::Surface565& dst) const;
  static bool src_ok_(const ::hearthmap::ui::View565& src);
  void writeback_rows_(const ::hearthmap::ui::Surface565& dst, int y, int h) const;
  bool big_enough_(const ::hearthmap::ui::BlitClip& c, uint32_t min_px) const;

  bool force_hw_;
  ppa_client_handle_t blend_ = nullptr;
  ppa_client_handle_t fill_ = nullptr;
  uint32_t hw_calls_ = 0;
  uint32_t sw_calls_ = 0;
};

}  // namespace esphome::hearthmap_panel

#endif  // USE_ESP32_VARIANT_ESP32P4
