#pragma once

#include <stdbool.h>

/*
 * On-watch settings: Wi-Fi network list + password keyboard, "set time now", open-Wi-Fi
 * fallback switch, display settings, system. Opened by a BOOT double press or a swipe up on the face.
 *
 * Functions without _nolock take the LVGL lock; _nolock variants are for LVGL callbacks.
 */
void settings_ui_init(void);            /* starts the network worker; call once */
void settings_ui_toggle(void);
void settings_ui_close(void);
void settings_ui_open_nolock(void);
bool settings_ui_is_open(void);
bool settings_ui_busy(void);            /* a scan/join/sync started from the menu is running */
