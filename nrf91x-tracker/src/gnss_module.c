#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_gnss.h>
#include <string.h>

#include "tracker.h"

LOG_MODULE_REGISTER(gnss_module, LOG_LEVEL_INF);

static struct k_sem gnss_fix_sem;
static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static bool fix_received;

static void gnss_event_handler(int event)
{
    switch (event) {
    case NRF_MODEM_GNSS_EVT_PVT:
        nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data),
                            NRF_MODEM_GNSS_DATA_PVT);

        if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
            fix_received = true;
            k_sem_give(&gnss_fix_sem);
        }
        break;

    case NRF_MODEM_GNSS_EVT_AGPS_REQ:
        /* A-GPS not wired up in this project; GNSS will still work cold */
        break;

    default:
        break;
    }
}

int gnss_module_init(void)
{
    int err;

    k_sem_init(&gnss_fix_sem, 0, 1);
    fix_received = false;

    err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
    if (err) {
        LOG_ERR("GNSS handler set failed (%d)", err);
        return err;
    }

    /* Single-fix mode: acquire one fix, then stop to save power */
    err = nrf_modem_gnss_fix_interval_set(0);
    if (err) {
        LOG_ERR("GNSS fix interval failed (%d)", err);
        return err;
    }

    /* Use all constellations */
    err = nrf_modem_gnss_system_mask_set(
        NRF_MODEM_GNSS_SYSTEM_GPS_MASK |
        NRF_MODEM_GNSS_SYSTEM_GLONASS_MASK |
        NRF_MODEM_GNSS_SYSTEM_GALILEO_MASK);
    if (err) {
        LOG_WRN("GNSS system mask set failed (%d), using default", err);
    }

    LOG_INF("GNSS module ready");
    return 0;
}

int gnss_module_get_fix(struct gnss_data *out, k_timeout_t timeout)
{
    int err;

    memset(out, 0, sizeof(*out));
    fix_received = false;
    k_sem_reset(&gnss_fix_sem);

    err = nrf_modem_gnss_start();
    if (err) {
        LOG_ERR("GNSS start failed (%d)", err);
        return err;
    }

    LOG_INF("Waiting for GNSS fix...");
    err = k_sem_take(&gnss_fix_sem, timeout);

    nrf_modem_gnss_stop();

    if (err == -EAGAIN) {
        LOG_WRN("GNSS fix timeout");
        return -ETIMEDOUT;
    }

    out->latitude   = pvt_data.latitude;
    out->longitude  = pvt_data.longitude;
    out->altitude   = pvt_data.altitude;
    out->accuracy   = pvt_data.accuracy;
    out->speed      = pvt_data.speed;
    out->heading    = pvt_data.heading;
    out->satellites = pvt_data.sv_count;
    out->valid      = true;

    LOG_INF("Fix: lat=%.6f lon=%.6f alt=%.1f acc=%.1f sats=%d",
            out->latitude, out->longitude, out->altitude,
            out->accuracy, out->satellites);
    return 0;
}
