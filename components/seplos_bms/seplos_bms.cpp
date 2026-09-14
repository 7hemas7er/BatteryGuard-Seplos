// Modified from esphome-seplos-bms by Sebastian Syska (@syssi),
// licensed under the Apache License, Version 2.0.
//
// Changes in this fork: adds decoding of the Seplos telesignalization frame
// (CID2 0x44), which upstream does not request. The component alternates
// between the 0x42 telemetry command and the 0x44 alarm command, recognises
// each reply by its frame length, and exposes the decoded warning /
// protection / system fault flags.
//
// See NOTICE and README.md for details.

#include "seplos_bms.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome::seplos_bms {

static const char *const TAG = "seplos_bms";

static const uint8_t MAX_NO_RESPONSE_COUNT = 5;

void SeplosBms::on_seplos_modbus_data(const std::vector<uint8_t> &data) {
  this->reset_online_status_tracker_();

  // The response CID2 only carries a return code (0x00 = normal), not the
  // original command, so the reply has to be recognised by its shape.
  //
  // Do NOT route by the command we last sent: a late or lost reply leaves the
  // bookkeeping pointing at the other command, and the frame is then handed to
  // the wrong decoder. Decoding a telemetry frame as an alarm frame reads cell
  // voltages and temperatures as alarm bitfields and publishes a burst of
  // entirely fictional protections.
  //
  // The two frame types are unambiguous by length, for any supported cell
  // count, so the size is what decides:
  //
  //   num_of_cells   telemetry_frame   alarm_frame
  //   8              65                43-45
  //   14             77                51
  //   15             79                52-54
  //   16             81                55
  //
  // Alarm frames are checked first: at 8 cells they can reach 45 bytes, which
  // would also satisfy the telemetry lower bound of 44.
  if (data.size() >= 9 && data.size() < 60 && data[8] >= 8 && data[8] <= 16) {
    this->on_telesignalization_data_(data);
    return;
  }

  if (data.size() >= 44 && data[8] >= 8 && data[8] <= 16) {
    this->on_telemetry_data_(data);
    return;
  }

  ESP_LOGW(TAG, "Unhandled data received (data_len: 0x%02X): %s", data[5],
           format_hex_pretty(&data.front(), data.size()).c_str());  // NOLINT
}

