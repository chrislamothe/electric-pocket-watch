#pragma once

/*
 * Improv Wi-Fi over the USB Serial/JTAG port (https://www.improv-wifi.com/serial/), so the
 * ESP Web Tools page (or tools/wifi.py) can hand the watch its Wi-Fi credentials.
 * Installs the USB Serial/JTAG driver and routes the console through it; call first in app_main.
 */
void improv_start(void);
