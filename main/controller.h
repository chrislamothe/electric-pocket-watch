#pragma once

/* Screen-on loop (buttons, face switching, inactivity timeout) and the screen-off sleep/wake cycle
 * (BOOT, PWR and IMU wake-on-motion + double-tap confirmation). Never returns. */
void controller_run(void);

/* Power the board off through the PMIC when the battery is empty: below BATTLOG_EMPTY_MV at rest
 * (or the screen-on equivalent) on two readings in a row, never on USB. The boot check covers a
 * press of PWR on an empty battery: it shows the message and powers off again. */
void controller_check_empty_at_boot(void);
