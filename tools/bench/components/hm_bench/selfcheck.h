// Present self-check steps (selfcheck.cpp).
#pragma once

#include <functional>
#include <vector>

#include "esphome/components/lvgl/lvgl_esphome.h"

namespace esphome::mipi_dsi {
class MipiDsi;
}

namespace esphome::hm_bench {

void add_selfcheck_steps(std::vector<std::function<void()>>& steps, mipi_dsi::MipiDsi* display,
                         lvgl::LvglComponent* lvgl);

}  // namespace esphome::hm_bench
