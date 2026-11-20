// photos.h — plant photos from a directory: <dir>/<photo id>.png, as the editor exports them.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "hearthmap/ui/ports.h"

namespace hearthmap::host {

class DirectoryPhotos final : public ui::ImageProvider {
 public:
  explicit DirectoryPhotos(std::string dir) : dir_(std::move(dir)) {}
  /// The decoded photo, or nullptr when the file is missing or not a PNG (the leaf is drawn).
  const lv_image_dsc_t* plant_photo(std::string_view photo_id) const override;

 private:
  struct Entry {
    std::vector<std::uint8_t> argb;
    lv_image_dsc_t dsc{};
    bool ok = false;
  };
  std::string dir_;
  mutable std::map<std::string, std::unique_ptr<Entry>, std::less<>> cache_;
};

}  // namespace hearthmap::host
