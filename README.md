# Codex FACES Bridge

ESP-IDF firmware that makes an M5Stack FIRE + FACES II bottom + FACES GameBoy panel appear as a Codex Micro-compatible Bluetooth HID device.

The firmware implements the vendor HID report and newline-delimited JSON RPC directly on the ESP32. No macOS background bridge is required.

## Hardware

- M5Stack FIRE (classic ESP32)
- FACES II bottom
- FACES GameBoy panel

Remove the FIRE M5GO bottom before fitting FACES II. The two bottoms are alternatives, not a stack.

## Current controls

| Input | Normal layer | Agent layer |
| --- | --- | --- |
| D-pad | radial input (`v.oai.rad`) | `AG00`–`AG03` |
| GameBoy A/B | `ACT06` / `ACT07` | `AG04` / `AG05` |
| Start | `ACT08` | `ACT08` |
| Pause, short press | `ACT09` | `ACT09` |
| Pause, hold 0.7 s | toggle Agent layer | toggle Agent layer |
| Pause + D-pad Up/Down | reasoning dial clockwise/counter-clockwise | same |
| FIRE A | `ACT10` | `ACT10` |
| FIRE B | reasoning dial press | same |
| FIRE C | `ACT12` | `ACT12` |

The FACES II base has five LEDs on each side. Firmware composites the six thread colors into a five-point palette, mirrors it across both sides, overlays ambient effects, and limits LED output to 20% brightness. The FIRE screen shows six matching tiles; a blue header is the normal layer, and orange is the Agent layer. Lighting effects `off`, `solid`, `snake`, `rainbow`, `breath`, `gradient`, and `shallowBreath` are rendered by the firmware. A segmented battery icon in the upper-right shows the IP5306 battery level and turns green while charging.

Codex owns and persists command-key and joystick mappings. FIRE B behaves like the physical dial: short press confirms the highlighted control, while a long press opens Codex Micro settings.

## Build and flash

ESP-IDF 5.5 is the tested toolchain.

```sh
. "$IDF_PATH/export.sh"
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

The release bundle also includes a merged image. It contains the bootloader,
partition table, and application and must be written at offset `0x0`:

```sh
python -m esptool --chip esp32 -p /dev/cu.usbserial-XXXX -b 460800 \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 40m \
  0x0 codex_faces_bridge_full.bin
```

After flashing:

1. Open macOS Bluetooth settings and pair `kbd-1.0-codex-micro`.
2. Start Codex and open the Codex Micro onboarding panel.
3. If a previous firmware identity is cached, forget the device in Bluetooth settings, reset the FIRE, and pair again.

## Implemented compatibility surface

- BLE HID over GATT, vendor Usage Page `0xFF00`
- Standard BLE Battery Service populated from the FIRE IP5306
- HID Report ID `6`, 63-byte report data (64 bytes including report ID)
- Work Louder identity, VID `0x303A`, PID `0x8360`
- RPC channel `2`, up to 61 payload bytes per HID frame
- `sys.version`
- `device.status`
- `v.oai.thstatus`
- `v.oai.rgbcfg`
- `v.oai.hid` notifications
- `v.oai.rad` notifications
- Encoder notifications (`ENC`, `ENC_CW`, `ENC_CC`)
- Live profile/layer status and a chip-derived unique serial number

This is a clean-room compatibility implementation based on observed app/device behavior. It is not an official OpenAI, Codex, or Work Louder product.

## Documentation

- [中文使用手册](docs/Codex_FACES_Bridge_使用手册.md)
