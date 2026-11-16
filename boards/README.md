# Board support

One directory per supported board. The build includes exactly one of them
(`boards/<id>/board`, the component `board`; top-level `CMakeLists.txt`, `-D BOARD=<id>`), so
the rest of the firmware is the same for every board.

| ID | Board | Display | Board package |
|---|---|---|---|
| `175c` | Waveshare ESP32-S3-Touch-AMOLED-1.75C | 466×466 round, CO5300 | `waveshare/esp32_s3_touch_amoled_1_75c` |
| `175` | Waveshare ESP32-S3-Touch-AMOLED-1.75, -1.75-B (with case), -1.75-G | 466×466 round, CO5300 | `waveshare/esp32_s3_touch_amoled_1_75` |
| `206` | Waveshare ESP32-S3-Touch-AMOLED-2.06 | 410×502 rectangle, CO5300 (SH8601 command set) | local (`boards/206/board/bsp_206.c`) |

Each board provides:

- `include/board.h`: pins and capabilities the firmware needs beyond the board package
  (PWR button, IMU interrupt, touch reset, speaker amplifier, motor).
- `board.c`: `board_init()` and `board_pwr_down()`.
- The `bsp/esp-bsp.h` API used by the firmware: `bsp_i2c_init()`, `bsp_i2c_get_handle()`,
  `bsp_display_start()` (LVGL through `esp_lvgl_adapter` 0.6.x), `bsp_display_lock()` returning
  `esp_err_t`, `bsp_display_unlock()`, `bsp_display_brightness_set()`. The 1.75C and 1.75 use
  Waveshare's packages; the 2.06 package from the registry (v2.0.0) uses `esp_lvgl_port` with a
  different lock signature and registers its QSPI panel as an RGB panel, so the 2.06 has its own
  implementation here, built from Waveshare's corrected copy in the 2.06 factory firmware.

Pin sources: Waveshare schematics and board packages, cross-checked against the product wiki
and the 1.75 `HARDWARE_REFERENCE.md`.
