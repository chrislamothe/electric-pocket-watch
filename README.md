# Electric Pocket Watch

Firmware that turns Waveshare's ESP32-S3 AMOLED boards into a pocket watch: classic analog
dials, screen off until you wake it, Wi-Fi only for setting the clock. Supported boards:
**ESP32-S3-Touch-AMOLED-1.75C**, **-1.75 / -1.75-B**, and **-2.06**.

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
- **Buttons, plus touch in Settings**: the watch is operated with BOOT and PWR. Touch works
  only while the Settings menu is open; the touch controller sleeps the rest of the time. The
  accelerometer and audio chips are not used and are powered down to save battery.
- **The screen stays off until woken**: press BOOT or PWR. It stays lit 10 s after the last
  button press (adjustable).
- **Battery indicator**: twelve segments at the edge of the glass, one per hour. 12–1 is the first
  segment and 11–12 the last, so a full battery lights all twelve. When only 12–1 and 1–2 are left
  (about 17 % or less) they turn red. While charging the segments are blue; a full green ring means
  charging is complete.
- **Charging**: plugging in turns the screen on, and it stays on while USB power is connected. After
  the normal screen-on time it dims to 20 %; a button press brightens it again (the first
  BOOT press only brightens). PWR turns it off as usual. Can be turned off in Display settings.
- **Time sync over Wi-Fi**: the watch syncs once at power-on and whenever you ask. It uses your saved
  network, or an open network if allowed. Wi-Fi is off the rest of the time.
- **Settings on the watch**: double-press BOOT; press BOOT to move the highlight, hold it to
  select. Live Wi-Fi status, brightness, screen-on time, battery indicator, restart and factory
  reset. The Wi-Fi network itself is saved from the install web page over USB.
- **Recovers from hangs**: a watchdog reboots the watch if the firmware ever locks up.

## Hardware

| Board (build ID) | Display | Touch | Clock chip | Status |
|---|---|---|---|---|
| ESP32-S3-Touch-AMOLED-1.75C (`175c`) | 1.75" 466×466 round, CO5300 | CST9217 | none | Tested on hardware |
| ESP32-S3-Touch-AMOLED-1.75 / 1.75-B / 1.75-G (`175`) | 1.75" 466×466 round, CO5300 | CST9217 | PCF85063 | Built from Waveshare's schematic and board package; not yet tested on hardware |
| ESP32-S3-Touch-AMOLED-2.06 (`206`) | 2.06" 410×502 rectangle, CO5300 | FT3168 | PCF85063 | Built from Waveshare's schematic and factory firmware; not yet tested on hardware |

All three: ESP32-S3R8 (8 MB octal PSRAM; the firmware uses 16 MB of flash), AXP2101 PMIC
(battery voltage, USB detect, software power-off), QMI8658 IMU (not used; powered down).
The 1.75-B is the standard 1.75 in a protective case and runs the `175` firmware; the 1.75-G
(GNSS) does too, without using its GPS. Each board needs its own firmware. On the 2.06 the round
dials are drawn in the 410×410 square centred on the screen.

Board differences the firmware handles (details in [boards/README.md](boards/README.md) and each
`boards/<id>/board/include/board.h`):

- **PWR button**: GPIO3 on the 1.75C, GPIO10 on the 2.06, but on the 1.75 / 1.75-B only through
  the TCA9554 I/O expander, whose interrupt is not wired to the ESP32. There the PWR button can't
  wake the chip directly, so while the screen is off the watch checks it every 200 ms.
- **IMU interrupt**: GPIO21 everywhere, but it carries INT2 on the 1.75 / 1.75-B (INT1 goes to
  the expander), so wake-on-motion is routed to INT2 there.
- **Clock chip**: the 1.75 / 1.75-B and 2.06 have a PCF85063 on the AXP2101's always-on RTC
  supply. The watch sets the system clock from it at power-on and writes it after every time sync,
  so they keep the time while powered off.
- **2.06 board package**: Waveshare's registry package (v2.0.0) uses a different LVGL layer with an
  incompatible lock signature and registers the QSPI panel as an RGB panel. The 2.06 uses a local
  package (`boards/206/board/bsp_206.c`) built from the corrected copy in Waveshare's 2.06 factory
  firmware, with the same LVGL setup as the 1.75 boards.

## Installing

### From the browser (recommended)

1. Open <https://chrislamothe.github.io/electric-pocket-watch/> in Chrome or Edge on a computer.
2. Plug the watch in with a USB-C **data** cable and click the button for **your board**. Pick
   the port that shows as *USB JTAG/serial debug unit*.
3. Choose **Install**. When it finishes, the page asks for your Wi-Fi network and password. The
   watch joins once, sets its clock and turns Wi-Fi off again.

Installing again erases saved settings and Wi-Fi. Browser USB access isn't available on iOS or
Android.

### From the command line

Each CI run publishes one artifact per board, `firmware-<id>` (`175c`, `175`, `206`), holding a
merged image flashed at offset 0.

