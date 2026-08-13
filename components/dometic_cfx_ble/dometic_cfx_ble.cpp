#include "dometic_cfx_ble.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

extern "C" {
#include "esp_gattc_api.h"
}

namespace esphome {
namespace dometic_cfx_ble {

// UUID strings (CFX5 new protocol)
static const char *SERVICE_UUID = "537a0400-0995-481f-926c-1604e23fd515";
static const char *WRITE_UUID   = "537a0401-0995-481f-926c-1604e23fd515";
static const char *NOTIFY_UUID  = "537a0402-0995-481f-926c-1604e23fd515";

// CFX5 DDM Protocol (reverse engineered from HCI snoop log)
// Subscribe format:  0x12 p1 p2 p3 p4
// Publish format:    0x10 p1 p2 p3 p4 value...
// Values are int32 little-endian, /1000 for temp (millidegrees) and voltage (millivolts)

static const char *power_source_str(int v) {
  switch (v) {
    case 0: return "AC";
    case 1: return "DC";
    case 2: return "Solar";
    default: return nullptr;
  }
}

// ----------------- Topic table (CFX5 new protocol) --------------------------
// param = {p1, p2, p3, p4} as sent in DDM frames
// Group 0x00 0x1A = realtime data
// Group 0x01 0x00 = device info
// Group 0x01 0x02 = compartment detail

const std::map<std::string, TopicInfo> TOPICS = {
    // Realtime data group [xx 00 00 1A]
    {"COMPARTMENT_0_MEASURED_TEMPERATURE", {{0x04, 0x00, 0x00, 0x1A}, "INT32_MILLIDEGREE_CELSIUS", "Compartment 1 current temp"}},
    {"COMPARTMENT_0_SET_TEMPERATURE",      {{0x05, 0x00, 0x00, 0x1A}, "INT32_MILLIDEGREE_CELSIUS", "Compartment 1 set temp"}},
    {"COOLER_POWER",                       {{0x03, 0x00, 0x00, 0x1A}, "INT8_BOOLEAN",  "Cooler power"}},
    {"COMPARTMENT_0_POWER",                {{0x0B, 0x00, 0x00, 0x1A}, "INT8_BOOLEAN",  "Compartment 1 power"}},
    {"BATTERY_VOLTAGE_LEVEL",              {{0x0C, 0x00, 0x00, 0x1A}, "INT32_MILLIVOLT","Battery voltage"}},                     // Battery/supply voltage (V)
    {"POWER_SOURCE",                       {{0x10, 0x00, 0x00, 0x1A}, "POWER_SOURCE_TEXT", "Power source"}},
    {"BLUETOOTH_MODE",                     {{0x06, 0x00, 0x00, 0x1A}, "INT8_BOOLEAN",  "Bluetooth mode (unused)"}},
    {"COMPARTMENT_0_DOOR_OPEN",            {{0x07, 0x00, 0x00, 0x1A}, "INT8_BOOLEAN",  "Compartment 1 door open"}},
    {"ACTIVE_ALARMS",                      {{0x12, 0x00, 0x00, 0x1A}, "RAW",           "Door and temp ±5C Array of active system alarms (active when non-empty)"}},
    // Device info group [xx 00 01 00]
    {"DEVICE_NAME",                        {{0x07, 0x00, 0x00, 0x1C}, "UTF8_STRING",   "Device name"}},
    {"FIRMWARE_VERSION",                   {{0x02, 0x00, 0x00, 0x00}, "UTF8_STRING",   "Firmware version"}},
    {"DEVICE_MODEL",                       {{0x07, 0x00, 0x01, 0x00}, "UTF8_STRING",   "Device model"}},
// Newly Identified Topics
    {"COMPARTMENT_0_TEMPERATURE_RANGE",    {{0x08, 0x00, 0x00, 0x1A}, "RAW",           "Temp min/max bounds"}},
    {"BATTERY_PROTECTION_LEVEL",           {{0x0D, 0x00, 0x00, 0x1A}, "INT8_NUMBER", "Battery protection level"}},
    {"COMPARTMENT_COUNT",                  {{0x00, 0x00, 0x01, 0x02}, "INT32_NUMBER",  "Compartment count"}},
    {"ICEMAKER_COUNT",                     {{0x03, 0x00, 0x01, 0x02}, "INT32_NUMBER",  "Icemaker count"}},
    {"DETAILED_FIRMWARE_VERSION",          {{0x07, 0x00, 0x06, 0x14}, "UTF8_STRING",   "Controller firmware details"}},
    {"HARDWARE_MODEL_ID",                  {{0x1B, 0x00, 0x00, 0x00}, "INT32_NUMBER",  "Hardware model capacity ID"}},
    {"COMPARTMENT_0_TEMP_HISTORY_JSON",    {{0x03, 0x00, 0x02, 0x18}, "UTF8_STRING",   "70-min temp history JSON"}},
    {"COMPARTMENT_0_TEMP_HISTORY_BINARY",  {{0x04, 0x00, 0x02, 0x18}, "RAW",           "Binary representation of temperature 70-min history dataset"}},
};

// All subscribe params as sent by the official Dometic app (reverse engineered)
static const uint8_t SUBSCRIBE_ALL[][4] = {
    // Group 00 00 00 (general status)
    {0x00, 0x00, 0x00, 0x00},
    {0x01, 0x00, 0x00, 0x00},
    {0x02, 0x00, 0x00, 0x00},
    {0x07, 0x00, 0x00, 0x00},
    {0x08, 0x00, 0x00, 0x00},
    {0x09, 0x00, 0x00, 0x00},
    {0x0C, 0x00, 0x00, 0x00},
    {0x11, 0x00, 0x00, 0x00},  // BLE MAC Address or is it? MAC is xx:xx:xx:3A but reports xx:xx:xx:38
    {0x12, 0x00, 0x00, 0x00},
    {0x1A, 0x00, 0x00, 0x00},
    {0x1B, 0x00, 0x00, 0x00},  // possible door status
    {0x1D, 0x00, 0x00, 0x00},  // possible door alert or is it Global System Error Code?
    // Group 00 01 02 (compartment detail)
    {0x00, 0x00, 0x01, 0x02},  // possible Compartment Count = 1 (01000000)
    {0x01, 0x00, 0x01, 0x02},
    {0x02, 0x00, 0x01, 0x02},
    {0x03, 0x00, 0x01, 0x02},  // possible Icemaker Count = 0 (00000000)
    {0x04, 0x00, 0x01, 0x02},
    {0x05, 0x00, 0x01, 0x02},
    {0x07, 0x00, 0x01, 0x00},  // device model
    {0x01, 0x00, 0x07, 0x02},
    // Group 00 00 1A (realtime data)
    {0x03, 0x00, 0x00, 0x1A},  // cooler power
    {0x04, 0x00, 0x00, 0x1A},  // measured temp
    {0x05, 0x00, 0x00, 0x1A},  // set temp
    {0x06, 0x00, 0x00, 0x1A},  // power source
    {0x07, 0x00, 0x00, 0x1A},  // DOOR_OPEN
    {0x08, 0x00, 0x00, 0x1A},  // battery+current  or is it Temp Range: -22°C to +20°C (10aaffff204e0000)
    {0x0B, 0x00, 0x00, 0x1A},  // compartment power
    {0x0C, 0x00, 0x00, 0x1A},  // Battery/Supply voltage (V)
    {0x10, 0x00, 0x00, 0x1A},  // Power source
    {0x11, 0x00, 0x00, 0x1A},
    {0x12, 0x00, 0x00, 0x1A},  // Door and temp ±5c ALERT
    {0x0D, 0x00, 0x00, 0x1A},  // Battery Protection Mode!!
    // Other groups
    {0x07, 0x00, 0x00, 0x1C},  // device name
    {0x07, 0x00, 0x06, 0x14},  // Controller Firmware Versions ASCII String MC1-HMI_2.1.1;MC1-FS_2.0.0;MC1_2.0.0
    {0x03, 0x00, 0x07, 0x14},
    // Group 1F (unknown - may contain power source)
    {0x00, 0x00, 0x00, 0x1F},   // possible Power Mode = 1 ? (01000000)
    // Group 18 params (unknown) ??History Group?? hour, day, week?
    {0x00, 0x00, 0x00, 0x18},   // = 01000000 possible TEMP_HISTORY_STATUS
    {0x01, 0x00, 0x01, 0x18},   // (=CFX5_HIST_TEMP_SZ) possible TEMP_HISTORY_DATASET_NAME
    {0x02, 0x00, 0x01, 0x18},
    {0x03, 0x00, 0x01, 0x18},   // HOUR:lsrcfg0,DAY:lsrcfg1,WEEK:lsrcfg2 ASCII string
    {0x00, 0x00, 0x02, 0x18},   //= 01000000 lsrcfg0 (HOUR) Status Flag (1 = Valid / Ready) 
    {0x01, 0x00, 0x02, 0x18},   //= "HOUR" broadcasts array automatically every 10 minutes
    {0x02, 0x00, 0x02, 0x18},   //"mccc0ctemp -f 10m -t :1h10m" (Filter: 10 min, Timeframe: 1 hr 10 min)
    {0x03, 0x00, 0x02, 0x18},   // = JSON Array ({"t":0, "i":600, "d":[...]}, 7 samples at 10-minute intervals) UTF8_STRING
    {0x04, 0x00, 0x02, 0x18},   // = Binary Array (the raw hex twin of the JSON array) [04 00 02 18] really is a binary representation of the same dataset contained in [03 00 02 18]
    {0x05, 0x00, 0x02, 0x18},   //possible = "DAY"
    {0x06, 0x00, 0x02, 0x18},   //possible = Filter command for 24-hour logging (e.g., sample every 1 hour)
    {0x07, 0x00, 0x02, 0x18},   //possible = DAY JSON Array (24 hours of historical temperatures)
    {0x08, 0x00, 0x02, 0x18},   //possible = DAY Binary Array
    {0x09, 0x00, 0x02, 0x18},
    {0x0A, 0x00, 0x02, 0x18},
    {0x0B, 0x00, 0x02, 0x18},

    // ============================================================
    // DIAGNOSTIC SWEEP — hunting for "battery protection mode".
    // None of these are confirmed. 

    // Delete this block once the real topic is identified and named.
    {0x01, 0x00, 0x01, 0x00},  // 
    {0x01, 0x00, 0x03, 0x02},  
    {0x01, 0x00, 0x09, 0x02},  
    {0x01, 0x00, 0x0A, 0x02},  
    {0x01, 0x01, 0x0A, 0x02},  
    {0x01, 0x02, 0x0A, 0x02},  
    {0x01, 0x00, 0x06, 0x14},
    {0x01, 0x00, 0x07, 0x14},
   // {0x01, 0x00, 0x08, 0x14},  // incremented every second possible internal uptime/heartbeat counter
    {0x01, 0x00, 0x03, 0x17},  
    {0x01, 0x00, 0x00, 0x1C},  // Exact Model Name Sensor UTF8_STRING
    {0x01, 0x00, 0x00, 0x1F},  
    {0x00, 0x00, 0x00, 0x1A},
    {0x02, 0x00, 0x00, 0x1A},
    {0x09, 0x00, 0x00, 0x1A},
    {0x0A, 0x00, 0x00, 0x1A},
   // {0x0D, 0x00, 0x00, 0x1A},  // Battery Protection Mode!!
    {0x0E, 0x00, 0x00, 0x1A},  // possible Cooling Active
    {0x0F, 0x00, 0x00, 0x1A},
    {0x13, 0x00, 0x00, 0x1A},  // Device Serjal
    {0x14, 0x00, 0x00, 0x1A},
    {0x15, 0x00, 0x00, 0x1A},
    {0x02, 0x00, 0x00, 0x1F},
    {0x03, 0x00, 0x00, 0x1F},
    {0x0A, 0x00, 0x00, 0x00},
    {0x0D, 0x00, 0x00, 0x00},
    {0x13, 0x00, 0x00, 0x00},
    // ============================================================


};
static const size_t SUBSCRIBE_ALL_COUNT = sizeof(SUBSCRIBE_ALL) / sizeof(SUBSCRIBE_ALL[0]);

// DDM action bytes (CFX5 new protocol)
enum : uint8_t {
  ACTION_PUB  = 0x10,  // CFX5 → App: publish value
  ACTION_SET  = 0x11,  // App → CFX5: set value
  ACTION_SUB  = 0x12,  // App → CFX5: subscribe to topic
};

// ----------------- Component lifecycle --------------------------------------

void DometicCfxBle::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Dometic CFX BLE (CFX5 protocol)...");
  ESP_LOGCONFIG(TAG, "  Product type: %d", this->product_type_);
}

void DometicCfxBle::loop() {
  if (!this->connected_ || this->write_handle_ == 0)
    return;

  uint32_t now = millis();

  // Staggered subscribe: send one subscription every 200ms
  if (!this->subscribed_ && this->subscribe_idx_ < SUBSCRIBE_ALL_COUNT) {
    if (now - this->last_subscribe_ms_ > 200) {
      uint8_t frame[5] = {ACTION_SUB,
                          SUBSCRIBE_ALL[this->subscribe_idx_][0],
                          SUBSCRIBE_ALL[this->subscribe_idx_][1],
                          SUBSCRIBE_ALL[this->subscribe_idx_][2],
                          SUBSCRIBE_ALL[this->subscribe_idx_][3]};
      esp_ble_gattc_write_char(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          this->write_handle_,
          sizeof(frame), frame,
          ESP_GATT_WRITE_TYPE_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      this->subscribe_idx_++;
      this->last_subscribe_ms_ = now;
      if (this->subscribe_idx_ >= SUBSCRIBE_ALL_COUNT) {
        this->subscribed_ = true;
        ESP_LOGI(TAG, "All CFX5 parameters subscribed (%u topics)", (unsigned)SUBSCRIBE_ALL_COUNT);
      }
    }
    return;
  }


  // Send queued set commands
  if (this->send_queue_.empty())
    return;

  auto frame = this->send_queue_.front();

  ESP_LOGV(TAG, "TX frame (%u bytes): %s",
           (unsigned) frame.size(),
           format_hex(frame.data(), frame.size()).c_str());

  auto *client = this->parent_;
  if (client == nullptr) {
    ESP_LOGW(TAG, "BLE client parent is null");
    return;
  }

  auto status = esp_ble_gattc_write_char(
      client->get_gattc_if(),
      client->get_conn_id(),
      this->write_handle_,
      frame.size(),
      const_cast<uint8_t *>(frame.data()),
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);

  if (status != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send frame: %d", status);
  } else {
    this->send_queue_.pop();
  }

  this->last_activity_ms_ = now;
}

void DometicCfxBle::dump_config() {
  ESP_LOGCONFIG(TAG, "Dometic CFX BLE (CFX5 protocol):");
  ESP_LOGCONFIG(TAG, "  Product type: %d", this->product_type_);
}

// ----------------- Frame helpers --------------------------------------------

void DometicCfxBle::send_pub(const std::string &topic, const std::vector<uint8_t> &value) {
  auto it = TOPICS.find(topic);
  if (it == TOPICS.end()) {
    ESP_LOGW(TAG, "send_pub: unknown topic '%s'", topic.c_str());
    return;
  }
  const TopicInfo &info = it->second;

  std::vector<uint8_t> frame;
  frame.reserve(1 + 4 + value.size());
  frame.push_back(ACTION_SET);
  frame.insert(frame.end(), info.param, info.param + 4);
  frame.insert(frame.end(), value.begin(), value.end());

  this->send_queue_.push(std::move(frame));
}

void DometicCfxBle::send_switch(const std::string &topic, bool value) {
  auto payload = this->encode_from_bool_(value);
  this->send_pub(topic, payload);
}

void DometicCfxBle::send_number(const std::string &topic, float value) {
  auto payload = this->encode_from_float_millidegree_(value);
  this->send_pub(topic, payload);
}

// battery protection mode
void DometicCfxBle::send_enum(const std::string &topic, uint8_t value) {
  // The Dometic CFX5 encodes enums as 32-bit little-endian integers
  int32_t val32 = value;
  std::vector<uint8_t> payload(4);
  memcpy(payload.data(), &val32, 4);
  this->send_pub(topic, payload);
}


// ----------------- GATTC callbacks ------------------------------------------

void DometicCfxBle::gattc_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "GATT open ok");
        this->connected_ = true;
        this->last_activity_ms_ = millis();
      } else {
        ESP_LOGW(TAG, "GATT open failed: %d", param->open.status);
      }
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGI(TAG, "Service discovery complete");

      auto *write_chr = this->parent_->get_characteristic(
          esp32_ble_tracker::ESPBTUUID::from_raw(SERVICE_UUID),
          esp32_ble_tracker::ESPBTUUID::from_raw(WRITE_UUID));

      auto *notify_chr = this->parent_->get_characteristic(
          esp32_ble_tracker::ESPBTUUID::from_raw(SERVICE_UUID),
          esp32_ble_tracker::ESPBTUUID::from_raw(NOTIFY_UUID));

      if (write_chr == nullptr || notify_chr == nullptr) {
        ESP_LOGW(TAG, "Dometic CFX5 service/characteristics not found");
        this->connected_ = false;
        this->parent_->disconnect();
        return;
      }

      this->write_handle_ = write_chr->handle;
      this->notify_handle_ = notify_chr->handle;

      ESP_LOGI(TAG, "Found write handle=0x%04X notify handle=0x%04X",
               this->write_handle_, this->notify_handle_);

      auto status = esp_ble_gattc_register_for_notify(
          gattc_if,
          this->parent_->get_remote_bda(),
          this->notify_handle_);

      if (status != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register for notifications: %d", status);
      } else {
        this->notify_registered_ = true;
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Notifications registered — starting full subscribe");
        this->subscribed_ = false;
        this->subscribe_idx_ = 0;
        this->last_subscribe_ms_ = millis();
      } else {
        ESP_LOGW(TAG, "REG_FOR_NOTIFY failed: %d", param->reg_for_notify.status);
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_)
        break;
      this->handle_notify_(param->notify.value, param->notify.value_len);
      this->last_activity_ms_ = millis();
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGI(TAG, "Disconnected from Dometic CFX device");
      this->connected_ = false;
      this->write_handle_ = 0;
      this->notify_handle_ = 0;
      this->notify_registered_ = false;
      this->subscribed_ = false;
      this->subscribe_idx_ = 0;
      while (!this->send_queue_.empty())
        this->send_queue_.pop();
      break;
    }

    default:
      break;
  }
}

