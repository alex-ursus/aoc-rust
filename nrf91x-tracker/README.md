# nRF91X Location Tracker

Zephyr/NCS firmware for the Nordic **Thingy:91X** that periodically POSTs a
JSON location payload to an AWS Lambda HTTPS endpoint.

## Hardware

| Component | Role |
|-----------|------|
| nRF9151 SiP | LTE-M/NB-IoT + GNSS + DECT NR+ (modem) |
| nRF5340 SoC | Application processor + Bluetooth 5.4 |
| nRF7002 IC | WiFi 6 companion — 2.4 GHz + 5 GHz passive scanning |
| BME688 | Temperature, humidity, pressure, gas resistance |
| Button 1 | Triggers fast-mode |
| LED 1 | Fast-mode indicator |

### Architecture note

The **nRF5340** is the application processor. It has direct hardware access to
the nRF7002 (WiFi) and native BLE. The **nRF9151** runs as a dedicated modem
and is accessed from the nRF5340 via IPC — NCS handles this transparently when
you target `thingy91x/nrf5340/cpuapp`. You can use all `lte_lc`, `nrf_modem_gnss`,
and `modem_key_mgmt` APIs as normal; the IPC shim is invisible to application code.

The nRF7002 and nRF5340 BLE **share the 2.4/5 GHz antenna** via an RF switch.
`CONFIG_MPSL_CX_NRF700X=y` enables coexistence so they do not interfere.

## Behaviour

| Mode | Interval | Trigger |
|------|----------|---------|
| Normal | 60 s | default |
| Fast | 10 s | Button 1 press → 10 min, then auto-revert |

Each report cycle:
1. Acquire GNSS fix (up to 30 s timeout; sends `null` if no fix)
2. Passive WiFi scan on 2.4 GHz + 5 GHz via nRF7002 (15 s timeout)
3. Passive BLE scan via nRF5340 (5 s)
4. Read BME688 (temperature, humidity, pressure, gas resistance)
5. POST JSON to AWS Lambda over TLS 1.2

## JSON Payload

```json
{
  "device_id": "thingy91x-unknown",
  "timestamp": 1749225600,
  "interval_s": 60,
  "location": {
    "latitude": 51.507400,
    "longitude": -0.127800,
    "altitude": 12.30,
    "accuracy": 3.50,
    "speed": 0.10,
    "heading": 0.00,
    "satellites": 9
  },
  "wifi": [
    { "bssid": "AA:BB:CC:DD:EE:FF", "ssid": "MyNetwork", "rssi": -65, "channel": 6, "band": 2 },
    { "bssid": "11:22:33:44:55:66", "ssid": "OfficeWifi", "rssi": -72, "channel": 36, "band": 5 }
  ],
  "bluetooth": [
    { "mac": "AA:BB:CC:DD:EE:FF", "rssi": -72, "name": "MyBeacon", "adv_type": 0 }
  ],
  "environment": {
    "temperature": 22.45,
    "humidity": 48.20,
    "pressure": 1013.25,
    "gas_resistance": 45230
  }
}
```

The `Authorization: Bearer <token>` header is sent on every request. WiFi
BSSID+RSSI data can be fed into a geolocation service (Google Maps Geolocation
API, Here, or your own fingerprint DB) on the Lambda side for indoor/urban
location refinement when GNSS is weak.

## Prerequisites

- [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html) ≥ 2.6.0 (tested on 2.7)
- `west` build tool
- Thingy:91X connected via USB (J-Link / USB-to-UART)

## Configuration

1. Copy the config template:
   ```bash
   cp src/aws_config.h.template src/aws_config.h
   ```

2. Edit `src/aws_config.h`:
   - `AWS_LAMBDA_HOST` — your API Gateway hostname
   - `AWS_LAMBDA_PATH` — URL path (`/stage/resource`)
   - `AWS_AUTH_TOKEN` — your bearer token
   - `AWS_CA_CERT` — paste the Amazon Root CA 1 PEM
     (download from https://www.amazontrust.com/repository/AmazonRootCA1.pem)

3. (Optional) Change `CONFIG_TRACKER_TLS_SEC_TAG` in `prj.conf` if tag `1` is
   already used by another application on your device.

## Build & Flash

```bash
# From inside the nrf91x-tracker directory
# Target: nRF5340 application core (has WiFi + BLE; modem accessed via IPC)
west build -b thingy91x/nrf5340/cpuapp -- -DEXTRA_CONF_FILE=boards/thingy91x.conf

# Flash over J-Link (USB cable attached)
west flash
```

### Serial monitor

```bash
# /dev/ttyACM0 is typical on Linux; adjust as needed
minicom -D /dev/ttyACM0 -b 115200
# or
screen /dev/ttyACM0 115200
```

## AWS Lambda — minimal handler

```python
import json

def handler(event, context):
    auth = event.get("headers", {}).get("Authorization", "")
    if auth != "Bearer your-secret-token-here":
        return {"statusCode": 401, "body": "Unauthorized"}

    body = json.loads(event.get("body", "{}"))
    # body["wifi"] contains BSSID+RSSI for WiFi geolocation
    # body["location"] contains GNSS fix
    # body["bluetooth"] contains nearby BLE devices
    # body["environment"] contains BME688 readings
    print(json.dumps(body))

    return {"statusCode": 200, "body": "OK"}
```

Use API Gateway HTTP API with a Lambda proxy integration and HTTPS only.

## Tuning

| Symbol | Default | Where |
|--------|---------|-------|
| `INTERVAL_NORMAL_S` | 60 | `tracker.h` |
| `INTERVAL_FAST_S` | 10 | `tracker.h` |
| `FAST_MODE_DURATION_S` | 600 | `tracker.h` |
| `WIFI_SCAN_TIMEOUT_S` | 15 | `tracker.h` |
| `WIFI_MAX_APS` | 20 | `tracker.h` |
| `BLE_SCAN_DURATION_S` | 5 | `tracker.h` |
| `BLE_MAX_DEVICES` | 20 | `tracker.h` |
| GNSS fix timeout | 30 s | `main.c` `do_report()` |
| HTTP timeout | 10 s | `http_client.c` |
