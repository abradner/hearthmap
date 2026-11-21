// present.h — showing the next frame of a small animated tile without an LVGL refresh.
//
// On the panel an LVGL refresh of one small area costs about 0.66 ms before a pixel is drawn
// (0.49 ms of LVGL's per-area work, 0.17 ms of flush set-up); rotating a 64 x 64 tile on the CPU
// and handing it to the display costs 0.17 ms in all (examples/bench/README.md, "Tiles"). A
// Presenter does the latter. It is only correct when the panel's pixels for the tile can come
// from the tile alone, so show_tile_frame() checks that first and otherwise falls back to an
// ordinary invalidation (docs/decisions.md D-058). The host and any target without a presenter
// use NoPresenter: always the fallback, so nothing changes but speed.
#pragma once

#include <cstdint>

#include "hearthmap/ui/blitter.h"
#include "lvgl.h"

namespace hearthmap::ui {

/// Puts an opaque RGB565 block on the panel at once, outside LVGL's refresh.
class Presenter {
 public:
  virtual ~Presenter() = default;
  /// `tile` as LVGL draws it (screen orientation, after LVGL's own rotation is undone by the
  /// implementation), top-left at screen pixel (x, y), wholly on screen. True: the panel shows
  /// exactly these pixels now. False: nothing was done, and the caller must invalidate.
  virtual bool present(int x, int y, const View565& tile) = 0;
};

/// Never presents. The host's Presenter, and every target's default.
class NoPresenter final : public Presenter {
 public:
  bool present(int x, int y, const View565& tile) override {
    (void)x;
    (void)y;
    (void)tile;
    return false;
  }
};

/// Why a tile cannot be presented (Ok when it can).
enum class PresentBlock : std::uint8_t {
  Ok,
  NotImage,           ///< null, not an lv_image, or its source is not `dsc`
  NotRgb565,          ///< the descriptor is not tightly packed RGB565
  Refreshing,         ///< called from inside an LVGL refresh
  NotOnActiveScreen,  ///< its screen is not the display's active screen
  Hidden,             ///< it or an ancestor is hidden
  NotOpaque,          ///< any opacity, image opacity or recolour below full along its ancestry
  Transformed,        ///< scaled, rotated, or offset by an image or style transform
  SizeMismatch,       ///< the object's area is not exactly the image (padding, border, size)
  Clipped,            ///< part of it is outside an ancestor or the display
  HasChildren,        ///< something is drawn inside it
  Covered,            ///< an object drawn after it (including the top and system layers)
                      ///< reaches into its area, with that object's extra draw size
};

/// The LVGL-side conditions for presenting `image`, whose source must be `dsc`. Updates the
/// layout first so coordinates are current. Limits: an ancestor's post-draw parts (scrollbars,
/// outlines drawn after children) are not checked; the UI's tile parents draw none.
PresentBlock present_check(lv_obj_t* image, const lv_image_dsc_t& dsc);

enum class TileShown : std::uint8_t { Presented, Invalidated };

/// Shows the next frame of an animated opaque tile. `image` is an lv_image whose source is
/// `dsc`, which the caller owns for the image's lifetime with a fixed header (RGB565, w, h,
/// stride w * 2). Points `dsc.data` at `frame` (w * h pixels, stride w) without invalidating,
/// then presents the frame when present_check() is Ok and the presenter succeeds; otherwise
/// invalidates the image so LVGL's next refresh draws it. Either way any later LVGL repaint of
/// that area draws `frame` too, so a stale frame never returns. Call from LVGL's thread (the main
/// loop), outside LVGL callbacks. `why`, when given, receives present_check()'s verdict.
TileShown show_tile_frame(lv_obj_t* image, lv_image_dsc_t& dsc, const std::uint16_t* frame,
                          Presenter& presenter, PresentBlock* why = nullptr);

}  // namespace hearthmap::ui
