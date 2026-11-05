#pragma once

#include <stdbool.h>

/*
 * Wi-Fi is off except while a time sync, a scan or Improv provisioning runs. Credentials are
 * stored by this module in NVS (keys "ssid"/"pass"), not by the Wi-Fi driver.
 */
typedef enum {
    NET_OK = 0,
    NET_OK_OPEN,            /* synced through an open (passwordless) network */
    NET_NO_CREDENTIALS,
    NET_CONNECT_FAILED,
    NET_NTP_FAILED,
    NET_BUSY,
} net_result_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool secured;
} net_ap_t;

typedef enum {
    NET_RADIO_OFF = 0,
    NET_RADIO_SCANNING,
    NET_RADIO_CONNECTING,
    NET_RADIO_CONNECTED,
} net_radio_t;

typedef struct {
    net_radio_t radio;
    char radio_ssid[33];        /* network being joined / joined */
    bool have_result;           /* any sync/join attempted since power-on */
    net_result_t last_result;
    char last_ssid[33];         /* network the last attempt ended on ("" if none) */
    long long last_attempt;     /* epoch seconds */
    long long last_sync;        /* epoch seconds of the last successful sync, 0 = never */
} net_status_t;

void net_init(void);
void net_get_status(net_status_t *out);
void net_forget(void);       /* erase the saved network */
bool net_busy(void);     /* radio in use (sync, scan, provisioning): do not light-sleep */
const char *net_result_text(net_result_t r);

/* Sync the clock over SNTP: saved network first; if that fails or none is saved and setting
 * "open_wifi" (default on) allows it, the three strongest open networks. Radio off afterwards.
 * Blocks up to ~25 s, longer when open networks are tried. */
net_result_t net_sync_time(void);

/* Connect with new credentials; on success save them, sync the clock, radio off. */
net_result_t net_provision(const char *ssid, const char *pass);

/* Scan for networks (radio on only for the scan). Returns the count written to out. */
int net_scan(net_ap_t *out, int max);
