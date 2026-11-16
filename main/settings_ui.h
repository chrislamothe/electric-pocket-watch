#pragma once

#include <stdbool.h>

/*
 * On-watch settings, operated with the BOOT button: Wi-Fi status and "set time now", display
 * settings, system. Opened and closed by a BOOT double press; BOOT short moves the highlight,
 * BOOT hold activates the highlighted item.
 *
 * Functions without _nolock take the LVGL lock; _nolock variants are for LVGL callbacks.
 */
void settings_ui_init(void);            /* starts the network worker; call once */
void settings_ui_toggle(void);
void settings_ui_close(void);
void settings_ui_next(void);            /* highlight the next item (wraps) */
void settings_ui_select(void);          /* activate the highlighted item */
void settings_ui_open_nolock(void);
bool settings_ui_is_open(void);
bool settings_ui_busy(void);            /* a time sync started from the menu is running */