// ----------------- Notification / DDM decode --------------------------------

void DometicCfxBle::handle_notify_(const uint8_t *data, uint16_t length) {
  if (data == nullptr || length == 0)
    return;

  uint8_t action = data[0];

  ESP_LOGD(TAG, "RX frame action=0x%02X len=%u: %s",
           action, (unsigned) length,
           format_hex(data, length).c_str());

  // CFX5 sends PUB (0x10) frames with: action p1 p2 p3 p4 value...
  if (action != ACTION_PUB) {
    ESP_LOGV(TAG, "Ignoring non-PUB action 0x%02X", action);
    return;
  }

  if (length < 5) {
    ESP_LOGW(TAG, "PUB frame too short: %u", (unsigned) length);
    return;
  }

  // Match param bytes to topic
  uint8_t p1 = data[1], p2 = data[2], p3 = data[3], p4 = data[4];

  std::string topic;
  const TopicInfo *info = nullptr;

  for (const auto &kv : TOPICS) {
    const TopicInfo &ti = kv.second;
    if (ti.param[0] == p1 && ti.param[1] == p2 &&
        ti.param[2] == p3 && ti.param[3] == p4) {
      topic = kv.first;
      info = &ti;
      break;
    }
  }

  std::vector<uint8_t> payload;
  if (length > 5)
    payload.assign(data + 5, data + length);

  if (info == nullptr) {
    ESP_LOGD(TAG, "Unknown DDM param [%02X %02X %02X %02X] value: %s",
             p1, p2, p3, p4,
             format_hex(payload.data(), payload.size()).c_str());
    return;
  }

  ESP_LOGD(TAG, "PUB %s = %s",
           topic.c_str(),
           format_hex(payload.data(), payload.size()).c_str());

  this->update_entity_(topic, payload, std::string(info->type ? info->type : ""));
}