void SeplosBms::on_telemetry_data_(const std::vector<uint8_t> &data) {
  auto seplos_get_16bit = [&](size_t i) -> uint16_t {
    return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0);
  };

  ESP_LOGI(TAG, "Telemetry frame (%zu bytes) received", data.size());
  ESP_LOGVV(TAG, "  %s", format_hex_pretty(&data.front(), data.size()).c_str());  // NOLINT

  // ->
  // 0x2000460010960001100CD70CE90CF40CD60CEF0CE50CE10CDC0CE90CF00CE80CEF0CEA0CDA0CDE0CD8060BA60BA00B970BA60BA50BA2FD5C14A0344E0A426803134650004603E8149F0000000000000000
  // 0x26004600307600011000000000000000000000000000000000000000000000000000000000000000000608530853085308530BAC0B9000000000002D0213880001E6B8
  //
  // *Data*
  //
  // Byte   Address Content: Description                      Decoded content               Coeff./Unit
  //   0    0x20             Protocol version      VER        2.0
  //   1    0x00             Device address        ADR
  //   2    0x46             Device type           CID1       Lithium iron phosphate battery BMS
  //   3    0x00             Function code         CID2       0x00: Normal, 0x01 VER error, 0x02 Chksum error, ...
  //   4    0x10             Data length checksum  LCHKSUM
  //   5    0x96             Data length           LENID      150 / 2 = 75
  //   6      0x00           Data flag
  //   7      0x01           Command group
  ESP_LOGV(TAG, "Command group: %d", data[7]);
  //   8      0x10           Number of cells                  16
  uint8_t cells = (this->override_cell_count_) ? this->override_cell_count_ : data[8];

  ESP_LOGV(TAG, "Number of cells: %d", cells);
  //   9      0x0C 0xD7      Cell voltage 1                   3287 * 0.001f = 3.287         V
  //   11     0x0C 0xE9      Cell voltage 2                   3305 * 0.001f = 3.305         V
  //   ...    ...            ...
  //   39     0x0C 0xD8      Cell voltage 16                                                V
  float min_cell_voltage = 100.0f;
  float max_cell_voltage = -100.0f;
  float average_cell_voltage = 0.0f;
  uint8_t min_voltage_cell = 0;
  uint8_t max_voltage_cell = 0;
  for (uint8_t i = 0; i < std::min((uint8_t) 16, cells); i++) {
    float cell_voltage = (float) seplos_get_16bit(9 + (i * 2)) * 0.001f;
    average_cell_voltage = average_cell_voltage + cell_voltage;
    if (cell_voltage < min_cell_voltage) {
      min_cell_voltage = cell_voltage;
      min_voltage_cell = i + 1;
    }
    if (cell_voltage > max_cell_voltage) {
      max_cell_voltage = cell_voltage;
      max_voltage_cell = i + 1;
    }
    this->publish_state_(this->cells_[i].cell_voltage_sensor_, cell_voltage);
  }
  average_cell_voltage = average_cell_voltage / cells;

  this->publish_state_(this->min_cell_voltage_sensor_, min_cell_voltage);
  this->publish_state_(this->max_cell_voltage_sensor_, max_cell_voltage);
  this->publish_state_(this->max_voltage_cell_sensor_, (float) max_voltage_cell);
  this->publish_state_(this->min_voltage_cell_sensor_, (float) min_voltage_cell);
  this->publish_state_(this->delta_cell_voltage_sensor_, max_cell_voltage - min_cell_voltage);
  this->publish_state_(this->average_cell_voltage_sensor_, average_cell_voltage);

  uint8_t offset = 9 + (cells * 2);

  //   41     0x06           Number of temperatures           6                             V
  uint8_t temperature_sensors = data[offset];
  ESP_LOGV(TAG, "Number of temperature sensors: %d", temperature_sensors);

  //   42     0x0B 0xA6      Temperature sensor 1             (2982 - 2731) * 0.1f = 25.1          °C
  //   44     0x0B 0xA0      Temperature sensor 2             (2976 - 2731) * 0.1f = 24.5          °C
  //   46     0x0B 0x97      Temperature sensor 3             (2967 - 2731) * 0.1f = 23.6          °C
  //   48     0x0B 0xA6      Temperature sensor 4             (2982 - 2731) * 0.1f = 25.1          °C
  //   50     0x0B 0xA5      Environment temperature          (2981 - 2731) * 0.1f = 25.0          °C
  //   52     0x0B 0xA2      Mosfet temperature               (2978 - 2731) * 0.1f = 24.7          °C
  for (uint8_t i = 0; i < std::min((uint8_t) 6, temperature_sensors); i++) {
    float raw_temperature = (float) seplos_get_16bit(offset + 1 + (i * 2));
    this->publish_state_(this->temperatures_[i].temperature_sensor_, (raw_temperature - 2731.0f) * 0.1f);
  }
  offset = offset + 1 + (temperature_sensors * 2);

  //   54     0xFD 0x5C      Charge/discharge current         signed int?                   A
  float current = (float) ((int16_t) seplos_get_16bit(offset)) * 0.01f;
  this->publish_state_(this->current_sensor_, current);

  //   56     0x14 0xA0      Total battery voltage            5280 * 0.01f = 52.80          V
  float total_voltage = (float) seplos_get_16bit(offset + 2) * 0.01f;
  this->publish_state_(this->total_voltage_sensor_, total_voltage);

  float power = total_voltage * current;
  this->publish_state_(this->power_sensor_, power);
  this->publish_state_(this->charging_power_sensor_, std::max(0.0f, power));               // 500W vs 0W -> 500W
  this->publish_state_(this->discharging_power_sensor_, std::abs(std::min(0.0f, power)));  // -500W vs 0W -> 500W

  //   58     0x34 0x4E      Residual capacity                13390 * 0.01f = 133.90        Ah
  this->publish_state_(this->residual_capacity_sensor_, (float) seplos_get_16bit(offset + 4) * 0.01f);

  //   60     0x0A           Custom number                    10
  //   61     0x42 0x68      Battery capacity                 17000 * 0.01f = 170.00        Ah
  this->publish_state_(this->battery_capacity_sensor_, (float) seplos_get_16bit(offset + 7) * 0.01f);

  //   63     0x03 0x13      Stage of charge                  787 * 0.1f = 78.7             %
  this->publish_state_(this->state_of_charge_sensor_, (float) seplos_get_16bit(offset + 9) * 0.1f);

  //   65     0x46 0x50      Rated capacity                   18000 * 0.01f = 180.00        Ah
  this->publish_state_(this->rated_capacity_sensor_, (float) seplos_get_16bit(offset + 11) * 0.01f);

  if (data.size() < offset + 13 + 2) {
    return;
  }

  //   67     0x00 0x46      Number of cycles                 70
  this->publish_state_(this->charging_cycles_sensor_, (float) seplos_get_16bit(offset + 13));

  if (data.size() < offset + 15 + 2) {
    return;
  }

  //   69     0x03 0xE8      State of health                  1000 * 0.1f = 100.0           %
  this->publish_state_(this->state_of_health_sensor_, (float) seplos_get_16bit(offset + 15) * 0.1f);

  if (data.size() < offset + 17 + 2) {
    return;
  }

  //   71     0x14 0x9F      Port voltage                     5279 * 0.01f = 52.79          V
  this->publish_state_(this->port_voltage_sensor_, (float) seplos_get_16bit(offset + 17) * 0.01f);

  //   73     0x00 0x00      Reserved
  //   75     0x00 0x00      Reserved
  //   77     0x00 0x00      Reserved
  //   79     0x00 0x00      Reserved
}

