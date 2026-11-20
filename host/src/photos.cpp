#include "photos.h"

#include "png_io.h"

namespace hearthmap::host {

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"  // lv_image_header_t bit-fields
#endif
const lv_image_dsc_t* DirectoryPhotos::plant_photo(std::string_view photo_id) const {
  auto it = cache_.find(photo_id);
  if (it == cache_.end()) {
    auto e = std::make_unique<Entry>();
    std::vector<std::uint8_t> rgba;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    const std::string path = dir_ + "/" + std::string(photo_id) + ".png";
    if (read_png_rgba(path, rgba, w, h).empty() && w > 0 && h > 0 && w < 0x10000 / 4) {
      e->argb.resize(rgba.size());
      for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {  // RGBA -> LVGL's B, G, R, A
        e->argb[i + 0] = rgba[i + 2];
        e->argb[i + 1] = rgba[i + 1];
        e->argb[i + 2] = rgba[i + 0];
        e->argb[i + 3] = rgba[i + 3];
      }
      e->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      e->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
      e->dsc.header.w = w;
      e->dsc.header.h = h;
      e->dsc.header.stride = w * 4U;
      e->dsc.data_size = static_cast<std::uint32_t>(e->argb.size());
      e->dsc.data = e->argb.data();
      e->ok = true;
    }
    it = cache_.emplace(std::string(photo_id), std::move(e)).first;
  }
  return it->second->ok ? &it->second->dsc : nullptr;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace hearthmap::host
