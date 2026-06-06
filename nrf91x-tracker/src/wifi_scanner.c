#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_if.h>
#include <string.h>
#include <stdio.h>

#include "tracker.h"

LOG_MODULE_REGISTER(wifi_scanner, LOG_LEVEL_INF);

/* ---- Internal state ---------------------------------------------------- */

static struct net_mgmt_event_callback wifi_cb;
static struct wifi_scan_result *scan_result_ptr;
static struct k_sem scan_done_sem;

/* ---- Event callback ---------------------------------------------------- */

static void handle_scan_result(struct net_mgmt_event_callback *cb)
{
    const struct wifi_scan_result *entry =
        (const struct wifi_scan_result *)cb->info;

    if (!scan_result_ptr) {
        return;
    }

    struct wifi_scan_data *res = (struct wifi_scan_data *)scan_result_ptr;

    if (res->count >= WIFI_MAX_APS) {
        return;
    }

    struct wifi_ap *ap = &res->aps[res->count];

    /* SSID */
    memcpy(ap->ssid, entry->ssid, entry->ssid_length);
    ap->ssid[entry->ssid_length] = '\0';

    /* BSSID as "AA:BB:CC:DD:EE:FF" */
    snprintf(ap->bssid, sizeof(ap->bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             entry->mac[0], entry->mac[1], entry->mac[2],
             entry->mac[3], entry->mac[4], entry->mac[5]);

    ap->rssi    = entry->rssi;
    ap->channel = entry->channel;
    ap->band    = (entry->band == WIFI_FREQ_BAND_5_GHZ) ? 5 : 2;

    res->count++;
}

static void handle_scan_done(void)
{
    k_sem_give(&scan_done_sem);
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                    uint32_t mgmt_event,
                                    struct net_if *iface)
{
    (void)iface;

    switch (mgmt_event) {
    case NET_EVENT_WIFI_SCAN_RESULT:
        handle_scan_result(cb);
        break;
    case NET_EVENT_WIFI_SCAN_DONE:
        handle_scan_done();
        break;
    default:
        break;
    }
}

/* ---- Public API -------------------------------------------------------- */

int wifi_scanner_init(void)
{
    k_sem_init(&scan_done_sem, 0, 1);

    net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler,
                                 NET_EVENT_WIFI_SCAN_RESULT |
                                 NET_EVENT_WIFI_SCAN_DONE);
    net_mgmt_add_event_callback(&wifi_cb);

    LOG_INF("WiFi scanner ready (nRF7002)");
    return 0;
}

int wifi_scanner_scan(struct wifi_scan_data *out, uint32_t timeout_s)
{
    struct net_if *iface;
    int err;

    memset(out, 0, sizeof(*out));
    scan_result_ptr = (struct wifi_scan_result *)out;  /* cast: same base ptr */
    k_sem_reset(&scan_done_sem);

    /* Get the WiFi interface (nRF7002) */
    iface = net_if_get_wifi_sta();
    if (!iface) {
        LOG_ERR("No WiFi interface found");
        scan_result_ptr = NULL;
        return -ENODEV;
    }

    /* Passive scan on both bands */
    struct wifi_scan_params params = {
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .bands     = WIFI_FREQ_BAND_2_4_GHZ | WIFI_FREQ_BAND_5_GHZ,
        .dwell_time_passive = 130,   /* ms per channel */
    };

    err = net_mgmt(NET_REQUEST_WIFI_SCAN, iface, &params, sizeof(params));
    if (err) {
        LOG_ERR("WiFi scan request failed (%d)", err);
        scan_result_ptr = NULL;
        return err;
    }

    LOG_INF("WiFi scanning (passive, 2.4+5 GHz)...");

    err = k_sem_take(&scan_done_sem, K_SECONDS(timeout_s));
    scan_result_ptr = NULL;

    if (err == -EAGAIN) {
        LOG_WRN("WiFi scan timeout after %us", timeout_s);
        return -ETIMEDOUT;
    }

    LOG_INF("WiFi scan done: %d AP(s) found", out->count);
    return 0;
}
