/*
 * Electric Pocket Watch - milestone 1
 *
 * Board: Waveshare ESP32-S3-Touch-AMOLED-1.75C. Screen off until woken by BOOT (GPIO0),
 * PWR (GPIO3) or a double tap (QMI8658 wake-on-motion on GPIO21, confirmed in software).
 * Faces: classic analog and minimal digital; BOOT short press or a swipe changes face.
 *
 * The bring-up/diagnostic firmware this replaces is tagged diag-v0.0.8 in git.
 */

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "axp.h"
#include "buttons.h"
#include "controller.h"
#include "faces.h"
#include "imu.h"
#include "watch_time.h"

static const char *TAG = "pocketwatch";

void app_main(void)
{
    ESP_LOGI(TAG, "Electric Pocket Watch milestone 1 (built %s %s)", __DATE__, __TIME__);

    ESP_ERROR_CHECK(bsp_i2c_init());
    lv_display_t *disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }
    bsp_display_brightness_set(0);              /* stay dark until the first face is drawn */

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!axp_init(bus)) {
        ESP_LOGE(TAG, "PMIC not available: battery and USB state unknown");
    }
    if (!imu_init(bus)) {
        ESP_LOGE(TAG, "IMU not available: double-tap wake disabled");
    }

    watch_time_init();
    buttons_init();

    if (bsp_display_lock(1000) == ESP_OK) {
        faces_init();
        bsp_display_unlock();
    }

    controller_run();
}
