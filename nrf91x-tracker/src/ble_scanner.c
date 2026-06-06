#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/addr.h>
#include <string.h>
#include <stdio.h>

#include "tracker.h"

LOG_MODULE_REGISTER(ble_scanner, LOG_LEVEL_INF);

static struct ble_scan_result *scan_result_ptr;
static struct k_sem scan_done_sem;

/* Called for every advertisement received during the scan window */
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
                    struct net_buf_simple *buf)
{
    if (!scan_result_ptr) {
        return;
    }

    struct ble_scan_result *res = scan_result_ptr;

    if (res->count >= BLE_MAX_DEVICES) {
        return;
    }

    /* Deduplicate by address */
    char addr_str[18];
    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    /* Strip type suffix appended by bt_addr_le_to_str, e.g. " (random)" */
    char *space = strchr(addr_str, ' ');
    if (space) {
        *space = '\0';
    }

    for (int i = 0; i < res->count; i++) {
        if (strcmp(res->devices[i].addr, addr_str) == 0) {
            /* Update RSSI in place (keep strongest reading) */
            if (rssi > res->devices[i].rssi) {
                res->devices[i].rssi = rssi;
            }
            return;
        }
    }

    struct ble_device *dev = &res->devices[res->count];
    strncpy(dev->addr, addr_str, sizeof(dev->addr) - 1);
    dev->rssi     = rssi;
    dev->adv_type = adv_type;

    /* Try to parse local name from advertisement data */
    struct net_buf_simple_state state;
    net_buf_simple_save(buf, &state);

    while (buf->len > 1) {
        uint8_t len  = net_buf_simple_pull_u8(buf);
        if (len == 0 || len > buf->len) {
            break;
        }
        uint8_t type = net_buf_simple_pull_u8(buf);
        len--;

        /* 0x08 = Shortened Local Name, 0x09 = Complete Local Name */
        if ((type == 0x08 || type == 0x09) && len > 0 &&
            len < sizeof(dev->name)) {
            memcpy(dev->name, buf->data, len);
            dev->name[len] = '\0';
            buf->data += len;
            buf->len  -= len;
            break;
        } else {
            net_buf_simple_pull(buf, len);
        }
    }

    net_buf_simple_restore(buf, &state);

    res->count++;
}

int ble_scanner_init(void)
{
    int err;

    k_sem_init(&scan_done_sem, 0, 1);

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("BT enable failed (%d)", err);
        return err;
    }

    LOG_INF("BLE scanner ready");
    return 0;
}

int ble_scanner_scan(struct ble_scan_result *out, uint32_t duration_s)
{
    int err;

    memset(out, 0, sizeof(*out));
    scan_result_ptr = out;

    struct bt_le_scan_param scan_params = {
        .type     = BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_FILTER_DUPLICATE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window   = BT_GAP_SCAN_FAST_WINDOW,
    };

    err = bt_le_scan_start(&scan_params, scan_cb);
    if (err) {
        LOG_ERR("BLE scan start failed (%d)", err);
        scan_result_ptr = NULL;
        return err;
    }

    LOG_INF("BLE scanning for %us...", duration_s);
    k_sleep(K_SECONDS(duration_s));

    bt_le_scan_stop();
    scan_result_ptr = NULL;

    LOG_INF("BLE scan done: %d device(s) found", out->count);
    return 0;
}
