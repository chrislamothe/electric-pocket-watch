#pragma once

/* Screen-on loop (buttons, face switching, inactivity timeout) and the screen-off sleep/wake cycle
 * (BOOT, PWR and IMU wake-on-motion + double-tap confirmation). Never returns. */
void controller_run(void);
