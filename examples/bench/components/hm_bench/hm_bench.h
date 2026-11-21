// Benchmark component: times the pixel operations a UI needs on this board, with the CPU, LVGL's
// software renderer, the PPA, the AXI GDMA and the PIE SIMD extension, and LVGL scene repaints
// under several draw-buffer placements. One measurement per loop() call; results are INFO lines
// tagged "bench" (examples/bench/README.md).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/core/component.h"

namespace esphome::mipi_dsi {
class MipiDsi;
}

namespace esphome::hm_bench {

class HmBench : public Component {
 public:
  void set_lvgl(lvgl::LvglComponent* lvgl) { lvgl_ = lvgl; }
  void set_display(mipi_dsi::MipiDsi* display) { display_ = display; }
  void set_period_ms(uint32_t ms) { period_ms_ = ms; }
  void set_start_delay_ms(uint32_t ms) { start_delay_ms_ = ms; }
  void set_quick(bool quick) { quick_ = quick; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void build_steps_();
  void build_scene_();

  lvgl::LvglComponent* lvgl_{nullptr};
  mipi_dsi::MipiDsi* display_{nullptr};
  uint32_t period_ms_{30000};
  uint32_t start_delay_ms_{8000};
  bool quick_{false};
  uint32_t next_run_ms_{0};
  uint32_t run_{0};
  size_t step_{0};
  std::vector<std::function<void()>> steps_;
};

}  // namespace esphome::hm_bench
