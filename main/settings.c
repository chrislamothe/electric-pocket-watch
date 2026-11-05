#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static nvs_handle_t s_nvs;
static bool s_ok;

void settings_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e == ESP_OK) {
        e = nvs_open("watch", NVS_READWRITE, &s_nvs);
    }
    s_ok = (e == ESP_OK);
    if (!s_ok) {
        ESP_LOGE(TAG, "NVS unavailable: %s", esp_err_to_name(e));
    }
}

int settings_get_int(const char *key, int def)
{
    int32_t v = def;
    if (s_ok) {
        nvs_get_i32(s_nvs, key, &v);
    }
    return (int)v;
}

void settings_set_int(const char *key, int value)
{
    if (s_ok && nvs_set_i32(s_nvs, key, value) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

bool settings_get_str(const char *key, char *buf, size_t len)
{
    if (!s_ok) {
        return false;
    }
    size_t n = len;
    return nvs_get_str(s_nvs, key, buf, &n) == ESP_OK;
}

void settings_set_str(const char *key, const char *value)
{
    if (s_ok && nvs_set_str(s_nvs, key, value) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}
