// hearthmap_heat_dump: host-only eyeballing helper. Renders the example flat's temperature heat
// field to a PNG or PPM (by the output's extension) at the map area's size, one flat block per
// grid cell (no smoothing: this shows what core computed, not what ui_lvgl will draw), with walls
// and sensor dots drawn over it.
//
//   hearthmap_heat_dump OUT.png|OUT.ppm [all-ok|demo]
//
// all-ok: examples/flat/readings/all-ok.json's temperatures (fixtures in heat_fixtures.h).
// demo:   the same plus a cold corner in the living room and a hot oven corner in the kitchen,
//         with the bedroom sensor removed so its room shows the "no data" hatching.
// Colours are the UI spec's (background #0d1013, walls #4a535d, no-data hatching #6b7280,
// outdoor rooms in the garden fill #101a14: they get no heat colour, ruling R10).
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hearthmap/colour_scale.h"
#include "hearthmap/config.h"
#include "hearthmap/heat_field.h"
#include "heat_fixtures.h"

using namespace hearthmap;

namespace {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgb;  // row-major, 3 bytes per pixel

  void set(int x, int y, Rgb888 c) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) *
                    3U;
    rgb[at] = c.r;
    rgb[at + 1] = c.g;
    rgb[at + 2] = c.b;
  }
};

constexpr Rgb888 kBackground{0x0d, 0x10, 0x13};
constexpr Rgb888 kWall{0x4a, 0x53, 0x5d};
constexpr Rgb888 kNoData{0x6b, 0x72, 0x80};
constexpr Rgb888 kDot{0xe6, 0xed, 0xf3};
constexpr Rgb888 kOutdoor{0x10, 0x1a, 0x14};  // the UI spec's garden fill