void SeplosBms::on_telesignalization_data_(const std::vector<uint8_t> &data) {
  // Telesignalization (alarm / protection) frame, requested via CID2 0x44.
  // Always dump the raw frame at INFO level: the official protocol XML
  // (Agreement/16S_V20_ADDR_EN.xml) documents the bit meanings but not the
  // exact framing offsets, so the first real frame is used to confirm them.
  ESP_LOGD(TAG, "Telesignalization frame (%zu bytes): %s", data.size(),
           format_hex_pretty(&data.front(), data.size()).c_str());  // NOLINT

  // Standard Seplos V2.0 alarm INFO layout (after the VER/ADR/CID1/RTN/LEN header):
  //   [6] data flag
  //   [7] command group
  //   [8] number of cells (N)
  //   [9 .. 9+N-1]            per-cell voltage warning (1 byte each)
  //   [9+N]                   number of temperatures (T)
  //   [.. +1 .. +T]           per-temperature warning (1 byte each)
  //   [..]                    charge/discharge current warning (1 byte)
  //   [..]                    total voltage warning (1 byte)
  //   [..]                    number of alarm-event bytes (P)
  //   [.. P bytes]            alarm-event bitfields  <-- mapped below
  if (data.size() < 9) {
    ESP_LOGW(TAG, "Telesignalization frame too short");
    return;
  }
  uint8_t cells = (this->override_cell_count_) ? this->override_cell_count_ : data[8];
  if (cells < 8 || cells > 16) {
    ESP_LOGW(TAG, "Unexpected cell count in alarm frame: %d", cells);
    return;
  }
  size_t offset = 9 + cells;
  if (offset >= data.size()) {
    ESP_LOGW(TAG, "Alarm frame truncated (temperatures)");
    return;
  }
  uint8_t temperature_sensors = data[offset];
  if (temperature_sensors > 8) {
    ESP_LOGW(TAG, "Unexpected temperature count in alarm frame: %d", temperature_sensors);
    return;
  }
  offset = offset + 1 + temperature_sensors;
  // current warning + total voltage warning + alarm-event count
  if (offset + 2 >= data.size()) {
    ESP_LOGW(TAG, "Alarm frame truncated (event header)");
    return;
  }
  uint8_t event_count = data[offset + 2];
  size_t ev = offset + 3;
  // Events 1-6 plus the six state bytes plus events 7-8: 14 bytes have to be
  // there. Refuse to decode a short frame rather than reading whatever follows.
  if (ev + 14 > data.size()) {
    ESP_LOGW(TAG, "Alarm-event region out of range (need %d bytes, have %d)", 14, (int) (data.size() - ev));
    return;
  }
  if (event_count > data.size() - ev)
    event_count = (uint8_t) (data.size() - ev);

  // byte/bit -> meaning, from the official 16S_V20 protocol (Ext_Bit block).
  // prot=true marks a protection (BMS acted); prot=false marks a warning.
  struct AlarmBit {
    uint8_t byte;
    uint8_t bit;
    bool prot;
    const char *name;
  };
  static const AlarmBit ALARM_BITS[] = {
      {0, 0, false, "Voltage sensor fault"},      {0, 1, false, "Temp sensor fault"},
      {0, 2, false, "Current sensor fault"},      {0, 3, false, "Button switch fault"},
      {0, 4, false, "Cell delta-V fault"},        {0, 5, false, "Charge switch fault"},
      {0, 6, false, "Discharge switch fault"},    {0, 7, false, "Current-limit switch fault"},
      {1, 0, false, "Cell high voltage"},         {1, 1, true, "Cell overvoltage"},
      {1, 2, false, "Cell low voltage"},          {1, 3, true, "Cell undervoltage"},
      {1, 4, false, "Pack high voltage"},         {1, 5, true, "Pack overvoltage"},
      {1, 6, false, "Pack low voltage"},          {1, 7, true, "Pack undervoltage"},
      {2, 0, false, "Charge high temp"},          {2, 1, true, "Charge over-temp"},
      {2, 2, false, "Charge low temp"},           {2, 3, true, "Charge under-temp"},
      {2, 4, false, "Discharge high temp"},       {2, 5, true, "Discharge over-temp"},
      {2, 6, false, "Discharge low temp"},        {2, 7, true, "Discharge under-temp"},
      {3, 0, false, "Ambient high temp"},         {3, 1, true, "Ambient over-temp"},
      {3, 2, false, "Ambient low temp"},          {3, 3, true, "Ambient under-temp"},
      {3, 4, true, "MOSFET over-temp"},           {3, 5, false, "MOSFET high temp"},
      {3, 6, false, "Cell low-temp heating"},     {3, 7, false, "Secondary tripping"},
      {4, 0, false, "Charge overcurrent (warn)"}, {4, 1, true, "Charge overcurrent"},
      {4, 2, false, "Discharge overcurrent (warn)"}, {4, 3, true, "Discharge overcurrent"},
      {4, 4, true, "Transient overcurrent"},      {4, 5, true, "Output short circuit"},
      {4, 6, true, "Transient lockout"},          {4, 7, true, "Short-circuit lockout"},
      {5, 0, true, "Charge high-voltage"},        {5, 1, false, "Intermittent supply waiting"},
      {5, 2, false, "Low remaining capacity"},    {5, 3, true, "Remaining capacity protect"},
      {5, 4, true, "Low-voltage charge inhibit"}, {5, 5, true, "Output reverse connection"},
      {5, 6, true, "Output connection failure"},  {12, 4, false, "Auto charging waiting"},
      {12, 5, false, "Manual charging waiting"},  {13, 0, false, "EEPROM fault"},
      {13, 1, false, "RTC fault"},                {13, 2, false, "Voltage cal. missing"},
      {13, 3, false, "Current cal. missing"},     {13, 4, false, "Zero-point cal. missing"},
      {13, 5, false, "Calendar not synced"},
  };

  std::string errors;
  bool any_protection = false;
  bool any_warning = false;
  bool any_system_fault = false;
  for (auto &a : ALARM_BITS) {
    if (a.byte >= event_count)
      continue;
    if ((data[ev + a.byte] >> a.bit) & 0x01) {
      if (!errors.empty())
        errors += "; ";
      errors += a.name;
      if (a.prot)
        any_protection = true;
      else
        any_warning = true;
      if (a.byte == 0 || a.byte == 13)
        any_system_fault = true;
    }
  }

  this->publish_state_(this->errors_text_sensor_, errors);
  this->publish_state_(this->protection_binary_sensor_, any_protection);
  this->publish_state_(this->warning_binary_sensor_, any_warning);
  this->publish_state_(this->system_fault_binary_sensor_, any_system_fault);
}