```sh
gh run download <run-id> -R chrislamothe/electric-pocket-watch -n firmware-206 -D fw
esptool.py --chip esp32s3 -p COM3 -b 460800 write_flash 0x0 fw/firmware-206.bin
```

## Using the watch

| Action | Result |
|---|---|
| **BOOT** or **PWR** press (screen off) | Wake |
| **BOOT** press (screen on) | Next face (single press is read after ~0.35 s, to allow a double press) |
| **BOOT** double press | Open or close Settings |
| **BOOT** hold ~1 s | Sync the time now |
| In Settings: **BOOT** press / hold | Highlight the next item / select it |
| **PWR** press | Screen off |
| **PWR** hold 3 s | Power off ("Hold to power off", then "Powering off"). Press PWR or plug in USB to power on. |

### Settings

- **Wi-Fi**
  - Live status: radio state, saved network, last sync time and result.
  - **Set time now**
  - **Use open Wi-Fi**: on by default.
  - **Forget network**
- **Display**: brightness (20 / 40 / 60 / 80 / 100 %), screen-on time (5 / 10 / 20 / 30 s / always), on while
  charging (default on), battery indicator on/off.
- **System**: version, battery, **Restart**, **Factory reset** (select twice).

The Wi-Fi network is set from the install page over USB: **Connect watch → Change Wi-Fi**.

### Time

The 1.75C has no RTC chip, so its clock is lost on power-off; the 1.75 / 1.75-B and 2.06 keep
time in their PCF85063 (see Hardware). The watch syncs from `pool.ntp.org` about 3 s after
power-on and whenever you hold BOOT or use **Set time now**.
A sync scans first and joins the saved network if it's in range. If that network isn't available,
the watch tries up to three open networks, if allowed. Networks with a login page (hotels, cafés)
usually block the time server; the watch reports that and moves on. Time zone: US Pacific with
automatic DST.

## Building from source

Requires **ESP-IDF v5.5.5**.

Pick the board with `-D BOARD=175c` (default), `175` or `206`; use one build directory per board.

```sh
idf.py -B build-206 -D BOARD=206 set-target esp32s3
idf.py -B build-206 -D BOARD=206 build
idf.py -B build-206 -p COM3 flash monitor
```

Each board has its own component lock file (`dependencies.<id>.lock`). CI
(`.github/workflows/build.yml`) builds all three boards in the `espressif/idf:v5.5.5` container,
prints the key `sdkconfig` values so an option that gets ignored is visible, and uploads the
merged binaries. On `main` it also publishes `site/` to GitHub Pages (ESP Web Tools) with one
install button and manifest per board; `manifest.json` / `firmware.bin` stay the 1.75C build for
older links.

Useful `idf.py menuconfig` options are under **Electric Pocket Watch**:

| Option | Default | Notes |
|---|---|---|
| `WATCH_AWAKE_MS` | 10000 | Default screen-on time (Settings overrides it) |
| `WATCH_SLEEP_ON_USB` | n | With USB power the watch idles instead of light-sleeping, so the serial log stays up |

### Project layout

```
boards/<id>/board/  per-board pins, PWR button, board package (see boards/README.md)
main/
  main.c            boot sequence, boot-time sync
  controller.c      screen on/off, buttons, light sleep and wake sources
  faces.c           face manager, battery indicator, status messages
  face_analog.c     Roman, Arabic, Black
  face_calendar.c   triple calendar / moon phase
  face_railroad.c   Railroad and Railroad night
  settings_ui.c     on-watch Settings menu
  net.c             Wi-Fi join/scan, SNTP, open-network fallback
  improv.c          Improv Wi-Fi over USB serial (used by the install page)
  battlog.c         battery log in its own flash partition, CSV export
  display_dma.c     preallocated strip buffer for display transfers
  splash.c          boot screen, "Battery empty" message
  ext_rtc.c         PCF85063 clock chip (1.75 / 1.75-B, 2.06)
  display.c         panel + LVGL start-up without a touch input device
  parked.c          powers down the unused touch, accelerometer and audio chips
  axp.c buttons.c settings.c watch_time.c i2c_dev.c
  fonts/            Abril Fatface digits for the railroad dials (generated with lv_font_conv)
site/               ESP Web Tools install page and manifest
tools/wifi.py       command-line Improv client, for debugging
```

### Board notes

