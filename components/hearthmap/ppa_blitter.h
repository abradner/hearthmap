// ppa_blitter.h — hm::ui::Blitter on the ESP32-P4's Pixel Processing Accelerator (PPA).
//
// Target-specific: compiled only for the P4 (USE_ESP32_VARIANT_ESP32P4); every other target, and
// the host, uses ui_lvgl's SoftwareBlitter. Selected by the YAML option `blitter: ppa`; the
// default is `software` (docs/decisions.md D-053).
//
// Each operation goes to the PPA only when it is worth it and allowed, else to SoftwareBlitter:
//   - at least `min_pixels` destination pixels (below that the PPA's fixed cost per call loses
//     to the CPU; the default comes from the bench, examples/bench/README.md);
//   - destination buffer start and size whole cache lines (64 B), as the PPA driver requires;
//   - every buffer in internal RAM or PSRAM (the PPA's DMA cannot read flash, where generated
//     photos live).
// Cache: the driver writes back its inputs, but for SRM it only invalidates the output rows, so
// CPU writes not yet written back in those rows would be lost; every call writes the destination
// rows back first (examples/bench/README.md, "Cache trap").
#pragma once

#ifdef USE_ESP32_VARIANT_ESP32P4

#include <cstdint>

#include "driver/ppa.h"
#include "hearthmap/ui/blitter.h"

namespace esphome::hearthmap_panel {

class PpaBlitter final : public ::hearthmap::ui::SoftwareBlitter {
 public:
  explicit PpaBlitter(uint32_t min_pixels) : min_pixels_(min_pixels) {}
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
  bool do_srm_(const ::hearthmap::ui::Surface565& dst, const ::hearthmap::ui::BlitClip& c,
               const ::hearthmap::ui::View565& src, float scale);

  uint32_t min_pixels_;
  ppa_client_handle_t srm_ = nullptr;
  ppa_client_handle_t blend_ = nullptr;
  ppa_client_handle_t fill_ = nullptr;
  uint32_t hw_calls_ = 0;
  uint32_t sw_calls_ = 0;
};

}  // namespace esphome::hearthmap_panel

#endif  // USE_ESP32_VARIANT_ESP32P4
