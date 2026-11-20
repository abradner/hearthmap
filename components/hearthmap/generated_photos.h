// generated_photos.h — the build's plant photos behind ui_lvgl's ImageProvider.
//
// Depends on LVGL's types only (no ESPHome), so it is compiled and tested on the host against a
// stand-in lvgl.h (tests/test_cpp.py).
#pragma once

#include <string_view>
#include <vector>

#include "hearthmap/ui/ports.h"
#include "lvgl.h"

namespace hearthmap_device {

/// ImageProvider contract (ui_lvgl/include/hearthmap/ui/ports.h): descriptors and pixels stay
/// valid until the Screen is destroyed, and the Screen asks for each id once, at construction.
/// The pixels are static tables; the descriptors live in a vector filled once by init(), which
/// must run before the Screen is built. A second init() is a no-op, so a descriptor handed out
/// can never move.
class GeneratedPhotos final : public hearthmap::ui::ImageProvider {
 public:
  void init();
  bool initialised() const { return initialised_; }
  /// nullptr for an id with no photo (ui_lvgl then draws its placeholder leaf), and for every
  /// id before init().
  const lv_image_dsc_t* plant_photo(std::string_view photo_id) const override;

 private:
  bool initialised_ = false;
  std::vector<lv_image_dsc_t> photos_;
};

}  // namespace hearthmap_device