| Signal | 1.75C (verified on hardware) | 1.75 / 1.75-B | 2.06 |
|---|---|---|---|
| I²C bus (SCL/SDA) | 14 / 15 | 14 / 15 | 14 / 15 |
| I²C devices | AXP2101 0x34, QMI8658 0x6B, CST9217 0x5A, ES8311 0x18, ES7210 0x40 | same + TCA9554 0x20, PCF85063 0x51 | AXP2101 0x34, QMI8658 0x6B, FT3168 0x38, PCF85063 0x51, ES8311 0x18, ES7210 0x40 |
| BOOT button | GPIO0, active low | GPIO0 | GPIO0 |
| PWR key (SYS_OUT, high while held) | GPIO3 | TCA9554 P4 | GPIO10 |
| IMU wake interrupt | GPIO21 = INT1 | GPIO21 = INT2 (INT1 on TCA9554 P6) | GPIO21 = INT1 |
| LCD reset / touch reset | GPIO1 / GPIO2 | GPIO39 / GPIO40 | GPIO8 / GPIO9 |
| Speaker amplifier enable | GPIO46 | GPIO46 | GPIO46 |
| Other | | RTC INT on TCA9554 P3 | vibration motor GPIO18, RTC INT GPIO39 |
| AXP2101 IRQ | not routed | TCA9554 P5 | not routed |

- The QMI8658 keeps power across ESP32 resets, so it is soft-reset before being powered down.
- Light sleep uses per-pin GPIO wake, which allows the mixed polarity of the wake pins (BOOT low,
  PWR high).
- **Display memory**: Waveshare's BSP streams the PSRAM draw buffers to the panel through
  ~46 KB internal-RAM bounce buffers. Wi-Fi/lwIP buffers are moved to PSRAM and 128 KB of internal
  RAM is reserved. The SPI driver used to allocate a fresh internal-RAM copy for every strip;
  when that failed the strip was dropped and the panel kept stale pixels (ghosts, wrong battery
  segments). Since v0.8.5 the watch allocates one strip buffer at boot and copies each strip into
  it through the adapter's draw-bitmap callback (`main/display_dma.c`), so no allocation happens
  per frame. The Wi-Fi driver is also unloaded after every sync or scan.

## Status and known limits

- **Battery** (measured on the 1.75C): screen off, full to empty, **75.1 h on v0.11.1**
  (48.5 h on v0.8.6); about 6 h with the screen always on (v0.8.x). Not yet re-measured since
  panel deep standby (v0.13.0). Charging from 20 % takes about 1 h 45 min. The percentage
  comes from a voltage curve measured on that cell; other boards use the same curve (single-cell
  LiPo shape) until measured, so expect a few points of error there. Below 3.35 V the watch shows "Battery empty" and powers
  itself off; USB or the PWR key turns it on again.
- **Unused hardware** (v0.11.0): the accelerometer and both audio chips are powered down over I2C
  and the speaker amplifier is held off (`main/parked.c`). The touch controller sleeps except
  while Settings is open; its sleep command is verified and retried (`main/touch.c`). No pin is
  latched with gpio_hold.
- **Screen-off power**: while the screen is off the display panel is in deep standby (its memory
  is lost; on wake it is reset, re-initialised and redrawn before it lights, about 0.2 s) and
  the processor is in light sleep until BOOT or PWR. Settings > System shows how much of the
  screen-off time the processor spent in light sleep (not the current drawn), and any failed
  panel or touch power transitions.
- **Battery log**: Settings > System > Send log over USB prints the log as CSV (one record per
  15 min, every 2 min below 3.5 V, with reset and power-off reasons at each boot).
- **1.75 / 1.75-B**: PWR is not on a wake-capable pin, so the watch wakes once a second to read
  it; the PMIC latches presses in between, so a short tap still wakes it (within 1 s). BOOT wakes
  at once.
- **Timekeeping** (`main/clock.c`, `main/clock_rtc.c`, v0.15.0, rebuilt from scratch): the time is
  an anchor (a UTC instant measured at an `esp_timer` instant) plus elapsed `esp_timer` time. Awake
  that is crystal-timed; across light sleep ESP-IDF bridges it with the internal RC oscillator,
  which is not trusted. Anchors come only from measurements: the PCF85063 (read before every
  screen wake is drawn, then refined on its seconds rollover, and at least hourly) or the network
  (SNTP's time hook is overridden, so SNTP hands over a measurement instead of setting the clock;
  the RTC is written from it and verified). Every reference also measures the sleep clock's error,
  which is stored and removed after each sleep - the 1.75C, without an RTC, relies on that and on a
  daily automatic sync with a saved network. Before any measurement the faces say "Time not set".
  Settings > System shows the source, RTC state, and the measured sleep-clock error.
- Planned: hinged-lid opening animation, time-zone setting, alarm with chime.

## License

MIT, see [LICENSE](LICENSE). Third-party components keep their own licenses (below); the Abril
Fatface font is under the SIL Open Font License.

## Credits and third-party licenses

- [LVGL](https://lvgl.io) 9.5: MIT
- [Waveshare BSP](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75c): Apache-2.0
- [Abril Fatface](https://fonts.google.com/specimen/Abril+Fatface): SIL Open Font License
  (`main/fonts/OFL-AbrilFatface.txt`)
- [ESP Web Tools](https://github.com/esphome/esp-web-tools) and the
  [Improv Wi-Fi](https://www.improv-wifi.com/) protocol
- [ESP-IDF](https://github.com/espressif/esp-idf): Apache-2.0
