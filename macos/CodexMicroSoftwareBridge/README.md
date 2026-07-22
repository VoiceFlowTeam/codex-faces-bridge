# Codex Micro Software Bridge for macOS

This Swift package reproduces the Codex Micro vendor HID interface entirely in
macOS software. It uses the same identity and report protocol as the verified
M5Stack bridge:

- Product: `kbd-1.0-codex-micro`
- Manufacturer: `Work Louder`
- VID/PID: `0x303A` / `0x8360`
- Vendor usage page: `0xFF00`
- Report ID: `6`
- Report size: 63 data bytes, 64 bytes including the report ID
- RPC channel: `2`
- JSON-RPC methods: `sys.version`, `device.status`, `v.oai.thstatus`,
  `v.oai.rgbcfg`, `v.oai.hid`, and `v.oai.rad`

## Architecture

```text
Xiaomi remote -> IOHIDManager/CoreBluetooth -> semantic input mapping
                                                |
Codex <-> macOS virtual HID <-> Codex RPC engine+
```

Standard HID remotes are read with `IOHIDManager`. Private BLE remotes are
discovered with CoreBluetooth; all services, characteristics, and notification
bytes are logged, and exact notifications can be mapped with a JSON file.

## Important macOS requirement

Creating an `IOHIDUserDevice` requires Apple's restricted entitlement:

```text
com.apple.developer.hid.virtual.device
```

An ad-hoc signature containing this entitlement is killed by macOS before
`main()` runs. A signed build therefore requires an Apple-approved entitlement
and a matching signing identity/provisioning setup. A normal Apple Development
or Developer ID certificate is not sufficient on its own: macOS will also kill
the process before `main()` when the signing team has not been granted this
restricted entitlement. Protocol tests and remote probe mode remain usable
without the entitlement.

## Build and test

```sh
cd macos/CodexMicroSoftwareBridge
CLANG_MODULE_CACHE_PATH=.build/module-cache \
SWIFT_MODULECACHE_PATH=.build/module-cache \
swift test --disable-sandbox

sh scripts/build.sh
.build/release/codex-micro-software-bridge --protocol-self-test
```

For a fully entitled build:

```sh
sh scripts/build-and-sign.sh "Apple Development: Your Name (TEAMID)"
.build/release/codex-micro-software-bridge --hid-product "Xiaomi"
```

## Probe a remote before the entitlement is available

Private BLE/GATT remote:

```sh
.build/release/codex-micro-software-bridge \
  --probe-only --ble-list --verbose

.build/release/codex-micro-software-bridge \
  --probe-only --ble-name "Xiaomi" --verbose
```

Use `--ble-list` first when the remote's advertised name is unknown. It lists
nearby advertisements without connecting to them.

Standard HID remote:

```sh
.build/release/codex-micro-software-bridge \
  --probe-only --hid-product "Xiaomi" --verbose
```

If the remote uses private BLE notifications, copy
`ble-map.example.json`, replace its UUID and value fields with captured data,
then run:

```sh
.build/release/codex-micro-software-bridge \
  --ble-name "Xiaomi" --ble-map my-remote-map.json --verbose
```

## Interactive simulation

With an entitled virtual HID build running, commands can be entered on stdin:

```text
tap ACT06
down ACT07
up ACT07
agent 0 tap
rad -90 1
rad -90 0
```

The bridge prints all thread RGB updates received from Codex.
