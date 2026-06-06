#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <dk_buttons_and_leds.h>
#include <modem/lte_lc.h>
#include <date_time.h>
#include <string.h>
#include <stdio.h>

#include "tracker.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* ---- State ------------------------------------------------------------- */

static struct k_work_delayable report_work;
static atomic_t fast_mode_remaining_s = ATOMIC_INIT(0);
static struct k_work_delayable fast_mode_countdown_work;

/* ---- LED helpers ------------------------------------------------------- */

static void set_led_fast_mode(bool active)
{
    dk_set_led(DK_LED1, active ? 1 : 0);
}

/* ---- Fast-mode countdown (ticks down every second) --------------------- */

static void fast_mode_countdown_handler(struct k_work *work)
{
    int remaining = atomic_dec(&fast_mode_remaining_s);

    if (remaining > 1) {
        k_work_schedule(&fast_mode_countdown_work, K_SECONDS(1));
    } else {
        atomic_set(&fast_mode_remaining_s, 0);
        set_led_fast_mode(false);
        LOG_INF("Fast mode ended, returning to %ds interval", INTERVAL_NORMAL_S);
        k_work_reschedule(&report_work, K_SECONDS(INTERVAL_NORMAL_S));
    }
}

/* ---- Button handler ---------------------------------------------------- */

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
    if (!((button_state & has_changed) & DK_BTN1_MSK)) {
        return;
    }

    LOG_INF("Button pressed: fast mode for %ds", FAST_MODE_DURATION_S);
    atomic_set(&fast_mode_remaining_s, FAST_MODE_DURATION_S);
    set_led_fast_mode(true);

    k_work_reschedule(&report_work, K_NO_WAIT);
    k_work_schedule(&fast_mode_countdown_work, K_SECONDS(1));
}

/* ---- Core report ------------------------------------------------------- */

static void do_report(struct k_work *work)
{
    int err;
    struct tracker_payload payload = { 0 };
    int64_t ts_ms;

    /* Timestamp */
    if (date_time_now(&ts_ms) == 0) {
        payload.timestamp = ts_ms / 1000;
    }

    /* Device ID from modem IMEI — populated once at boot, reused each cycle */
    static char device_id[32] = { 0 };
    if (device_id[0] == '\0') {
        strncpy(device_id, "thingy91x-unknown", sizeof(device_id) - 1);
    }
    strncpy(payload.device_id, device_id, sizeof(payload.device_id) - 1);

    /* Current interval */
    bool fast = atomic_get(&fast_mode_remaining_s) > 0;
    payload.interval_s = fast ? INTERVAL_FAST_S : INTERVAL_NORMAL_S;

    /* GNSS — up to 30 s; sends null location if no fix */
    err = gnss_module_get_fix(&payload.gnss, K_SECONDS(30));
    if (err) {
        LOG_WRN("GNSS fix failed (%d), continuing without location", err);
    }

    /* WiFi AP scan via nRF7002 */
    err = wifi_scanner_scan(&payload.wifi, WIFI_SCAN_TIMEOUT_S);
    if (err) {
        LOG_WRN("WiFi scan failed (%d)", err);
    }

    /* BLE scan via nRF5340 */
    err = ble_scanner_scan(&payload.ble, BLE_SCAN_DURATION_S);
    if (err) {
        LOG_WRN("BLE scan failed (%d)", err);
    }

    /* Environmental sensor (BME688) */
    err = sensor_module_read(&payload.env);
    if (err) {
        LOG_WRN("Sensor read failed (%d)", err);
    }

    /* POST to AWS Lambda */
    err = http_client_post(&payload);
    if (err) {
        LOG_ERR("HTTP POST failed (%d)", err);
    } else {
        LOG_INF("Sent OK — lat=%.6f lon=%.6f temp=%.1f wifi=%d ble=%d",
                payload.gnss.latitude, payload.gnss.longitude,
                payload.env.temperature, payload.wifi.count, payload.ble.count);
    }

    k_work_schedule(&report_work,
                    K_SECONDS(fast ? INTERVAL_FAST_S : INTERVAL_NORMAL_S));
}

/* ---- LTE connect ------------------------------------------------------- */

static void lte_event_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
        if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
            evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
            LOG_INF("LTE registered (%s)",
                    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME
                        ? "home" : "roaming");
        }
        break;
    case LTE_LC_EVT_PSM_UPDATE:
        LOG_DBG("PSM: TAU=%d s, active=%d s",
                evt->psm_cfg.tau, evt->psm_cfg.active_time);
        break;
    default:
        break;
    }
}

static int lte_connect(void)
{
    int err;

    LOG_INF("Connecting to LTE...");
    lte_lc_register_handler(lte_event_handler);

    err = lte_lc_init_and_connect();
    if (err) {
        LOG_ERR("LTE connect failed (%d)", err);
        return err;
    }

    LOG_INF("LTE connected");
    return 0;
}

/* ---- Entry point ------------------------------------------------------- */

int main(void)
{
    int err;

    LOG_INF("nRF91X location tracker starting");

    err = gnss_module_init();
    if (err) {
        LOG_ERR("GNSS init failed (%d)", err);
        return err;
    }

    err = wifi_scanner_init();
    if (err) {
        LOG_ERR("WiFi init failed (%d)", err);
        return err;
    }

    err = ble_scanner_init();
    if (err) {
        LOG_ERR("BLE init failed (%d)", err);
        return err;
    }

    err = sensor_module_init();
    if (err) {
        LOG_ERR("Sensor init failed (%d)", err);
        return err;
    }

    err = dk_buttons_init(button_handler);
    if (err) {
        LOG_ERR("Button init failed (%d)", err);
        return err;
    }

    err = dk_leds_init();
    if (err) {
        LOG_ERR("LED init failed (%d)", err);
        return err;
    }

    err = lte_connect();
    if (err) {
        return err;
    }

    err = http_client_init();
    if (err) {
        LOG_ERR("HTTP client init failed (%d)", err);
        return err;
    }

    err = date_time_update_async(NULL);
    if (err) {
        LOG_WRN("Date/time sync failed (%d)", err);
    }

    k_work_init_delayable(&report_work, do_report);
    k_work_init_delayable(&fast_mode_countdown_work, fast_mode_countdown_handler);

    k_work_schedule(&report_work, K_NO_WAIT);

    return 0;
}
