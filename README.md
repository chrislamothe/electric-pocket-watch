# Electric Pocket Watch

Firmware that turns the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** into a pocket watch: classic
analog dials on a 466×466 round AMOLED, screen off until you wake it, Wi-Fi only for setting the
clock.

**Install it from your browser:** <https://chrislamothe.github.io/electric-pocket-watch/>
(Chrome or Edge on a computer, USB-C data cable.)

---

## Features

- **Six dials**, all running to the edge of the glass:
  - **Roman**: roman numerals with a running-seconds sub-dial at 6.
  - **Arabic**: arabic numerals with a red centre seconds hand.
  - **Black**: black dial, large cream numerals, an outer 5–60 minute ring and silver hands.
  - **Calendar**: triple calendar with a moon phase. It has day and month windows, a 1–31 date ring read by a brass pointer, a live moon-phase window at 6 and a red seconds hand.
  - **Railroad**: cream enamel, heavy serif numerals, spade hands and a seconds sub-dial.
  - **Railroad night**: the railroad dial inverted to black, the easiest on an AMOLED battery.
- **The screen stays off until woken**: press BOOT or PWR, or double-tap the case. It stays lit
  10 s after the last touch (adjustable).
- **Battery indicator**: twelve segments at the edge of the glass, one per hour. 12–1 is the first
  segment and 11–12 the last, so a full battery lights all twelve. When only 12–1 and 1–2 are left
  (about 17 % or less) they turn red.
- **Time sync over Wi-Fi**: the watch syncs once at power-on and whenever you ask. It uses your saved
  network, or an open network if allowed. Wi-Fi is off the rest of the time.
- **Settings on the watch**: Wi-Fi setup with a network list and on-screen keyboard, live Wi-Fi
  status, brightness, screen-on time, battery indicator, dial theme, restart and factory reset.
- **Recovers from hangs**: a watchdog reboots the watch if the firmware ever locks up.

## Hardware

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.75C (ESP32-S3 with octal PSRAM; the firmware uses 16 MB of flash) |
| Display | 1.75" 466×466 AMOLED, CO5300 over QSPI, CST9217 touch |
| PMIC | AXP2101 (battery %, USB detect, PWR long-press power-off) |
| IMU | QMI8658 (wake-on-motion for double-tap) |

The standard (non-"C") 1.75 board has different wiring and is **not** supported.

## Installing

### From the browser (recommended)

1. Open <https://chrislamothe.github.io/electric-pocket-watch/> in Chrome or Edge on a computer.
2. Plug the watch in with a USB-C **data** cable and click **Connect watch**. Pick the port that
   shows as *USB JTAG/serial debug unit*.
3. Choose **Install**. When it finishes, the page asks for your Wi-Fi network and password. The
   watch joins once, sets its clock and turns Wi-Fi off again.

Installing again erases saved settings and Wi-Fi. Browser USB access isn't available on iOS or
Android.

### From the command line

Each CI run publishes a `firmware` artifact. `firmware-stage2.bin` is the full watch: a merged
image flashed at offset 0.

```sh
gh run download <run-id> -R chrislamothe/electric-pocket-watch -n firmware -D fw
esptool.py --chip esp32s3 -p COM3 -b 460800 write_flash 0x0 fw/firmware-stage2.bin
```

`firmware-stage1.bin` is a bring-up build: faces only, screen always on, no sleep or Wi-Fi. It's
useful for checking the display or measuring screen power.

## Using the watch

| Action | Result |
|---|---|
| **BOOT** press (screen on) | Next face (single press is read after ~0.35 s, to allow a double press) |
| **BOOT** double press | Open or close Settings |
| **BOOT** hold ~1 s | Sync the time now |
| **PWR** press | Screen off / on |
| **PWR** hold 6 s | Power off (handled by the PMIC, works even if the firmware is frozen). Press PWR to power on. |
| Swipe left / right | Next / previous face |
| Swipe up | Open Settings |
| Double-tap the case (screen off) | Wake |

### Settings

- **Wi-Fi**
  - Live status: radio state, saved network, last sync time and result.
  - **Choose network**: scan, tap a network, type the password.
  - **Set time now**
  - **Use open Wi-Fi**: on by default.
  - **Forget network**
- **Display**: brightness slider (5–100 %), screen-on time (5 / 10 / 20 / 30 s), battery indicator
  on/off.
- **Theme**: Brass / Silver / Dark, for the Roman and Arabic dials.
- **System**: version, battery, **Restart**, **Factory reset** (tap twice).

Wi-Fi can also be changed from the install page: **Connect watch → Change Wi-Fi**.

### Time

