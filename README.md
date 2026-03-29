# HVAC Matter IDF

Clean ESP-IDF + ESP-Matter baseline for the HVAC-only firmware.

Current scope:
- Pure `espidf` build in PlatformIO
- Matter room-air-conditioner endpoint baseline
- Matter Wi-Fi commissioning flow
- NVS-backed HVAC registration/config
- Matter `Mode Select` protocol picker per HVAC
- Backend temperature-source assignment per HVAC
- IR transmit support for `daikin`, `gree`, and `midea`
- DS18B20 temperature input with setpoint fallback when no sensor is detected
- Simple HTTP API for config and runtime status
- Thermostat mode and power state kept in sync for Home Assistant climate control

Current limits:
- Config changes that alter endpoint count require reboot
- Matter commissioning currently depends on BLE from the commissioner side
- Some Matter startup warnings still need cleanup
- DS18B20 is only probed at boot today, so connecting one after startup requires a reboot
- Home Assistant may still render decimal setpoint controls even though firmware rounds to whole-degree values before sending IR

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
- Home Assistant creates a protocol dropdown from the Matter `Mode Select` cluster after a clean recommission.
- Home Assistant climate mode writes now work correctly:
  - `off` maps to `SystemMode=0` and `OnOff=0`
  - `cool` maps to `SystemMode=3` and `OnOff=1`
  - `heat` maps to `SystemMode=4` and `OnOff=1`
- Power toggle feedback is working in Home Assistant.
- IR sends are working again after releasing the unused DS18B20 1-Wire RMT bus when no sensor is present.
- DS18B20 polling works when the sensor is present at boot; otherwise local temperature falls back to the active setpoint.
- Setpoints are rounded to whole degrees before the IR protocol state is built.
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
- `temp_sensor_gpio` and whether a DS18B20 is currently detected

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

Default GPIO choices used by the firmware:

- IR emitter GPIO: `4`
- DS18B20 GPIO: `16`

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

## Temperature Sensor Behavior

The DS18B20 path uses a 1-Wire bus on GPIO `16`.

Current behavior:

- If a DS18B20 is detected during boot, the firmware keeps the 1-Wire bus active and reports `LocalTemperature` from the sensor.
- If no DS18B20 is detected during boot, the firmware releases the 1-Wire bus so IR can use RMT and falls back to the active setpoint as `LocalTemperature`.
- If you connect a DS18B20 after boot, reboot the device so it gets detected and the polling task starts.

Typical no-sensor boot log:

```text
No DS18B20 detected on GPIO 16
Released 1-Wire bus on GPIO 16 because no DS18B20 is active
```

## Setpoint Rounding

The firmware rounds setpoints to whole degrees before updating runtime state and sending IR commands.

Examples:

- `26.5` requested from a controller is sent as `27`
- `21.0` stays `21`

Home Assistant may still show decimal controls in its UI, but the firmware normalizes values before IR transmit.

## IR Runtime Notes

IR sending now uses a single owned emitter runtime in `main/ir_sender.cpp`, and successful live logs look like:

```text
ir_sender: Configured IR emitter on GPIO 4
hvac_matter: send ... protocol=gree emitter_gpio=4 ... err=ESP_OK
```

Changing `protocol` or `emitter_gpio` through `/api/config` is persisted correctly. Reboot after changes so the runtime starts from a clean state.
