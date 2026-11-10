#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "settings.h"

static const char *TAG = "net";

#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
#define CONNECT_TIMEOUT_MS 15000            /* association + DHCP; public networks can be slow */
#define OPEN_MAX_TRIES     3
#define MAX_SCAN           20
#define NTP_TIMEOUT_MS     10000
#define MAX_RETRIES        3

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_ev;
static bool s_base_inited;           /* netif layer, event loop, our handlers: once */
static bool s_inited;                /* Wi-Fi driver + STA netif: only while a job runs */
static esp_netif_t *s_netif;
static volatile bool s_want_connect;
static int s_retries;
static net_status_t s_st;

static void record(net_result_t r, const char *ssid)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_st.have_result = true;
    s_st.last_result = r;
    strlcpy(s_st.last_ssid, ssid ? ssid : "", sizeof(s_st.last_ssid));
    s_st.last_attempt = tv.tv_sec;
    if (r == NET_OK || r == NET_OK_OPEN) {
        s_st.last_sync = tv.tv_sec;
    }
}

void net_get_status(net_status_t *out)
{
    *out = s_st;
}

void net_forget(void)
{
    settings_erase("ssid");
    settings_erase("pass");
}

const char *net_result_text(net_result_t r)
{
    switch (r) {
    case NET_OK:             return "Time synced";
    case NET_OK_OPEN:        return "Time synced (open Wi-Fi)";
    case NET_NO_CREDENTIALS: return "No Wi-Fi set up";
    case NET_CONNECT_FAILED: return "Wi-Fi failed";
    case NET_NTP_FAILED:     return "Time server failed";
    case NET_NOT_FOUND:      return "No usable network in range";
    case NET_OPEN_BLOCKED:   return "Open Wi-Fi joined, internet blocked";
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
        if (!s_want_connect) {
            return;                                 /* our own disconnect/stop, or a stale event */
        }
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGI(TAG, "disconnected, reason %d", d ? d->reason : -1);
        if (s_retries++ < MAX_RETRIES) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, BIT_FAILED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_st.radio = NET_RADIO_CONNECTED;
        xEventGroupSetBits(s_ev, BIT_GOT_IP);
    }
}

static void log_dma(const char *when)
{
    ESP_LOGI(TAG, "%s: internal DMA free %u (largest %u)", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
}

/* The Wi-Fi driver holds internal RAM (buffers, task stack) that the display's DMA bounce buffers
 * also need, so it is loaded for each job and unloaded afterwards (wifi_release). */
static bool ensure_init(void)
{
    if (s_inited) {
        return true;
    }
    esp_err_t e = ESP_OK;
    if (!s_base_inited) {
        e = esp_netif_init();
        if (e == ESP_OK) {
            e = esp_event_loop_create_default();
            if (e == ESP_ERR_INVALID_STATE) {
                e = ESP_OK;                             /* already created */
            }
        }
        if (e == ESP_OK) e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "network stack init failed: %s", esp_err_to_name(e));
            return false;
        }
        s_base_inited = true;
    }
    log_dma("before Wi-Fi");
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "Wi-Fi netif create failed");
        return false;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e == ESP_OK) {
        e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_STA);
        if (e != ESP_OK) {
            esp_wifi_deinit();
        }
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(e));
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
        return false;
    }
    s_inited = true;
    return true;
}

/* Unload the Wi-Fi driver and its netif. Call with s_lock held, at the end of each job. */
static void wifi_release(void)
{
    s_want_connect = false;
    if (!s_inited) {
        return;
    }
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));                 /* let the event loop drain stop events */
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(s_netif);
    s_netif = NULL;
    s_inited = false;
    s_st.radio = NET_RADIO_OFF;
    log_dma("Wi-Fi unloaded");
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
    s_st.radio = NET_RADIO_CONNECTING;
    strlcpy(s_st.radio_ssid, ssid, sizeof(s_st.radio_ssid));
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
    s_st.radio = NET_RADIO_OFF;
    s_st.radio_ssid[0] = '\0';
    if (s_inited) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(100));             /* let the event loop drain stop/disconnect */
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

static int scan_locked(net_ap_t *out, int max);

bool net_busy(void)
{
    return s_lock && uxSemaphoreGetCount(s_lock) == 0;
}

void net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
}

/*
 * Scan first, then:
 *   1. the saved network, if it is in range (or the scan saw nothing, e.g. a hidden SSID);
 *   2. if that did not sync and "open_wifi" is on, the strongest open networks (up to 3).
 * A captive-portal network joins fine but blocks NTP; it is reported as such and skipped.
 */
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

    static net_ap_t aps[MAX_SCAN];
    int n = scan_locked(aps, MAX_SCAN);
    net_result_t r = NET_NOT_FOUND;
    const char *used = "";

    if (have_saved) {
        bool visible = (n == 0);
        for (int i = 0; i < n && !visible; i++) {
            visible = strcmp(aps[i].ssid, ssid) == 0;
        }
        if (visible) {
            ESP_LOGI(TAG, "saved network '%s' in range", ssid);
            used = ssid;
            r = NET_CONNECT_FAILED;
            if (wifi_join(ssid, pass, CONNECT_TIMEOUT_MS)) {
                r = sync_ntp() ? NET_OK : NET_NTP_FAILED;
            }
            radio_off();
        } else {
            ESP_LOGI(TAG, "saved network '%s' not in range", ssid);
        }
    }

    if (r != NET_OK && allow_open) {
        int tried = 0;
        for (int i = 0; i < n && tried < OPEN_MAX_TRIES; i++) {
            if (aps[i].secured || (have_saved && strcmp(aps[i].ssid, ssid) == 0)) {
                continue;
            }
            tried++;
            ESP_LOGI(TAG, "trying open network '%s' (%d dBm)", aps[i].ssid, aps[i].rssi);
            bool joined = wifi_join(aps[i].ssid, "", CONNECT_TIMEOUT_MS);
            bool synced = joined && sync_ntp();
            radio_off();
            if (synced) {
                r = NET_OK_OPEN;
                used = aps[i].ssid;
                break;
            }
            if (joined && r != NET_NTP_FAILED) {
                r = NET_OPEN_BLOCKED;                   /* likely a login page */
                used = aps[i].ssid;
            } else if (!joined && r == NET_NOT_FOUND) {
                r = NET_CONNECT_FAILED;
                used = aps[i].ssid;
            }
        }
        if (!tried) {
            ESP_LOGI(TAG, "no open networks in range");
        }
    }
    if (r == NET_NOT_FOUND && !have_saved && !allow_open) {
        r = NET_NO_CREDENTIALS;
    }
    wifi_release();
    record(r, used);
    ESP_LOGI(TAG, "time sync result: %s", net_result_text(r));
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
    wifi_release();
    record(r, ssid);
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
    s_st.radio = NET_RADIO_SCANNING;
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
    s_st.radio = NET_RADIO_OFF;
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
    wifi_release();
    xSemaphoreGive(s_lock);
    return count;
}