The board has no RTC chip or 32 kHz crystal, so the clock is lost on power-off. The watch syncs
from `pool.ntp.org` about 3 s after power-on and whenever you hold BOOT or use **Set time now**.
A sync scans first and joins the saved network if it's in range. If that network isn't available,
the watch tries up to three open networks, if allowed. Networks with a login page (hotels, cafés)
usually block the time server; the watch reports that and moves on. Time zone: US Pacific with
automatic DST.

## Building from source

Requires **ESP-IDF v5.5.5**.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

CI (`.github/workflows/build.yml`) builds both stages in the `espressif/idf:v5.5.5` container. It
prints the key `sdkconfig` values, so an option that gets ignored is visible, and it uploads the
merged binaries. On `main` it also publishes `site/` with the stage 2 firmware to GitHub Pages
(ESP Web Tools).

Useful `idf.py menuconfig` options are under **Electric Pocket Watch**:

| Option | Default | Notes |
|---|---|---|
| `WATCH_STAGE` | 1 | 2 = full watch (CI builds both) |
| `WATCH_AWAKE_MS` | 10000 | Default screen-on time (Settings overrides it) |
| `WATCH_SLEEP_ON_USB` | n | With USB power the watch idles instead of light-sleeping, so the serial log stays up |
| `WATCH_WOM_THRESHOLD_MG` | 200 | Wake-on-motion threshold |
| `WATCH_TAP_THRESHOLD_MG` | 600 | Second-tap peak for double-tap wake |
| `WATCH_TAP_WINDOW_MS` | 700 | Window for the second tap |

### Project layout

```
main/
  main.c            boot sequence, stage 1 loop, boot-time sync
  controller.c      screen on/off, buttons, light sleep and wake sources
  faces.c           face manager, battery indicator, status messages
  face_analog.c     Roman, Arabic, Black
  face_calendar.c   triple calendar / moon phase
  face_railroad.c   Railroad and Railroad night
  settings_ui.c     on-watch Settings menu
  net.c             Wi-Fi join/scan, SNTP, open-network fallback
  improv.c          Improv Wi-Fi over USB serial (used by the install page)
  imu.c axp.c buttons.c settings.c watch_time.c i2c_dev.c
  fonts/            Abril Fatface digits for the railroad dials (generated with lv_font_conv)
components/watch_bsp/
  local copy of Waveshare's BSP 3.0.0 with psram_dma_direct enabled (see below)
site/               ESP Web Tools install page and manifest
tools/wifi.py       command-line Improv client, for debugging
```

### Board notes (verified on hardware)

| Signal | Pin / address |
|---|---|
| I²C bus | SCL 14, SDA 15. AXP2101 0x34, QMI8658 0x6B, CST9217 0x5A, ES8311 0x18, ES7210 0x40 |
| BOOT button | GPIO0, active low |
| PWR key | GPIO3 (SYS_OUT through a FET), high while held; the PMIC also sees it |
| IMU INT1 | GPIO21 (INT2 is not connected) |
| AXP2101 IRQ | not routed to the ESP32, so the PMIC is polled |

- The QMI8658 keeps power across ESP32 resets and is soft-reset on every mode change. Accelerometer
  data freezes after leaving wake-on-motion without a reset.
- Light sleep uses per-pin GPIO wake, which allows the mixed polarity of the wake pins (BOOT low,
  PWR high, IMU on change).
- **Display fix**: the upstream BSP's QSPI panel I/O left `psram_dma_direct` off. Every pixel
  transfer from the PSRAM draw buffers then needed a ~46 KB internal-RAM bounce buffer. When that
  allocation failed (internal RAM is shared with Wi-Fi), the strip was dropped and the panel kept
  stale pixels. `components/watch_bsp` turns the flag on.

## Status and known limits

- Not yet measured: sleep current, real battery life, clock drift between syncs.
- Double-tap sensitivity is a first tuning.
- Planned: hinged-lid opening animation, time-zone setting, alarm with chime.

## Credits and third-party licenses

- [LVGL](https://lvgl.io) 9.5: MIT
- [Waveshare ESP32 components](https://github.com/waveshareteam/waveshare-esp32-components) BSP:
  Apache-2.0 (`components/watch_bsp/LICENSE`)
- [Abril Fatface](https://fonts.google.com/specimen/Abril+Fatface): SIL Open Font License
  (`main/fonts/OFL-AbrilFatface.txt`)
- [ESP Web Tools](https://github.com/esphome/esp-web-tools) and the
  [Improv Wi-Fi](https://www.improv-wifi.com/) protocol
- [ESP-IDF](https://github.com/espressif/esp-idf): Apache-2.0
