#ifndef TRACKER_H
#define TRACKER_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Timing ------------------------------------------------------------ */
#define INTERVAL_NORMAL_S       60
#define INTERVAL_FAST_S         10
#define FAST_MODE_DURATION_S    600   /* 10 minutes */

/* ---- BLE scan ---------------------------------------------------------- */
#define BLE_SCAN_DURATION_S     5
#define BLE_MAX_DEVICES         20

/* ---- WiFi scan (nRF7002) ----------------------------------------------- */
#define WIFI_SCAN_TIMEOUT_S     15
#define WIFI_MAX_APS            20

/* ---- Payload structures ------------------------------------------------ */

struct gnss_data {
    double  latitude;
    double  longitude;
    double  altitude;
    float   accuracy;
    float   speed;
    float   heading;
    int     satellites;
    bool    valid;
};

struct ble_device {
    char    addr[18];       /* "AA:BB:CC:DD:EE:FF" */
    char    name[32];
    int8_t  rssi;
    uint8_t adv_type;       /* ADV_IND, ADV_NONCONN_IND, etc. */
};

struct ble_scan_result {
    struct ble_device devices[BLE_MAX_DEVICES];
    int count;
};

struct wifi_ap {
    char    ssid[33];       /* max 32 chars + NUL */
    char    bssid[18];      /* "AA:BB:CC:DD:EE:FF" */
    int8_t  rssi;
    uint8_t channel;
    uint8_t band;           /* 2 = 2.4 GHz, 5 = 5 GHz */
};

struct wifi_scan_data {
    struct wifi_ap aps[WIFI_MAX_APS];
    int count;
};

struct env_data {
    double  temperature;    /* °C */
    double  humidity;       /* % RH */
    double  pressure;       /* hPa */
    double  gas_resistance; /* Ω  (air quality proxy) */
    bool    valid;
};

struct tracker_payload {
    char                    device_id[32];
    int64_t                 timestamp;      /* Unix epoch seconds */
    struct gnss_data        gnss;
    struct ble_scan_result  ble;
    struct wifi_scan_data   wifi;
    struct env_data         env;
    int                     interval_s;     /* current reporting interval */
};

/* ---- Module APIs ------------------------------------------------------- */

/* gnss_module.c */
int  gnss_module_init(void);
int  gnss_module_get_fix(struct gnss_data *out, k_timeout_t timeout);

/* ble_scanner.c */
int  ble_scanner_init(void);
int  ble_scanner_scan(struct ble_scan_result *out, uint32_t duration_s);

/* wifi_scanner.c */
int  wifi_scanner_init(void);
int  wifi_scanner_scan(struct wifi_scan_data *out, uint32_t timeout_s);

/* sensor_module.c */
int  sensor_module_init(void);
int  sensor_module_read(struct env_data *out);

/* http_client.c */
int  http_client_init(void);
int  http_client_post(const struct tracker_payload *payload);

#endif /* TRACKER_H */
