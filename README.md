# BatteryGuard-Seplos
ESPHome configuration for monitoring Seplos BMS through Home Assistant. Complete battery monitoring solution with cell voltage tracking, temperature sensing, and power monitoring via RS485/Modbus communication.

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

### WiFi Setup
Add to your ESPHome's `secrets.yaml`:
```yaml
wifi_IOT: "your_ssid"
wifi_IOT_password: "your_password"
```

### External Component
The project uses syssi's Seplos BMS component:
```yaml
external_components:
  - source: github://syssi/esphome-seplos-bms@main
    refresh: 0s
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

## Contributing

Feel free to report issues and propose improvements.

## License

This project is open source. Feel free to use and modify as needed.

## Credits

Based on the [esphome-seplos-bms](https://github.com/syssi/esphome-seplos-bms) component by @syssi.