void line(Image& img, int x0, int y0, int x1, int y1, Rgb888 c) {
  const int dx = std::abs(x1 - x0);
  const int dy = -std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1;
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    for (int ox = -1; ox <= 0; ++ox) {
      for (int oy = -1; oy <= 0; ++oy) img.set(x0 + ox, y0 + oy, c);  // 2 px stroke
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void dot(Image& img, int cx, int cy) {
  for (int y = -10; y <= 10; ++y) {
    for (int x = -10; x <= 10; ++x) {
      const int d2 = x * x + y * y;
      if (d2 <= 49)
        img.set(cx + x, cy + y, kDot);
      else if (d2 <= 100)
        img.set(cx + x, cy + y, kBackground);
    }
  }
}

Image render(const Config& config, const std::vector<HeatSample>& samples) {
  const RoomMask mask = build_room_mask(config);
  const HeatField field = compute_heat_field(config, mask, samples);
  // Colour is resolved once per heat cell through the band's lookup table (ruling R11), then
  // blown up to blocks: this is the per-cell path the panel uses, without its smoothing.
  const ColourTable table = make_colour_table(config.layer(Layer::Temperature).band);
  std::vector<Rgb888> cell_colour(field.cells.size());
  for (std::size_t i = 0; i < field.cells.size(); ++i) {
    cell_colour[i] = from_rgb565(table.lookup(field.cells[i].value));
  }
  Image img;
  img.width = config.canvas.map_area.width;
  img.height = config.canvas.map_area.height;
  img.rgb.assign(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 3U,
                 0);
  for (int y = 0; y < img.height; ++y) {
    const int r = y * field.rows / img.height;
    for (int x = 0; x < img.width; ++x) {
      const int c = x * field.cols / img.width;
      const auto at = static_cast<std::size_t>(r * field.cols + c);
      switch (field.cells[at].state) {
        case CellState::Outside: {
          const std::int16_t room = mask.room_index[at];
          const bool outdoor = room >= 0 && config.rooms[static_cast<std::size_t>(room)].outdoor;
          img.set(x, y, outdoor ? kOutdoor : kBackground);
          break;
        }
        case CellState::NoData:
          img.set(x, y, ((x + y) / 6) % 3 == 0 ? kNoData : kBackground);  // diagonal hatching
          break;
        case CellState::Value: img.set(x, y, cell_colour[at]); break;
      }
    }
  }
  const auto w = static_cast<double>(img.width);
  const auto h = static_cast<double>(img.height);
  for (const Room& room : config.rooms) {
    for (std::size_t i = 0, j = room.polygon.size() - 1; i < room.polygon.size(); j = i++) {
      line(img, static_cast<int>(room.polygon[j].x * w), static_cast<int>(room.polygon[j].y * h),
           static_cast<int>(room.polygon[i].x * w), static_cast<int>(room.polygon[i].y * h), kWall);
    }
  }
  for (const HeatSample& s : samples) {
    dot(img, static_cast<int>(s.position.x * w), static_cast<int>(s.position.y * h));
  }
  return img;
}

// --- minimal PNG writer: 8-bit RGB, zlib stream of stored (uncompressed) deflate blocks ------

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
  static std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
  return ~crc;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}

void chunk(std::FILE* f, const char* type, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> buf;
  put32(buf, static_cast<std::uint32_t>(data.size()));
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  put32(buf, crc32(buf.data() + 4, buf.size() - 4));
  std::fwrite(buf.data(), 1, buf.size(), f);
}

bool write_png(const Image& img, std::FILE* f) {
  static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::fwrite(kSignature, 1, sizeof kSignature, f);
  std::vector<std::uint8_t> header;
  put32(header, static_cast<std::uint32_t>(img.width));
  put32(header, static_cast<std::uint32_t>(img.height));
  header.insert(header.end(), {8, 2, 0, 0, 0});  // 8-bit, truecolour, deflate, no filter/interlace
  chunk(f, "IHDR", header);

  std::vector<std::uint8_t> raw;  // filter byte 0 + RGB per row
  const auto stride = static_cast<std::size_t>(img.width) * 3U;
  for (int y = 0; y < img.height; ++y) {
    raw.push_back(0);
    const auto* row = img.rgb.data() + static_cast<std::size_t>(y) * stride;
    raw.insert(raw.end(), row, row + stride);
  }
  std::vector<std::uint8_t> z{0x78, 0x01};
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (const std::uint8_t byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  for (std::size_t pos = 0; pos < raw.size();) {
    const std::size_t len = raw.size() - pos < 65535U ? raw.size() - pos : 65535U;
    const bool last = pos + len == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<std::uint8_t>(len & 0xFFU));
    z.push_back(static_cast<std::uint8_t>(len >> 8U));
    z.push_back(static_cast<std::uint8_t>(~len & 0xFFU));
    z.push_back(static_cast<std::uint8_t>((~len >> 8U) & 0xFFU));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
             raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
  }
  put32(z, (b << 16U) | a);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  return std::ferror(f) == 0;
}

bool write_ppm(const Image& img, std::FILE* f) {
  std::fprintf(f, "P6\n%d %d\n255\n", img.width, img.height);
  std::fwrite(img.rgb.data(), 1, img.rgb.size(), f);
  return std::ferror(f) == 0;
}

bool ends_with(const std::string& s, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "usage: %s OUT.png|OUT.ppm [all-ok|demo]\n", argv[0]);
    return 2;
  }
  const std::string out = argv[1];
  const std::string variant = argc == 3 ? argv[2] : "all-ok";
  const Config config = fixtures::example_flat_config();
  std::vector<HeatSample> samples = fixtures::example_flat_temperatures();
  if (variant == "demo") {
    samples.push_back({{0.04, 0.55}, 0, 16.0F});  // living room: cold corner by the balcony door
    samples.push_back({{0.64, 0.04}, 1, 28.0F});  // kitchen: the oven corner
    samples.erase(samples.begin() + 5);           // bedroom sensor gone: no data, not a guess
  } else if (variant != "all-ok") {
    std::fprintf(stderr, "unknown variant: %s\n", variant.c_str());
    return 2;
  }
  const Image img = render(config, samples);
  std::FILE* f = std::fopen(out.c_str(), "wb");
  if (f == nullptr) {
    std::perror(out.c_str());
    return 1;
  }
  const bool ok = ends_with(out, ".png") ? write_png(img, f) : write_ppm(img, f);
  return std::fclose(f) == 0 && ok ? 0 : 1;
}
