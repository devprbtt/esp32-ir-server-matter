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

Current limits:
- No physical DS18B20 integration yet
- Config changes that alter endpoint count require reboot
- Matter commissioning currently depends on BLE from the commissioner side
- Some Matter startup warnings still need cleanup

Endpoints:
- `GET /api/config`
- `POST /api/config`
- `GET /api/status`
- `POST /api/hvac/send`

Commissioning:
- The firmware prints the Matter manual pairing code and `MT:` QR payload on boot.