// ----------------- Entity update --------------------------------------------

void DometicCfxBle::update_entity_(const std::string &topic,
                                   const std::vector<uint8_t> &value,
                                   const std::string &type_hint) {
  if (auto it = sensors_.find(topic); it != sensors_.end()) {
    float v = this->decode_to_float_(value, type_hint);
    if (!std::isnan(v)) {
      ESP_LOGD(TAG, "%s = %.3f", topic.c_str(), v);
      it->second->publish_state(v);
      if (topic == "COMPARTMENT_0_MEASURED_TEMPERATURE")
        this->cfx_measured_temp_ = v;
      if (topic == "BATTERY_VOLTAGE_LEVEL")
        ; // no climate update needed
    }
    return;
  }

  //Old DOOR_ALERT replaced by ACTIVE_ALARMS
//  if (topic == "DOOR_ALERT") {
//    bool alarm = !value.empty();
//    ESP_LOGD(TAG, "DOOR_ALERT = %s (raw len=%u)", alarm ? "ON" : "OFF", (unsigned)value.size());
//    if (auto it = binary_sensors_.find(topic); it != binary_sensors_.end()) {
//      it->second->publish_state(alarm);
//    }
//    return;
//  }

// --- New ALARM SCANNER ---
  if (topic == "ACTIVE_ALARMS") {
    bool door_alarm = false;
    bool temp_alarm = false;

    // Scan the payload array for known 2-byte error codes
    for (size_t i = 0; i < value.size(); i += 2) {
      if (value[i] == 0x17) door_alarm = true;
      if (value[i] == 0x1B) temp_alarm = true;
    }

    ESP_LOGD(TAG, "ALARMS: Door=%s, Temp=%s (raw len=%u)", 
             door_alarm ? "ON" : "OFF", 
             temp_alarm ? "ON" : "OFF", 
             (unsigned)value.size());

    if (auto it = binary_sensors_.find("DOOR_ALERT"); it != binary_sensors_.end()) {
      it->second->publish_state(door_alarm);
    }
    if (auto it = binary_sensors_.find("TEMP_ALERT"); it != binary_sensors_.end()) {
      it->second->publish_state(temp_alarm);
    }
    return;
  }
  // ------------------------------
  
  if (auto it = binary_sensors_.find(topic); it != binary_sensors_.end()) {
    bool v = this->decode_to_bool_(value);
    ESP_LOGD(TAG, "%s = %s", topic.c_str(), v ? "ON" : "OFF");
    it->second->publish_state(v);
    return;
  }

  if (auto it = switches_.find(topic); it != switches_.end()) {
    bool v = this->decode_to_bool_(value);
    ESP_LOGD(TAG, "%s = %s", topic.c_str(), v ? "ON" : "OFF");
    it->second->publish_state(v);
  }

  if (auto it = numbers_.find(topic); it != numbers_.end()) {
    float v = this->decode_to_float_(value, type_hint);
    if (!std::isnan(v)) {
      ESP_LOGD(TAG, "%s = %.3f", topic.c_str(), v);
      it->second->publish_state(v);
      if (topic == "COMPARTMENT_0_SET_TEMPERATURE")
        this->cfx_set_temp_ = v;
    }
  }

  if (auto it = text_sensors_.find(topic); it != text_sensors_.end()) {
    std::string s = this->decode_to_string_(value, type_hint);
    ESP_LOGD(TAG, "%s = %s", topic.c_str(), s.c_str());
    it->second->publish_state(s);
  }

  // Select battery protection mode
  if (auto it = selects_.find(topic); it != selects_.end()) {
    float v = this->decode_to_float_(value, type_hint);
    if (!std::isnan(v)) {
      int mode = static_cast<int>(v);
      std::string mode_str = "Unknown";
      if (mode == 0) mode_str = "Low";
      else if (mode == 1) mode_str = "Medium";
      else if (mode == 2) mode_str = "High";

      ESP_LOGD(TAG, "%s = %s", topic.c_str(), mode_str.c_str());
      it->second->publish_state(mode_str);
    }
  }

  // Update internal state for climate
  if (topic == "COMPARTMENT_0_MEASURED_TEMPERATURE") {
    this->cfx_measured_temp_ = this->decode_to_float_(value, type_hint);
  } else if (topic == "COMPARTMENT_0_SET_TEMPERATURE") {
    this->cfx_set_temp_ = this->decode_to_float_(value, type_hint);
  } else if (topic == "COMPARTMENT_0_POWER") {
    this->cfx_power_ = this->decode_to_bool_(value);
  }

  // Update climate entity when relevant topics change
  if (!climate_.empty() &&
      (topic == "COMPARTMENT_0_MEASURED_TEMPERATURE" ||
       topic == "COMPARTMENT_0_SET_TEMPERATURE" ||
       topic == "COMPARTMENT_0_POWER")) {
    for (auto &kv : climate_) {
      auto *c = static_cast<DometicCfxBleClimate *>(kv.second);
      c->update_state(this->cfx_power_, this->cfx_set_temp_, this->cfx_measured_temp_);
    }
  }

  
}

