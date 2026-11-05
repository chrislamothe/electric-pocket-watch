#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "settings.h"

static const char *TAG = "net";

#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
#define CONNECT_TIMEOUT_MS 15000
#define OPEN_CONNECT_TIMEOUT_MS 8000
#define OPEN_MAX_TRIES     3
#define NTP_TIMEOUT_MS     10000
#define MAX_RETRIES        3

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_ev;
static bool s_inited;
static volatile bool s_want_connect;
static int s_retries;

const char *net_result_text(net_result_t r)
{
    switch (r) {
    case NET_OK:             return "Time synced";
    case NET_OK_OPEN:        return "Time synced (open Wi-Fi)";
    case NET_NO_CREDENTIALS: return "No Wi-Fi set up";
    case NET_CONNECT_FAILED: return "Wi-Fi failed";
    case NET_NTP_FAILED:     return "Time server failed";
    case NET_BUSY:           return "Busy";
    }
    return "?";
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_want_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_want_connect && s_retries++ < MAX_RETRIES) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static bool ensure_init(void)
{
    if (s_inited) {
        return true;
    }
    esp_err_t e = esp_netif_init();
    if (e == ESP_OK) {
        e = esp_event_loop_create_default();
        if (e == ESP_ERR_INVALID_STATE) {
            e = ESP_OK;                                 /* already created */
        }
    }
    if (e == ESP_OK && !esp_netif_create_default_wifi_sta()) {
        e = ESP_FAIL;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (e == ESP_OK) e = esp_wifi_init(&cfg);
    if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e == ESP_OK) e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(e));
        return false;
    }
    s_inited = true;
    return true;
}

static bool wifi_join(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ensure_init()) {
        return false;
    }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    xEventGroupClearBits(s_ev, BIT_GOT_IP | BIT_FAILED);
    s_retries = 0;
    s_want_connect = true;
    ESP_LOGI(TAG, "connecting to '%s'", ssid);
    if (esp_wifi_start() != ESP_OK) {
        s_want_connect = false;
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    bool ok = (bits & BIT_GOT_IP) != 0;
    ESP_LOGI(TAG, "connect %s", ok ? "ok" : "failed");
    return ok;
}

static void radio_off(void)
{
    s_want_connect = false;
    if (s_inited) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
}

static bool sync_ntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        return false;
    }
    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_TIMEOUT_MS)) == ESP_OK;
    esp_netif_sntp_deinit();
    if (ok) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ESP_LOGI(TAG, "SNTP sync ok, epoch %lld", (long long)tv.tv_sec);
    } else {
        ESP_LOGW(TAG, "SNTP sync timed out");
    }
    return ok;
}

static net_result_t sync_via_open_locked(void);

void net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
}

net_result_t net_sync_time(void)
{
    char ssid[33], pass[65];
    bool have_saved = settings_get_str("ssid", ssid, sizeof(ssid));
    if (!settings_get_str("pass", pass, sizeof(pass))) {
        pass[0] = '\0';
    }
    bool allow_open = settings_get_int("open_wifi", 1) != 0;
    if (!have_saved && !allow_open) {
        return NET_NO_CREDENTIALS;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return NET_BUSY;
    }
    net_result_t r = have_saved ? NET_CONNECT_FAILED : NET_NO_CREDENTIALS;
    if (have_saved) {
        if (wifi_join(ssid, pass, CONNECT_TIMEOUT_MS)) {
            r = sync_ntp() ? NET_OK : NET_NTP_FAILED;
        }
        radio_off();
    }
    if (r != NET_OK && allow_open) {
        net_result_t o = sync_via_open_locked();
        if (o == NET_OK_OPEN || !have_saved) {
            r = o;
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

net_result_t net_provision(const char *ssid, const char *pass)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return NET_BUSY;
    }
    net_result_t r = NET_CONNECT_FAILED;
    if (wifi_join(ssid, pass, CONNECT_TIMEOUT_MS)) {
        settings_set_str("ssid", ssid);
        settings_set_str("pass", pass);
        r = sync_ntp() ? NET_OK : NET_NTP_FAILED;
    }
    radio_off();
    xSemaphoreGive(s_lock);
    return r;
}

/* Caller holds s_lock. */
static int scan_locked(net_ap_t *out, int max)
{
    int count = 0;
    if (!ensure_init()) {
        return 0;
    }
    s_want_connect = false;
    esp_wifi_start();
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = (uint16_t)max;
        wifi_ap_record_t *recs = calloc(max, sizeof(*recs));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                if (recs[i].ssid[0] == '\0') {
                    continue;
                }
                strlcpy(out[count].ssid, (const char *)recs[i].ssid, sizeof(out[count].ssid));
                out[count].rssi = recs[i].rssi;
                out[count].secured = recs[i].authmode != WIFI_AUTH_OPEN;
                count++;
            }
        }
        free(recs);
    }
    esp_wifi_stop();
    /* strongest first */
    for (int i = 1; i < count; i++) {
        net_ap_t t = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < t.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = t;
    }
    ESP_LOGI(TAG, "scan found %d networks", count);
    return count;
}

int net_scan(net_ap_t *out, int max)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(30000)) != pdTRUE) {
        return 0;
    }
    int count = scan_locked(out, max);
    xSemaphoreGive(s_lock);
    return count;
}

/* Caller holds s_lock. Try the strongest open networks; captive-portal networks (hotels, cafes)
 * usually block NTP until someone logs in, so they simply fail and the next one is tried. */
static net_result_t sync_via_open_locked(void)
{
    static net_ap_t aps[20];
    int n = scan_locked(aps, 20);
    int tried = 0;
    for (int i = 0; i < n && tried < OPEN_MAX_TRIES; i++) {
        if (aps[i].secured) {
            continue;
        }
        tried++;
        ESP_LOGI(TAG, "trying open network '%s' (%d dBm)", aps[i].ssid, aps[i].rssi);
        if (wifi_join(aps[i].ssid, "", OPEN_CONNECT_TIMEOUT_MS)) {
            bool ok = sync_ntp();
            radio_off();
            if (ok) {
                return NET_OK_OPEN;
            }
        } else {
            radio_off();
        }
    }
    return tried ? NET_CONNECT_FAILED : NET_NO_CREDENTIALS;
}
