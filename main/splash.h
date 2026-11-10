#pragma once

/* Boot screen: name, version and why the chip started (power on, restart, crash). Shown from
 * splash_show() until splash_hide(), for at least SPLASH_MIN_MS so a reboot is easy to notice. */
#define SPLASH_MIN_MS 1500

void splash_show(void);   /* call right after the display starts; takes the LVGL lock */
void splash_hide(void);   /* waits out SPLASH_MIN_MS, then removes it; takes the LVGL lock */

/* Full-screen message on the top layer (e.g. "Battery empty"); lights the screen. Takes the LVGL
 * lock, so LVGL must be running. */
void splash_message(const char *title, const char *detail);
