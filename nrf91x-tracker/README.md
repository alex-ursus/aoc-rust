# nRF91X Location Tracker

Zephyr/NCS firmware for the Nordic **Thingy:91X** that periodically POSTs a
JSON location payload to an AWS Lambda HTTPS endpoint.

## Hardware

| Component | Role |
|-----------|------|
| nRF9161 SiP | LTE-M/NB-IoT modem + GNSS |
| nRF5340 SoC | Application processor + BLE 5.4 scanner |
| BME688 | Temperature, humidity, pressure, gas resistance |
| Button 1 | Triggers fast-mode |
| LED 1 | Fast-mode indicator |

> **Note:** The Thingy:91X has no WiFi hardware. WiFi AP scanning is not
> possible. The firmware scans nearby BLE advertisements instead, which covers
> BLE beacons, IoT sensors, phones, and other BLE devices.

## Behaviour

| Mode | Interval | Trigger |
|------|----------|---------|
| Normal | 60 s | default |
| Fast | 10 s | Button 1 press → 10 min, then auto-revert |

Each report cycle:
1. Acquire GNSS fix (up to 30 s timeout; sends `null` location if no fix)
2. Passive BLE scan for 5 s
3. Read BME688 (temperature, humidity, pressure, gas resistance)
4. POST JSON to AWS Lambda over TLS 1.2

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

The `Authorization: Bearer <token>` header is added on every request.

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
west build -b thingy91x/nrf9161 -- -DEXTRA_CONF_FILE=boards/thingy91x.conf

# Flash over J-Link (USB cable attached)
west flash
```

### Serial monitor

```bash
# /dev/ttyACM0 is typical; adjust as needed
minicom -D /dev/ttyACM0 -b 115200
# or
screen /dev/ttyACM0 115200
```

## AWS Lambda — minimal handler

Your Lambda just needs to receive POST with JSON body and validate the header:

```python
import json

def handler(event, context):
    auth = event.get("headers", {}).get("Authorization", "")
    if auth != "Bearer your-secret-token-here":
        return {"statusCode": 401, "body": "Unauthorized"}

    body = json.loads(event.get("body", "{}"))
    # Store body in DynamoDB / S3 / etc.
    print(json.dumps(body))

    return {"statusCode": 200, "body": "OK"}
```

Use API Gateway HTTP API (not REST API) with a Lambda proxy integration and
enable HTTPS only. The Thingy:91X will validate the server certificate against
the Amazon Root CA you provisioned.

## Tuning

| Symbol | Default | Where |
|--------|---------|-------|
| `INTERVAL_NORMAL_S` | 60 | `tracker.h` |
| `INTERVAL_FAST_S` | 10 | `tracker.h` |
| `FAST_MODE_DURATION_S` | 600 | `tracker.h` |
| `BLE_SCAN_DURATION_S` | 5 | `tracker.h` |
| `BLE_MAX_DEVICES` | 20 | `tracker.h` |
| GNSS fix timeout | 30 s | `main.c` `do_report()` |
| HTTP timeout | 10 s | `http_client.c` |