// ----------------- Decode ---------------------------------------------------

float DometicCfxBle::decode_to_float_(const std::vector<uint8_t> &bytes,
                                      const std::string &type_hint) {
  if (type_hint == "INT32_MILLIDEGREE_CELSIUS") {
    // 4-byte int32 LE, /1000 = °C
    if (bytes.size() < 4) return NAN;
    int32_t raw;
    memcpy(&raw, bytes.data(), 4);
    return static_cast<float>(raw) / 1000.0f;
  }

  if (type_hint == "INT32_MILLIVOLT") {
    // 4-byte uint32 LE, /1000 = V
    if (bytes.size() < 4) return NAN;
    uint32_t raw;
    memcpy(&raw, bytes.data(), 4);
    return static_cast<float>(raw) / 1000.0f;
  }

  if (type_hint == "INT8_NUMBER" || type_hint == "UINT8_NUMBER") {
    if (bytes.empty()) return NAN;
    return static_cast<float>(bytes[0]);
  }

  return NAN;
}

bool DometicCfxBle::decode_to_bool_(const std::vector<uint8_t> &bytes) {
  if (bytes.empty()) return false;
  return bytes[0] != 0;
}

//std::string DometicCfxBle::decode_to_string_(const std::vector<uint8_t> &bytes,
//                                              const std::string &type_hint) {
//  if (type_hint == "POWER_SOURCE_TEXT") {
//    if (bytes.empty()) return "Unknown";
//    switch (bytes[0]) {
//      case 0: return "AC";
//      case 1: return "DC";
//      case 2: return "Solar";
//      default: return "Unknown";
//    }
//  }
//  if (bytes.empty()) return "";
//  size_t end = 0;
//  while (end < bytes.size() && bytes[end] != 0x00)
//    end++;
//  return std::string(reinterpret_cast<const char *>(bytes.data()), end);
//}
std::string DometicCfxBle::decode_to_string_(const std::vector<uint8_t> &bytes,
                                              const std::string &type_hint) {
  if (type_hint == "POWER_SOURCE_TEXT") {
    if (bytes.empty()) return "Unknown";
    switch (bytes[0]) {
      case 0: return "AC";
      case 1: return "DC";
      case 2: return "Solar";
      default: return "Unknown";
    }
  }

  if (type_hint == "BATTERY_PROTECTION_TEXT") {
    if (bytes.empty()) return "Unknown";
    switch (bytes[0]) {
      case 0: return "Low";
      case 1: return "Medium";
      case 2: return "High";
      default: return "Unknown";
    }
  }

  if (bytes.empty()) return "";
  size_t end = 0;
  while (end < bytes.size() && bytes[end] != 0x00)
    end++;
  return std::string(reinterpret_cast<const char *>(bytes.data()), end);
}


