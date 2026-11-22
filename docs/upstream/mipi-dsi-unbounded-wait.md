# Draft upstream report: unbounded waits on the ESP32-P4 display path

Status: a draft for the owner to file or not. Nothing here has been sent to ESPHome or
Espressif. Versions read: ESPHome 2026.11.0, ESP-IDF 5.5.6.

## 1. ESPHome `mipi_dsi`: the copy's completion is awaited with no timeout and no error check

`esphome/components/mipi_dsi/mipi_dsi.cpp`, `MipiDsi::write_to_display_`:

```cpp
err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y_start, x_start + w, y_start + h, ptr);
xSemaphoreTake(this->io_lock_, portMAX_DELAY);
```

The line-by-line branch does the same per row. `io_lock_` is a binary semaphore
(`xSemaphoreCreateBinary`, created empty) given only by the panel's `on_color_trans_done`
callback, which ESP-IDF calls when the DMA2D copy into the frame buffer completes.

If `esp_lcd_panel_draw_bitmap` returns an error, nothing will ever give the semaphore, and the
task waits forever. In ESP-IDF 5.5.6 `dpi_panel_draw_bitmap` (`esp_lcd/dsi/esp_lcd_panel_dpi.c`) can
return an error before starting a copy: `ESP_ERR_INVALID_STATE` when `draw_sem` is not available
("previous draw operation is not finished"), or the error of `esp_async_fbcpy` (for example a
failed `dma2d_enqueue`). Because the call runs on ESPHome's main loop (LVGL's flush), a hang here
freezes the device until the task watchdog resets it.

There is a second way to lose the completion (section 2), which this wait also cannot survive.

A timeout alone is not a fix, and an earlier draft of this section proposed one: returning after
a timed-out wait lets ESPHome call `lv_display_flush_ready()`, so LVGL may rewrite the source
buffer while a copy that is merely late is still reading it; and a late completion still gives
`io_lock_` (and ESP-IDF's `draw_sem`), so the next transfer's wait consumes the previous
transfer's signal and returns before its own copy is done. A late completion and a lost one
need different handling, and from the caller's side they look the same until the late one
arrives.

Proposed change (behaviour unchanged on success): wait with a timeout, and on a timeout stop
rather than carry on.

```diff
--- a/esphome/components/mipi_dsi/mipi_dsi.cpp
+++ b/esphome/components/mipi_dsi/mipi_dsi.cpp
@@ void MipiDsi::write_to_display_(int x_start, int y_start, int w, int h, const uint8_t *ptr, int x_offset, int y_offset,
   if (x_offset == 0 && x_pad == 0) {
     err = esp_lcd_panel_draw_bitmap(this->handle_, x_start, y_start, x_start + w, y_start + h, ptr);
-    xSemaphoreTake(this->io_lock_, portMAX_DELAY);
-
+    if (err == ESP_OK && xSemaphoreTake(this->io_lock_, pdMS_TO_TICKS(MIPI_DSI_DRAW_TIMEOUT_MS)) != pdTRUE) {
+      this->draw_timed_out_();  // never returns the buffer: see below
+      return;
+    }
   } else {
     // draw line by line (each row's wait the same way)
```

where `draw_timed_out_()` logs, counts, and marks the display failed (`mark_failed()`), and the
flush path then never calls `lv_display_flush_ready()` for that buffer and starts no further
transfer: LVGL stops drawing into a buffer a copy may still read (it waits for the flush that
never completes; with ESPHome's LVGL that freezes the screen, not the device), and no later
wait can be satisfied by the stale completion. `MIPI_DSI_DRAW_TIMEOUT_MS` about 100 ms (a
full-screen copy takes about 15 ms on this panel). The device stays up, logs, and can be reached
or restarted from a higher level. Resuming drawing safely needs one of: a way to cancel the copy
(ESP-IDF 5.5.6's `esp_async_fbcpy` has none), or a late completion observed (the semaphore given
after the timeout) followed by draining `io_lock_` back to empty before the next transfer; a lost
completion (section 2) never gives it, so only the fail-stop is safe for both. The two cases
should be tested separately: a completion delayed past the timeout, and one never delivered.

## 2. ESP-IDF DMA2D: the completion callback is skipped when a transfer ends in error

`esp_hw_support/dma/dma2d.c`, `_dma2d_default_rx_isr`: on `RX_SUC_EOF`, `RX_ERR_EOF`,
`RX_DESC_ERROR` or `RX_DESC_EMPTY` the ISR frees the transaction's channels, but it calls the
user's `on_recv_eof` only when `RX_SUC_EOF` is set. Users that wait for that callback with no
timeout wait forever after an errored transfer:

- `esp_lcd` DPI `draw_bitmap` (via `esp_async_fbcpy`): `draw_sem` is never given back, and the
  caller's own wait (section 1) never ends;
- `esp_driver_ppa` blocking operations: `ppa_do_operation` waits on `xSemaphoreTake(trans_elm->sem,
  portMAX_DELAY)` (`ppa_core.c`), given only from the same callback path.

Proposed for Espressif: deliver an error event to the users (or call `on_recv_eof` with an error
flag in `dma2d_event_data_t`) for every freed transaction, so drivers can release their
semaphores and report the error.

We have not observed an errored DMA2D transfer on the panel; this is a gap found by reading the
code, recorded because the panel does reset with its main loop blocked (`docs/decisions.md`
D-062 adds a monitor to name the wait the next time).

## What hearthmap does meanwhile

- `DisplayPresenter` (its one call into `draw_pixels_at`) checks every precondition that would make
  `draw_bitmap` refuse before calling it: a failed display, an empty or off-panel block, a call
  from another task. It cannot protect ESPHome's own LVGL flush.
- `PpaBlitter` uses only blocking PPA calls whose arguments the driver validates before starting a
  transaction (those errors return at once and fall back to software).
- The residual risk, an errored transfer after it started, lives in ESPHome's flush and in ESP-IDF;
  the stall monitor (D-062) reports it when it happens.
