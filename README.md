# HVAC Matter IDF

Clean ESP-IDF + ESP-Matter baseline for the HVAC-only firmware.

Current scope:
- Pure `espidf` build in PlatformIO
- Matter room-air-conditioner endpoint baseline
- Matter Wi-Fi commissioning flow
- NVS-backed HVAC registration/config
- Backend protocol selection per HVAC
- Backend temperature-source assignment per HVAC
- IR transmit support for `daikin`, `gree`, and `midea`
- Simple HTTP API for config and runtime status
- Thermostat mode and power state kept in sync for Home Assistant climate control

Current limits:
- No physical DS18B20 integration yet
- Config changes that alter endpoint count require reboot
- Matter commissioning currently depends on BLE from the commissioner side
- Some Matter startup warnings still need cleanup
- Changing the IR emitter GPIO at runtime can fail if the previous RMT TX channel is still allocated

Endpoints:
- `GET /api/config`
- `POST /api/config`
- `GET /api/status`
- `POST /api/hvac/send`

Commissioning:
- The firmware prints the Matter manual pairing code and `MT:` QR payload on boot.

## What Was Verified

- Matter commissioning with Home Assistant is working.
- The ESP32 restores Wi-Fi and fabric state after reboot.
- Home Assistant climate mode writes now work correctly:
  - `off` maps to `SystemMode=0` and `OnOff=0`
  - `cool` maps to `SystemMode=3` and `OnOff=1`
  - `heat` maps to `SystemMode=4` and `OnOff=1`
- Power toggle feedback is working in Home Assistant.
- The previous `ThreadNetworkDiagnostics` read failures from Home Assistant were fixed by returning fallback values on Wi-Fi-only builds instead of `CHIP_ERROR_NOT_IMPLEMENTED`.

## Build Notes

This repo was adjusted to build cleanly on macOS:

- `platformio.ini` uses a local `.pio/build` directory instead of a Windows-only path.
- `scripts/set_component_cache.py` now picks a platform-appropriate `cmake` executable and cache path.

## Configuration API

The firmware stores HVAC configuration in NVS and exposes it over HTTP.

Available endpoints:

- `GET /api/config`
- `POST /api/config`
- `GET /api/status`
- `POST /api/hvac/send`

`GET /api/config` returns the saved configuration, including:

- `protocol`
- `model`
- `emitter_gpio`
- `temp_sensor_index`
- `current_temp_source`

`GET /api/status` returns:

- the supported protocol list
- current endpoint IDs
- active runtime state such as power, mode, setpoints, and last send error

Current supported IR protocols:

- `daikin`
- `gree`
- `midea`

## Default Configuration

The built-in default HVAC entry is:

- `id`: `hvac-1`
- `protocol`: `daikin`
- `model`: `-1`
- `emitter_gpio`: `4`
- `temp_sensor_index`: `0`
- `current_temp_source`: `sensor`

## How To Change Protocol And IR GPIO

1. Read the current config:

```bash
curl http://<esp32-ip>/api/config
```

2. Check runtime state and supported protocols:

```bash
curl http://<esp32-ip>/api/status
```

3. Post a new config. Example: switch to `gree` on GPIO `27`:

```bash
curl -X POST http://<esp32-ip>/api/config \
  -H 'Content-Type: application/json' \
  -d '{
    "hvac_count": 1,
    "temp_sensors": [
      { "id": "sensor-1", "name": "Ambient" }
    ],
    "hvacs": [
      {
        "id": "hvac-1",
        "protocol": "gree",
        "model": -1,
        "emitter_gpio": 27,
        "temp_sensor_index": 0,
        "current_temp_source": "sensor"
      }
    ]
  }'
```

4. Reboot the device.

The config API saves successfully, but changes to GPIO/protocol should currently be treated as requiring a reboot to fully apply.

## Direct IR Send Test

You can test sending without going through Matter:

```bash
curl -X POST http://<esp32-ip>/api/hvac/send \
  -H 'Content-Type: application/json' \
  -d '{
    "id": "hvac-1",
    "power": true,
    "system_mode": 4,
    "fan_mode": 5,
    "heating_setpoint_centi_c": 2300
  }'
```

Example successful response shape:

```json
{"ok":true,"id":"hvac-1","protocol":"daikin","emitter_gpio":4,"error":"ESP_OK","active_target_c":23}
```

## Known IR GPIO Issue

Changing the saved config from one emitter GPIO to another currently exposes an RMT allocation issue.

Observed behavior after switching from `daikin` on GPIO `4` to `gree` on GPIO `27`:

- `GET /api/config` correctly reports `protocol:"gree"` and `emitter_gpio:27`
- `GET /api/status` correctly reports the new values
- `POST /api/hvac/send` fails with `ESP_ERR_NOT_FOUND`

Relevant runtime logs:

```text
rmt: no free tx channels
ir_sender: rmt_new_tx_channel failed: ESP_ERR_NOT_FOUND
ir_sender: send_gree(...) emitter unavailable
```

This means:

- protocol selection is working
- config persistence is working
- the send path is failing because the firmware tries to allocate a fresh RMT TX channel for the new GPIO and does not recover cleanly when channels are already consumed

## Next Change To Make GPIO Switching Work

The next code change should be in `main/ir_sender.cpp`.

Goal:

- allow runtime emitter GPIO changes without exhausting RMT TX channels

Recommended implementation:

1. Add an emitter release path that disables and deletes the old `rmt_channel_handle_t` and `rmt_encoder_handle_t`.
2. Detect when a saved/configured GPIO has changed for an HVAC and release the old emitter before creating a new one.
3. If only one emitter is needed, prefer reusing a single slot/channel instead of allocating a new one per GPIO forever.
4. Keep logging the selected `protocol` and `emitter_gpio` on every send for easy verification.

Until that change lands, changing `protocol` and `emitter_gpio` together is persisted correctly but may not transmit successfully after switching to a new GPIO.
