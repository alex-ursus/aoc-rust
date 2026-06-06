#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/device.h>
#include <string.h>

#include "tracker.h"

LOG_MODULE_REGISTER(sensor_module, LOG_LEVEL_INF);

static const struct device *bme688_dev;
static const struct device *npm1300_dev;  /* nPM1300 fuel gauge */

int sensor_module_init(void)
{
    bme688_dev = DEVICE_DT_GET_ANY(bosch_bme680);
    if (!bme688_dev || !device_is_ready(bme688_dev)) {
        LOG_ERR("BME688 device not ready");
        return -ENODEV;
    }

    /* nPM1300 fuel gauge — non-fatal if absent */
    npm1300_dev = DEVICE_DT_GET_ANY(nordic_npm1300_fuel_gauge);
    if (!npm1300_dev || !device_is_ready(npm1300_dev)) {
        LOG_WRN("nPM1300 fuel gauge not ready — battery level unavailable");
        npm1300_dev = NULL;
    }

    LOG_INF("Sensor module ready (BME688 + %s)",
            npm1300_dev ? "nPM1300 fuel gauge" : "no fuel gauge");
    return 0;
}

int sensor_module_read(struct env_data *out)
{
    int err;
    struct sensor_value val;

    memset(out, 0, sizeof(*out));

    if (!bme688_dev) {
        return -ENODEV;
    }

    err = sensor_sample_fetch(bme688_dev);
    if (err) {
        LOG_ERR("BME688 fetch failed (%d)", err);
        return err;
    }

    err = sensor_channel_get(bme688_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
    if (!err) {
        out->temperature = sensor_value_to_double(&val);
    }

    err = sensor_channel_get(bme688_dev, SENSOR_CHAN_HUMIDITY, &val);
    if (!err) {
        out->humidity = sensor_value_to_double(&val);
    }

    err = sensor_channel_get(bme688_dev, SENSOR_CHAN_PRESS, &val);
    if (!err) {
        out->pressure = sensor_value_to_double(&val);
    }

    /* Gas resistance — proxy for air quality (higher = cleaner air) */
    err = sensor_channel_get(bme688_dev, SENSOR_CHAN_GAS_RES, &val);
    if (!err) {
        out->gas_resistance = sensor_value_to_double(&val);
    }

    out->valid = true;

    /* Battery state from nPM1300 fuel gauge */
    if (npm1300_dev) {
        err = sensor_sample_fetch(npm1300_dev);
        if (!err) {
            /* State of charge: 0–100 % */
            if (!sensor_channel_get(npm1300_dev, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &val)) {
                out->battery_pct = (uint8_t)val.val1;
                out->battery_valid = true;
            }
            /* Terminal voltage in mV */
            if (!sensor_channel_get(npm1300_dev, SENSOR_CHAN_GAUGE_VOLTAGE, &val)) {
                out->battery_mv = (uint16_t)(sensor_value_to_double(&val) * 1000.0);
            }
        } else {
            LOG_WRN("nPM1300 fetch failed (%d)", err);
        }
    }

    LOG_INF("Env: temp=%.2f°C hum=%.1f%% pres=%.2fhPa gas=%.0fΩ batt=%u%%%s",
            out->temperature, out->humidity, out->pressure, out->gas_resistance,
            out->battery_pct, out->battery_valid ? "" : "(n/a)");
    return 0;
}
