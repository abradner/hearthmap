// phrase_matcher.h — finds a fixed phrase in a stream fed one character at a time (KMP), for
// counting a driver's console message that has no other hook (dsi_underrun_tap.h, D-067).
//
// Pure, header-only, and free of library calls: feed() runs inside the console's character
// output, which can be an interrupt handler running while the flash cache is off, so it must
// inline into its caller (kept in IRAM there) and touch only this object, which lives in RAM.
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::hearthmap_panel {

/// `N` is the phrase's length without its terminating NUL (at most 255).
template <std::size_t N>
class PhraseMatcher {
  static_assert(N > 0 && N < 256, "a short phrase");

 public:
  explicit constexpr PhraseMatcher(const char (&phrase)[N + 1]) {
    for (std::size_t i = 0; i < N; ++i) phrase_[i] = phrase[i];
    // fail_[i]: the length of the longest proper prefix of phrase[0..i] that is also its suffix.
    std::size_t k = 0;
    for (std::size_t i = 1; i < N; ++i) {
      while (k > 0 && phrase_[i] != phrase_[k]) k = fail_[k - 1];
      if (phrase_[i] == phrase_[k]) ++k;
      fail_[i] = static_cast<std::uint8_t>(k);
    }
  }

  /// True when `c` completes the phrase. Overlapping occurrences each count.
  __attribute__((always_inline)) inline bool feed(char c) {
    std::size_t k = matched_;
    while (k > 0 && c != phrase_[k]) k = fail_[k - 1];
    if (c == phrase_[k]) ++k;
    if (k == N) {
      matched_ = fail_[N - 1];
      return true;
    }
    matched_ = static_cast<std::uint8_t>(k);
    return false;
  }

 private:
  char phrase_[N] = {};
  std::uint8_t fail_[N] = {};
  std::uint8_t matched_ = 0;
};

}  // namespace esphome::hearthmap_panel
