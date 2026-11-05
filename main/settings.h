#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Persistent settings in NVS namespace "watch". Note: flashing the merged image at 0x0 also
 * rewrites the NVS area, so settings and Wi-Fi credentials reset on every full flash. */
void settings_init(void);
int  settings_get_int(const char *key, int def);
void settings_set_int(const char *key, int value);
bool settings_get_str(const char *key, char *buf, size_t len);
void settings_set_str(const char *key, const char *value);
void settings_erase(const char *key);
void settings_factory_reset(void);   /* erase all saved settings and Wi-Fi, then reboot */
