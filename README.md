# BatteryGuard-seplos
A project to monitor Seplos Battery Management System (BMS) through ESPHome in Home Assistant, providing comprehensive battery monitoring capabilities.

Note: This project is based on esphome-seplos-bms by Sebastian Syska (@syssi).

**It ships a fork of that component that adds alarm monitoring.** Upstream reads
only the telemetry frame (CID2 `0x42`) — voltages, currents, temperatures. The
BMS reports its actual warning and protection flags in a *separate*
telesignalization frame (CID2 `0x44`) that upstream never requests, so a Seplos
pack can sit in protection while every published sensor still reads normal.
This fork alternates between the two commands and exposes the alarms. See
[Alarm monitoring](#alarm-monitoring).

> ### ⚠️ Known limitation — read before relying on the alarm entities
>
> **The alarm flags in this fork produce false positives.** On the author's
> installation the `protection` binary sensor toggles on for a single frame
> roughly 80 times a day, on a pack that is not in protection at all.
>
> **Root cause:** this fork routes an incoming frame by remembering which
> command it last sent. When a reply is late or lost — and the request
> schedule has already moved on — a *telemetry* reply gets handed to the alarm
> decoder. The decoder has no frame-size or temperature-count validation to
> catch it, so it reads cell voltages and temperatures as alarm bitfields.
> A real 16-cell telemetry frame fed through this path decodes as 17
> simultaneous alarms, including "Output short circuit".
>
> **Use [syssi's PR #155](https://github.com/syssi/esphome-seplos-bms/pull/155)
> instead.** It decodes the same frame into 7 granular flags, covers
> `seplos_bms_ble` too, and — crucially — routes by frame size, which makes the
> misrouting above impossible. It is the maintainer's own work, it has been
> mergeable with green CI since July 2025, and it is strictly better than what
> is in this repository.
>
> This repository keeps its fork only so the configuration below stays
> reproducible. If you want alarms that you can trust, track #155.

## Prerequisites

- Home Assistant with ESPHome installed
- ESP32 (DOIT DevKit V1)
- RS485 connection to Seplos BMS (TX: GPIO16, RX: GPIO17)
- Seplos BMS configured with address 0x00 (all DIP switches OFF)

## Features

- Comprehensive battery monitoring:
  - Individual cell voltages (up to 16 cells)
  - Temperature sensors (4 battery + environment + MOSFET)
  - Current and power monitoring
  - Charging and discharging power
  - Battery health and capacity metrics
  - State of charge monitoring

- Alarm monitoring (fork addition):
  - `warning` — the BMS has raised a warning
  - `protection` — the BMS has acted to protect the pack
  - `system fault` — hardware/system fault flag
  - `errors` — text sensor listing the active alarm conditions

- Connectivity:
  - WiFi connectivity
  - Home Assistant API integration
  - OTA updates support
  - Modbus communication protocol

## Installation

1. In Home Assistant, navigate to ESPHome dashboard
2. Click on "+ NEW DEVICE"
3. Select your ESP32
4. Copy the provided YAML file content
5. Update WiFi credentials in your `secrets.yaml`
6. Install the firmware to your device

### Basic Configuration
```yaml
substitutions:
  name: seplos-bms
  device_description: "Monitor a Seplos BMS via RS485"
  tx_pin: GPIO16
  rx_pin: GPIO17
```

### Required secrets
Add to your ESPHome's `secrets.yaml`:
```yaml
wifi_IOT: "your_ssid"
wifi_IOT_password: "your_password"
fallback_password: "captive_portal_password"
api_encryption_key: "base64_32_byte_key"
ota_password: "your_ota_password"
```

### External Component
This configuration uses the **local** `components/` directory in this
repository, not the upstream component, because the alarm decoding lives there:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [seplos_bms, seplos_modbus]
    refresh: 0s
```

Copy the `components/` folder next to your YAML file, so you end up with
`/config/esphome/components/seplos_bms/` and
`/config/esphome/components/seplos_modbus/`.

To go back to plain upstream behaviour (telemetry only, no alarms), replace the
block above with `source: github://syssi/esphome-seplos-bms@main` and remove the
`binary_sensor` and `text_sensor` sections that reference `warning`,
`protection`, `system_fault` and `errors`.

## Alarm monitoring

The Seplos V2.0 protocol splits its data across two commands:

| CID2 | Frame | Contents |
|---|---|---|
| `0x42` | Telemetry | cell voltages, currents, temperatures, SOC |
| `0x44` | Telesignalization | per-cell, per-temperature and system alarm bitfields |

Upstream only ever issues `0x42`. This fork alternates: each `update_interval`
tick requests the other command, so with the default 5 s interval each frame
type arrives roughly every 10 s.

The alarm bytes distinguish *warnings* (the BMS is unhappy) from *protections*
(the BMS has already acted — cut charge or discharge). Those are aggregated into
the `warning` and `protection` binary sensors, with the individual conditions
listed in the `errors` text sensor.

**Do not drive an automation directly off these flags** — see the known
limitation above. If you automate on them anyway, require the state to hold for
at least a minute (`for: "00:01:00"`), which filters the single-frame spikes.

Add to your YAML:

```yaml
binary_sensor:
  - platform: seplos_bms
    seplos_bms_id: bms0
    online_status:
      name: "${name} online"
    warning:
      name: "${name} warning"
    protection:
      name: "${name} protection"
    system_fault:
      name: "${name} system fault"

text_sensor:
  - platform: seplos_bms
    seplos_bms_id: bms0
    errors:
      name: "${name} errors"
```

## Communication Protocol

The device communicates with the BMS using Modbus over RS485:
- Baud rate: 9600 (or 19200 for some models)
- Protocol version: 0x20 (Seplos)
- Buffer size: 384 bytes
- Address: 0x00 (default)

## Available Sensors

The project includes extensive sensor monitoring:
- Cell voltages (1-16)
- Temperature sensors (1-6)
- Total voltage and current
- Power metrics (charging/discharging)
- Battery capacity and health
- State of charge
- Charging cycles

## Troubleshooting

- Check device logs in ESPHome dashboard
- Verify RS485 connections and baud rate
- Ensure DIP switches on BMS are correctly configured
- Monitor debug output in ESPHome logs
- If the alarm entities stay `unknown`, the `0x44` frame is not being answered.
  Set `logger: level: DEBUG` and look for `Telesignalization frame (N bytes)` in
  the logs — the raw frame is printed there, which is what you need to confirm
  the offsets for your firmware revision.

## Contributing

Feel free to report issues and propose improvements.

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

The component sources under `components/` are derived from @syssi's
Apache-2.0 licensed work; the three modified files carry a notice of change at
the top, as required.

## Credits

Based on the [esphome-seplos-bms](https://github.com/syssi/esphome-seplos-bms)
component by Sebastian Syska (@syssi), licensed under Apache 2.0. The
telesignalization (`0x44`) decoding is the addition made here.

