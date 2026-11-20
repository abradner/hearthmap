#include "generated_photos.h"

#include <cstdint>

#include "hearthmap_gen/photos.h"

namespace hearthmap_device {

void GeneratedPhotos::init() {
  if (initialised_) return;  // descriptors already handed out must not move
  photos_.reserve(hearthmap_gen::kPhotoCount);
  for (unsigned i = 0; i < hearthmap_gen::kPhotoCount; ++i) {
    const hearthmap_gen::PhotoData& p = hearthmap_gen::kPhotos[i];
    lv_image_dsc_t d{};
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.w = p.size;
    d.header.h = p.size;
    d.header.stride = static_cast<std::uint32_t>(p.size) * 2U;
    d.data_size = static_cast<std::uint32_t>(p.size) * p.size * 2U;
    d.data = p.pixels;
    photos_.push_back(d);
  }
  initialised_ = true;
}

const lv_image_dsc_t* GeneratedPhotos::plant_photo(std::string_view photo_id) const {
  if (!initialised_) return nullptr;
  // The table lists each photo's sizes largest first, so the first match is the largest.
  for (unsigned i = 0; i < hearthmap_gen::kPhotoCount; ++i) {
    if (photo_id == hearthmap_gen::kPhotos[i].id) return &photos_[i];
  }
  return nullptr;
}

const lv_image_dsc_t* GeneratedPhotos::plant_photo_sized(std::string_view photo_id,
                                                         int diameter) const {
  if (!initialised_) return nullptr;
  for (unsigned i = 0; i < hearthmap_gen::kPhotoCount; ++i) {
    const hearthmap_gen::PhotoData& p = hearthmap_gen::kPhotos[i];
    if (p.size == diameter && photo_id == p.id) return &photos_[i];
  }
  return nullptr;
}

}  // namespace hearthmap_device