// ----------------- Encode ---------------------------------------------------

std::vector<uint8_t> DometicCfxBle::encode_from_bool_(bool value) {
  return {static_cast<uint8_t>(value ? 1 : 0)};
}

std::vector<uint8_t> DometicCfxBle::encode_from_float_millidegree_(float value) {
  // °C → int32 millidegrees LE
  int32_t millideg = static_cast<int32_t>(std::lround(value * 1000.0f));
  std::vector<uint8_t> out(4);
  memcpy(out.data(), &millideg, 4);
  return out;
}

// ----------------- Wrapper entity methods -----------------------------------

void DometicCfxBleSwitch::write_state(bool state) {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "Switch has no parent");
    this->publish_state(state);
    return;
  }
  this->parent_->send_switch(this->topic_, state);
  this->publish_state(state);
}

void DometicCfxBleNumber::control(float value) {
  if (this->parent_ == nullptr) {
    ESP_LOGW(TAG, "Number has no parent");
    this->publish_state(value);
    return;
  }
  this->parent_->send_number(this->topic_, value);
  this->publish_state(value);
}

// Battery protection mode select
void DometicCfxBleSelect::control(const std::string &value) {
  if (this->parent_ == nullptr) return;
  
  uint8_t val = 0;
  if (value == "Low") val = 0;
  else if (value == "Medium") val = 1;
  else if (value == "High") val = 2;

  this->parent_->send_enum(this->topic_, val);
  this->publish_state(value);
}

