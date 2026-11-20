// png_io.h — PNG in and out for the host (lodepng stays inside png_io.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hearthmap::host {

struct RgbImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgb;  ///< 3 bytes per pixel, row-major.
};

/// RGB565 (native integer, R high) -> 8-bit RGB, replicating high bits into low bits.
RgbImage from_rgb565(const std::vector<std::uint16_t>& pixels, std::uint32_t width,
                     std::uint32_t height);

/// Write an RGB PNG. Returns an error message, or empty on success.
std::string write_png(const std::string& path, const RgbImage& image);

/// Read a PNG as RGB (alpha dropped). Returns an error message, or empty on success.
std::string read_png(const std::string& path, RgbImage& out);

/// Read a PNG as 8-bit RGBA. Returns an error message, or empty on success.
std::string read_png_rgba(const std::string& path, std::vector<std::uint8_t>& rgba,
                          std::uint32_t& width, std::uint32_t& height);

}  // namespace hearthmap::host
