// display_presenter.h — hm::ui::Presenter on an ESPHome display (docs/decisions.md D-058).
//
// Undoes LVGL's software rotation of the tile on the CPU into a small internal-RAM buffer, then
// hands it to Display::draw_pixels_at(), the call the lvgl component's own flush ends in. On the
// panel this is 0.08 ms for a 32 px tile and 0.17 ms for 64 px, against 0.7-1.2 ms for an LVGL
// refresh of the same area (examples/bench/README.md, "Tiles"). Rotating straight into the DSI
// frame buffer was measured too and is slower (per-row cache maintenance), and needs the panel's
// private handle, so it is not used.
//
// Refuses (returns false, so the caller invalidates instead) when: not called from the task that
// ran begin() (ESPHome's main loop, LVGL's thread); the tile is larger than the buffer; the
// rotation is 180 degrees (not needed by the panel; a 0, 90 or 270 rotation is handled); or the
// tile is not wholly on screen. Portable: nothing here is specific to the ESP32-P4.
#pragma once

#include <cstdint>
#include <memory>

#include "esphome/components/display/display.h"
#include "hearthmap/ui/present.h"

namespace esphome::hearthmap_panel {

class DisplayPresenter final : public ::hearthmap::ui::Presenter {
 public:
  /// Largest tile accepted, in pixels (a 128 x 128 tile; 32 KiB of internal RAM).
  static constexpr int kMaxTilePx = 128 * 128;

  DisplayPresenter(display::Display* display, display::DisplayRotation rotation)
      : display_(display), rotation_(rotation) {}
  ~DisplayPresenter() override;
  DisplayPresenter(const DisplayPresenter&) = delete;
  DisplayPresenter& operator=(const DisplayPresenter&) = delete;

  /// Allocates the rotation buffer and records the calling task as the only one allowed to
  /// present. Call from setup(). False if the buffer cannot be had (then every present fails).
  bool begin();

  bool present(int x, int y, const ::hearthmap::ui::View565& tile) override;

  uint32_t presented() const { return presented_; }
  uint32_t refused() const { return refused_; }

 private:
  display::Display* display_;
  display::DisplayRotation rotation_;
  uint16_t* buf_ = nullptr;
  void* task_ = nullptr;
  uint32_t presented_ = 0;
  uint32_t refused_ = 0;
};

}  // namespace esphome::hearthmap_panel
