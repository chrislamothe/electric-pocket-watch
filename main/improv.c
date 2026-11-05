/*
 * Improv Wi-Fi serial, device side. Framing checked against improv-wifi/sdk-serial-js
 * (serial.ts): "IMPROV" + version 1 + type + length + data + checksum, where the checksum is
 * the low byte of the sum of every preceding byte; the client appends '\n' and resets its
 * line parser on '\n', so every packet we send starts on a fresh line.
 */
#include "improv.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "faces.h"
#include "net.h"
#include "version.h"

static const char *TAG = "improv";

enum { TYPE_STATE = 0x01, TYPE_ERROR = 0x02, TYPE_RPC = 0x03, TYPE_RESULT = 0x04 };
enum { STATE_READY = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
enum { ERR_NONE = 0x00, ERR_INVALID_RPC = 0x01, ERR_UNKNOWN_RPC = 0x02, ERR_UNABLE_TO_CONNECT = 0x03 };
enum { CMD_WIFI = 0x01, CMD_STATE = 0x02, CMD_INFO = 0x03, CMD_SCAN = 0x04 };

#define MAX_SCAN 20

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len)
{
    uint8_t buf[1 + 9 + 255 + 2];
    size_t n = 0;
    buf[n++] = '\n';                                    /* start on a fresh line */
    memcpy(buf + n, "IMPROV", 6);
    n += 6;
    buf[n++] = 1;
    buf[n++] = type;
    buf[n++] = len;
    memcpy(buf + n, data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 1; i < n; i++) {
        sum += buf[i];
    }
    buf[n++] = sum;
    buf[n++] = '\n';
    /* One ring-buffer write keeps the packet contiguous even if another task is logging. */
    usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(200));
}

static void send_byte(uint8_t type, uint8_t v)
{
    send_packet(type, &v, 1);
}

static void send_result(uint8_t cmd, const char *const *strs, int count)
{
    uint8_t data[255];
    size_t n = 2;
    for (int i = 0; i < count; i++) {
        size_t l = strlen(strs[i]);
        if (n + 1 + l > sizeof(data)) {
            break;
        }
        data[n++] = (uint8_t)l;
        memcpy(data + n, strs[i], l);
        n += l;
    }
    data[0] = cmd;
    data[1] = (uint8_t)(n - 2);
    send_packet(TYPE_RESULT, data, (uint8_t)n);
}

static void handle_rpc(const uint8_t *d, uint8_t len)
{
    if (len < 2 || d[1] + 2 > len) {
        send_byte(TYPE_ERROR, ERR_INVALID_RPC);
        return;
    }
    uint8_t cmd = d[0];
    const uint8_t *p = d + 2;
    uint8_t plen = d[1];
    send_byte(TYPE_ERROR, ERR_NONE);

    switch (cmd) {
    case CMD_STATE:
        send_byte(TYPE_STATE, STATE_READY);             /* radio is off between syncs */
        break;

    case CMD_INFO: {
        const char *info[] = {WATCH_NAME, WATCH_VERSION, "ESP32-S3", "Pocket Watch"};
        send_result(CMD_INFO, info, 4);
        break;
    }

    case CMD_SCAN: {
        static net_ap_t aps[MAX_SCAN];
        int n = net_scan(aps, MAX_SCAN);
        for (int i = 0; i < n; i++) {
            char rssi[8];
            snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
            const char *row[] = {aps[i].ssid, rssi, aps[i].secured ? "YES" : "NO"};
            send_result(CMD_SCAN, row, 3);
        }
        send_result(CMD_SCAN, NULL, 0);                 /* end of list */
        break;
    }

    case CMD_WIFI: {
        if (plen < 1 || p[0] + 2 > plen || p[0] > 32) {
            send_byte(TYPE_ERROR, ERR_INVALID_RPC);
            return;
        }
        char ssid[33] = {0}, pass[65] = {0};
        uint8_t sl = p[0];
        memcpy(ssid, p + 1, sl);
        uint8_t pl = p[1 + sl];
        if (pl > 64 || 2 + sl + pl > plen) {
            send_byte(TYPE_ERROR, ERR_INVALID_RPC);
            return;
        }
        memcpy(pass, p + 2 + sl, pl);
        ESP_LOGI(TAG, "Wi-Fi settings received for '%s'", ssid);
        send_byte(TYPE_STATE, STATE_PROVISIONING);
        faces_show_status("Joining Wi-Fi...", 0);
        net_result_t r = net_provision(ssid, pass);
        if (r == NET_OK || r == NET_OK_OPEN || r == NET_NTP_FAILED) {       /* credentials work; NTP is a bonus */
            send_byte(TYPE_STATE, STATE_PROVISIONED);
            send_result(CMD_WIFI, NULL, 0);             /* no redirect URL */
            faces_show_status(r == NET_OK ? "Wi-Fi saved, time synced" : "Wi-Fi saved", 4000);
        } else {
            send_byte(TYPE_ERROR, ERR_UNABLE_TO_CONNECT);
            send_byte(TYPE_STATE, STATE_READY);
            faces_show_status("Wi-Fi failed", 4000);
        }
        break;
    }

    default:
        send_byte(TYPE_ERROR, ERR_UNKNOWN_RPC);
        break;
    }
}

static void handle_packet(const uint8_t *pkt, int total)
{
    uint8_t sum = 0;
    for (int i = 0; i < total - 1; i++) {
        sum += pkt[i];
    }
    if (pkt[6] != 1 || sum != pkt[total - 1]) {
        send_byte(TYPE_ERROR, ERR_INVALID_RPC);
        return;
    }
    if (pkt[7] == TYPE_RPC) {
        handle_rpc(pkt + 9, pkt[8]);
    }
}

static void improv_task(void *arg)
{
    static const char HDR[] = "IMPROV";
    static uint8_t pkt[9 + 255 + 1];
    int have = 0;
    uint8_t rx[64];
    while (1) {
        int n = usb_serial_jtag_read_bytes(rx, sizeof(rx), pdMS_TO_TICKS(1000));
        for (int i = 0; i < n; i++) {
            uint8_t b = rx[i];
            if (have < 6) {
                if (b == (uint8_t)HDR[have]) {
                    pkt[have++] = b;
                } else {
                    have = 0;
                    if (b == 'I') {
                        pkt[have++] = b;
                    }
                }
                continue;
            }
            pkt[have++] = b;
            if (have >= 9 && have == 9 + pkt[8] + 1) {
                handle_packet(pkt, have);
                have = 0;
            }
        }
    }
}

void improv_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 2048, .rx_buffer_size = 512};
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: Improv disabled");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    xTaskCreate(improv_task, "improv", 8192, NULL, 4, NULL);
}