void SeplosBms::dump_config() {
  ESP_LOGCONFIG(TAG, "SeplosBms:");
  LOG_SENSOR("", "Minimum Cell Voltage", this->min_cell_voltage_sensor_);
  LOG_SENSOR("", "Maximum Cell Voltage", this->max_cell_voltage_sensor_);
  LOG_SENSOR("", "Minimum Voltage Cell", this->min_voltage_cell_sensor_);
  LOG_SENSOR("", "Maximum Voltage Cell", this->max_voltage_cell_sensor_);
  LOG_SENSOR("", "Delta Cell Voltage", this->delta_cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 1", this->cells_[0].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 2", this->cells_[1].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 3", this->cells_[2].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 4", this->cells_[3].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 5", this->cells_[4].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 6", this->cells_[5].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 7", this->cells_[6].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 8", this->cells_[7].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 9", this->cells_[8].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 10", this->cells_[9].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 11", this->cells_[10].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 12", this->cells_[11].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 13", this->cells_[12].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 14", this->cells_[13].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 15", this->cells_[14].cell_voltage_sensor_);
  LOG_SENSOR("", "Cell Voltage 16", this->cells_[15].cell_voltage_sensor_);
  LOG_SENSOR("", "Temperature 1", this->temperatures_[0].temperature_sensor_);
  LOG_SENSOR("", "Temperature 2", this->temperatures_[1].temperature_sensor_);
  LOG_SENSOR("", "Temperature 3", this->temperatures_[2].temperature_sensor_);
  LOG_SENSOR("", "Temperature 4", this->temperatures_[3].temperature_sensor_);
  LOG_SENSOR("", "Temperature 5", this->temperatures_[4].temperature_sensor_);
  LOG_SENSOR("", "Temperature 6", this->temperatures_[5].temperature_sensor_);
  LOG_SENSOR("", "Total Voltage", this->total_voltage_sensor_);
  LOG_SENSOR("", "Current", this->current_sensor_);
  LOG_SENSOR("", "Power", this->power_sensor_);
  LOG_SENSOR("", "Charging Power", this->charging_power_sensor_);
  LOG_SENSOR("", "Discharging Power", this->discharging_power_sensor_);
  LOG_SENSOR("", "Charging cycles", this->charging_cycles_sensor_);
  LOG_SENSOR("", "State of charge", this->state_of_charge_sensor_);
  LOG_SENSOR("", "Residual capacity", this->residual_capacity_sensor_);
  LOG_SENSOR("", "Battery capacity", this->battery_capacity_sensor_);
  LOG_SENSOR("", "Rated capacity", this->rated_capacity_sensor_);
  LOG_SENSOR("", "Average Cell Voltage", this->average_cell_voltage_sensor_);
  LOG_SENSOR("", "State of health", this->state_of_health_sensor_);
  LOG_SENSOR("", "Port Voltage", this->port_voltage_sensor_);
}

float SeplosBms::get_setup_priority() const {
  // After UART bus
  return setup_priority::BUS - 1.0f;
}

void SeplosBms::update() {
  this->track_online_status_();

  // Alternate between telemetry (0x42, analog values) and telesignalization
  // (0x44, alarm/protection flags). Both replies land on the same callback,
  // so we remember which one we asked for to route the right decoder.
  if (this->last_requested_function_ == 0x42) {
    this->last_requested_function_ = 0x44;
    this->send(0x44, this->pack_);
  } else {
    this->last_requested_function_ = 0x42;
    this->send(0x42, this->pack_);
  }
}

void SeplosBms::publish_state_(binary_sensor::BinarySensor *binary_sensor, const bool &state) {
  if (binary_sensor == nullptr)
    return;

  binary_sensor->publish_state(state);
}

void SeplosBms::publish_state_(sensor::Sensor *sensor, float value) {
  if (sensor == nullptr)
    return;

  sensor->publish_state(value);
}

void SeplosBms::publish_state_(text_sensor::TextSensor *text_sensor, const std::string &state) {
  if (text_sensor == nullptr)
    return;

  text_sensor->publish_state(state);
}

void SeplosBms::track_online_status_() {
  if (this->no_response_count_ < MAX_NO_RESPONSE_COUNT) {
    this->no_response_count_++;
  }
  if (this->no_response_count_ == MAX_NO_RESPONSE_COUNT) {
    this->publish_device_unavailable_();
    this->no_response_count_++;
  }
}

void SeplosBms::reset_online_status_tracker_() {
  this->no_response_count_ = 0;
  this->publish_state_(this->online_status_binary_sensor_, true);
}

void SeplosBms::publish_device_unavailable_() {
  this->publish_state_(this->online_status_binary_sensor_, false);
  this->publish_state_(this->errors_text_sensor_, "Offline");

  this->publish_state_(this->min_cell_voltage_sensor_, NAN);
  this->publish_state_(this->max_cell_voltage_sensor_, NAN);
  this->publish_state_(this->min_voltage_cell_sensor_, NAN);
  this->publish_state_(this->max_voltage_cell_sensor_, NAN);
  this->publish_state_(this->delta_cell_voltage_sensor_, NAN);
  this->publish_state_(this->average_cell_voltage_sensor_, NAN);
  this->publish_state_(this->total_voltage_sensor_, NAN);
  this->publish_state_(this->current_sensor_, NAN);
  this->publish_state_(this->power_sensor_, NAN);
  this->publish_state_(this->charging_power_sensor_, NAN);
  this->publish_state_(this->discharging_power_sensor_, NAN);
  this->publish_state_(this->state_of_charge_sensor_, NAN);
  this->publish_state_(this->residual_capacity_sensor_, NAN);
  this->publish_state_(this->battery_capacity_sensor_, NAN);
  this->publish_state_(this->rated_capacity_sensor_, NAN);
  this->publish_state_(this->charging_cycles_sensor_, NAN);
  this->publish_state_(this->state_of_health_sensor_, NAN);
  this->publish_state_(this->port_voltage_sensor_, NAN);

  for (auto &temperature : this->temperatures_) {
    this->publish_state_(temperature.temperature_sensor_, NAN);
  }

  for (auto &cell : this->cells_) {
    this->publish_state_(cell.cell_voltage_sensor_, NAN);
  }
}

}  // namespace esphome::seplos_bms
