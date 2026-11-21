#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "hearthmap/ui/blitter.h"

using namespace hearthmap::ui;

namespace {

struct Img {
  int w, h, stride;
  std::vector<std::uint16_t> px;
  Img(int w_, int h_, int stride_ = 0, std::uint16_t fill = 0)
      : w(w_),
        h(h_),
        stride(stride_ ? stride_ : w_),
        px(static_cast<std::size_t>(stride) * static_cast<std::size_t>(h_), fill) {}
  std::size_t idx(int x, int y) const {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
           static_cast<std::size_t>(x);
  }
  Surface565 surface() { return {px.data(), w, h, stride}; }
  View565 view() const { return {px.data(), w, h, stride}; }
  std::uint16_t at(int x, int y) const { return px[idx(x, y)]; }
  std::uint16_t& at(int x, int y) { return px[idx(x, y)]; }
};

Img pattern(int w, int h) {
  Img i(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) i.at(x, y) = static_cast<std::uint16_t>(0x1000 + y * 64 + x);
  return i;
}

std::uint16_t rgb(unsigned r5, unsigned g6, unsigned b5) {
  return static_cast<std::uint16_t>((r5 << 11U) | (g6 << 5U) | b5);
}

}  // namespace

TEST_CASE("clip_blit: inside, partly outside on every edge, wholly outside") {
  Img d(10, 8);
  const Surface565 s = d.surface();
  BlitClip c = clip_blit(s, 2, 3, 4, 2);
  CHECK(c.x == 2);
  CHECK(c.y == 3);
  CHECK(c.w == 4);
  CHECK(c.h == 2);
  CHECK(c.src_x == 0);
  c = clip_blit(s, -3, -1, 5, 4);  // left and top cut
  CHECK(c.x == 0);
  CHECK(c.y == 0);
  CHECK(c.src_x == 3);
  CHECK(c.src_y == 1);
  CHECK(c.w == 2);
  CHECK(c.h == 3);
  c = clip_blit(s, 8, 6, 5, 5);  // right and bottom cut
  CHECK(c.w == 2);
  CHECK(c.h == 2);
  CHECK(clip_blit(s, 10, 0, 3, 3).empty());
  CHECK(clip_blit(s, -3, 0, 3, 3).empty());
  CHECK(clip_blit(s, 0, 8, 3, 3).empty());
  CHECK(clip_blit(s, 0, 0, 0, 3).empty());
  CHECK(clip_blit(Surface565{}, 0, 0, 3, 3).empty());
}

TEST_CASE("clip_blit at scale 2 skips whole source pixels") {
  Img d(10, 8);
  // x = -3: source pixels 0 and 1 cover x -3..0, so skip 2 and start at x = 1.
  const BlitClip c = clip_blit(d.surface(), -3, 0, 4, 2, 2);
  CHECK(c.src_x == 2);
  CHECK(c.x == 1);
  CHECK(c.w == 4);
  CHECK(c.h == 4);
}

TEST_CASE("copy writes the clipped block and nothing else, honouring both strides") {
  SoftwareBlitter b;
  Img d(8, 6, 11, 0xFFFF);
  Img src = pattern(5, 4);
  b.copy(d.surface(), -2, 3, src.view());  // clipped left and bottom
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 8; ++x) {
      const bool in = x < 3 && y >= 3;
      CHECK(d.at(x, y) == (in ? src.at(x + 2, y - 3) : 0xFFFF));
    }
  }
  // Padding past w in each destination row is untouched.
  for (int y = 0; y < 6; ++y)
    for (int x = 8; x < 11; ++x) CHECK(d.at(x, y) == 0xFFFF);
}

TEST_CASE("fill clips, and an empty or outside rectangle writes nothing") {
  SoftwareBlitter b;
  Img d(6, 4, 0, 0);
  b.fill(d.surface(), 4, -1, 5, 3, 0x1234);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 6; ++x) CHECK(d.at(x, y) == ((x >= 4 && y < 2) ? 0x1234 : 0));
  Img e(6, 4, 0, 7);
  b.fill(e.surface(), 6, 0, 3, 3, 1);
  b.fill(e.surface(), 0, 0, 0, 3, 1);
  for (auto p : e.px) CHECK(p == 7);
}

TEST_CASE("blend: endpoints, rounding to nearest per channel, each channel independent") {
  SoftwareBlitter b;
  Img src(1, 1, 0, rgb(31, 63, 31));
  {
    Img d(1, 1, 0, rgb(0, 0, 0));
    b.blend(d.surface(), 0, 0, src.view(), 0);
    CHECK(d.at(0, 0) == rgb(0, 0, 0));
    b.blend(d.surface(), 0, 0, src.view(), 255);
    CHECK(d.at(0, 0) == rgb(31, 63, 31));
  }
  {
    // 31 * 128 / 255 = 15.56 -> 16; 63 * 128 / 255 = 31.62 -> 32.
    Img d(1, 1, 0, rgb(0, 0, 0));
    b.blend(d.surface(), 0, 0, src.view(), 128);
    CHECK(d.at(0, 0) == rgb(16, 32, 16));
  }
  {
    // Only red differs: green and blue must stay exactly as they were.
    Img red(1, 1, 0, rgb(31, 10, 5));
    Img d(1, 1, 0, rgb(0, 10, 5));
    b.blend(d.surface(), 0, 0, red.view(), 64);
    // 31 * 64 / 255 = 7.78 -> 8
    CHECK(d.at(0, 0) == rgb(8, 10, 5));
  }
}

TEST_CASE("blend clips like copy") {
  SoftwareBlitter b;
  Img src(3, 3, 0, rgb(31, 0, 0));
  Img d(4, 4, 0, 0);
  b.blend(d.surface(), 2, 2, src.view(), 255);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) CHECK(d.at(x, y) == ((x >= 2 && y >= 2) ? rgb(31, 0, 0) : 0));
}

TEST_CASE("scale2x doubles every pixel and clips to whole source pixels at the edges") {
  SoftwareBlitter b;
  Img src = pattern(3, 2);
  Img d(7, 5, 0, 0xFFFF);
  b.scale2x(d.surface(), 0, 0, src.view());
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 7; ++x) {
      const bool in = x < 6 && y < 4;
      CHECK(d.at(x, y) == (in ? src.at(x / 2, y / 2) : 0xFFFF));
    }
  }
  // Odd negative origin: source column 0 covers x -1..0 and is skipped entirely, so x = 0 is
  // left alone and column 1 starts at x = 1.
  Img e(4, 2, 0, 0xFFFF);
  b.scale2x(e.surface(), -1, 0, src.view());
  CHECK(e.at(0, 0) == 0xFFFF);
  CHECK(e.at(1, 0) == src.at(1, 0));
  CHECK(e.at(2, 0) == src.at(1, 0));
  CHECK(e.at(3, 0) == src.at(2, 0));
  CHECK(e.at(3, 1) == src.at(2, 0));
  // Right edge cuts a source pixel in half: the left half is still drawn.
  Img f(3, 2, 0, 0xFFFF);
  b.scale2x(f.surface(), 0, 0, src.view());
  CHECK(f.at(2, 0) == src.at(1, 0));
}

TEST_CASE("a Blitter is usable through the interface") {
  SoftwareBlitter impl;
  Blitter& b = impl;
  Img d(2, 2, 0, 0);
  b.fill(d.surface(), 0, 0, 2, 2, 5);
  CHECK(d.at(1, 1) == 5);
}