// ----------------- Climate --------------------------------------------------

climate::ClimateTraits DometicCfxBleClimate::traits() {
  auto traits = climate::ClimateTraits();
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(-22.0);
  traits.set_visual_max_temperature(20.0);
  traits.set_visual_temperature_step(1.0);
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_COOL,
  });
  return traits;
}

void DometicCfxBleClimate::control(const climate::ClimateCall &call) {
  if (this->parent_ == nullptr) return;

  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
    bool power = (mode != climate::CLIMATE_MODE_OFF);
    this->parent_->send_switch("COMPARTMENT_0_POWER", power);
    this->parent_->cfx_power_ = power;
    this->update_state(this->parent_->cfx_power_,
                       this->parent_->cfx_set_temp_,
                       this->parent_->cfx_measured_temp_);
  }

  if (call.get_target_temperature().has_value()) {
    float temp = *call.get_target_temperature();
    this->parent_->send_number("COMPARTMENT_0_SET_TEMPERATURE", temp);
    this->parent_->cfx_set_temp_ = temp;
    this->update_state(this->parent_->cfx_power_,
                       this->parent_->cfx_set_temp_,
                       this->parent_->cfx_measured_temp_);
  }
}

void DometicCfxBleClimate::update_state(bool power, float target_temp, float current_temp) {
  this->mode = power ? climate::CLIMATE_MODE_COOL : climate::CLIMATE_MODE_OFF;
  this->target_temperature = target_temp;
  this->current_temperature = current_temp;
  this->publish_state();
}

}  // namespace dometic_cfx_ble
}  // namespace esphome
