
#include "winix_c545.h"

#include <map>

#include "esphome/core/log.h"

namespace esphome {
namespace winix_c545 {

static const char *const TAG = "winix_c545";

void WinixC545Component::write_sentence(const std::string &sentence) {
  // Build complete command sentence
  std::string tx_data = TX_PREFIX + sentence + "\r\n";

  // Send over UART
  ESP_LOGD(TAG, "Sending sentence: %s", tx_data.c_str());
  this->write_str(tx_data.c_str());
}

bool WinixC545Component::readline_(char data, char *buffer, int max_length) {
  static int position = 0;

  // Read failed
  if (data < 0) return false;

  switch (data) {
    case '\n':  // Ignore new-lines
      break;

    case '\r': {  // Return on CR
      int end = position;
      position = 0;  // Reset position index ready for next time
      return end;
    }

    default:
      if (position < max_length - 1) {
        buffer[position++] = data;
        buffer[position] = 0;
      }
      break;
  }

  // No end of line has been found
  return false;
}

void WinixC545Component::update_state_(const WinixStateMap &states) {
  for (const auto &state : states) {
    const std::string &key = state.first;
    const uint16_t value = state.second;

    // Handle sensor states and other non-fan states
    if (key == "S07" && this->aqi_stoplight_sensor_ != nullptr) {
      // AQI stoplight
      this->aqi_stoplight_sensor_->publish_state(value);
    } else if (key == "S08" && this->aqi_sensor_ != nullptr) {
      // AQI
      this->aqi_sensor_->publish_state(value);
    } else if (key == "S14" && this->light_sensor_ != nullptr) {
      // Light
      this->light_sensor_->publish_state(value);
    } else if (key == "A21" && this->filter_age_sensor_ != nullptr) {
      // Filter age
      this->filter_age_sensor_->publish_state(value);
    }
  }

  // Pass states to underlying fan if it exists
  if (this->fan_ != nullptr) {
    this->fan_->update_state(states);
  }
}

void WinixC545Component::parse_aws_sentence_(const char *sentence) {
  uint16_t api_code = 0;
  if (sscanf(sentence, "AWS_SEND=A%3d", &api_code) != 1) {
    ESP_LOGE(TAG, "Failed to extract API code from sentence: %s", sentence);
    return;
  }

  bool valid = false;
  switch (api_code) {
    case 210:  // Overall device state
    case 220:  // Sensor update
    {
      ESP_LOGI(TAG, "State update: %s", sentence);

      // Create a modifiable copy of the message payload for tokenization
      char payload[MAX_LINE_LENGTH] = {0};
      strncpy(payload, sentence + strlen("AWS_SEND=A2XX {"), MAX_LINE_LENGTH);

      // Construct map to hold updates
      WinixStateMap states;

      // Parse each token into a KV pair
      char *token = strtok(payload, ",");
      while (token != NULL) {
        char key[4] = {0};
        uint32_t value = 0;
        if (sscanf(token, "\"%3s\":\"%d\"", key, &value) != 2) {
          ESP_LOGE(TAG, "Failed to extract from token: %s", token);
          return;
        }

        // Add token to map
        states.emplace(std::string(key), value);

        token = strtok(NULL, ",");
      }

      // Update internal state
      this->update_state_(states);

      break;
    }

    case 230:  // Error code
    case 240:  // Version information
    {
      ESP_LOGI(TAG, "Misc update: %s", sentence);
      valid = true;
      break;
    }

    default:
      ESP_LOGW(TAG, "Unknown API code %d: %s", api_code, sentence);
      break;
  }

  if (valid) {
    // Acknowledge the message
    this->write_sentence("AWS_SEND:OK");
    this->write_sentence("AWS_IND:SEND OK");
  }
}

void WinixC545Component::parse_sentence_(const char *sentence) {
  ESP_LOGD(TAG, "Received sentence: %s", sentence);

  // Example sentence formats
  // AT*ICT*MCU_READY=1.2.0
  // AT*ICT*MIB=32
  // AT*ICT*SETMIB=18 C545
  // AT*ICT*AWS_SEND=A210 {"A02":"1","A03":"02","A04":"02","A05":"01","A07":"1","A21":"3706","S07":"01","S08":"97","S14":"34"}
  // AT*ICT*AWS_SEND=A220 {"S07":"01","S08":"116","S14":"34"}

  // Ensure sentence starts as expected
  if (strncmp(sentence, RX_PREFIX.c_str(), RX_PREFIX.size()) != 0) {
    ESP_LOGW(TAG, "Received invalid sentence: %s", sentence);
    return;
  }

  // Advance past prefix
  sentence += RX_PREFIX.size();

  // Handle MCU_READY message
  if (strncmp(sentence, "MCU_READY", strlen("MCU_READY")) == 0) {
    ESP_LOGI(TAG, "MCU_READY");
    this->write_sentence("MCU_READY:OK");
    return;
  }

  // Handle MIB=32 message
  if (strncmp(sentence, "MIB=32", strlen("MIB=32")) == 0) {
    ESP_LOGI(TAG, "MIB:OK");
    // 7595 is version of OEM wifi module
    this->write_sentence("MIB:OK 7595");
    return;
  }

  // Handle SETMIB messages
  if (strncmp(sentence, "SETMIB", strlen("SETMIB")) == 0) {
    ESP_LOGI(TAG, "SETMIB:OK");
    this->write_sentence("SETMIB:OK");
    return;
  }

  // Handle SMODE messages
  if (strncmp(sentence, "SMODE", strlen("SMODE")) == 0) {
    ESP_LOGI(TAG, "SMODE:OK");
    this->write_sentence("SMODE:OK");
    return;
  }

  // Parse AWS sentences from MCU
  if (strncmp(sentence, "AWS_SEND", strlen("AWS_SEND")) == 0)
    this->parse_aws_sentence_(sentence);
  else
    ESP_LOGW(TAG, "Unsupported sentence: %s", sentence);
}

void WinixC545Component::loop() {
  static char buffer[MAX_LINE_LENGTH];

  // TODO check available() against a min size?
  if (!this->available()) return;

  while (this->available()) {
    char data = this->read();
    bool found = this->readline_(data, buffer, MAX_LINE_LENGTH);
    if (!found) continue;

    // Line received, parse it
    this->parse_sentence_(buffer);
  }
}

void WinixC545Component::dump_config() {
  // TODO
}

// void WinixC545Component::on_plasmawave_state_(bool state) {
//   // TODO Send uart command and update fan state if necessary
// }

void WinixC545Component::setup() {
  // Restore state
  // auto restore = this->restore_state_();
  // if (restore.has_value()) {
  //   restore->apply(*this);
  //   this->write_state_();
  // }

  // TODO need to create switches first?? Switches
  // this->plasmawave_switch_.add_on_state_callback(this->on_plasmawave_state_);

  // Indicate device is ready
  // TODO base on wifi state?
  this->write_sentence("DEVICEREADY");
  // Some subset of these may be needed too
  // *ICT*ASSOCIATED:0
  // *ICT*IPALLOCATED:10.100.1.250 255.255.255.0 10.100.1.1 10.100.1.6
  // *ICT*AWS_IND:MQTT OK
  // *ICT*AWS_IND:SUBSCRIBE OK
  // *ICT*AWS_IND:CONNECT OK
}

void WinixC545Fan::update_state(const WinixStateMap &states) {
  // Nothing to do if empty
  if (states.empty())
    return;

  for (const auto &state : states) {
    const std::string &key = state.first;
    const uint16_t value = state.second;

    // Handle fan states
    if (key == "A02") {
      // Power on/off
      this->state = value == 1 ? true : false;
    } else if (key == "A03") {
      // Auto
      // TODO Not in fan anyway?
    } else if (key == "A04") {
      // Speed
      if (value == 5)  // Turbo
        this->speed = 4;
      else if (value == 6)  // Sleep
        this->speed = 0;    // TODO?
      else
        this->speed = value;
    } else if (key == "A07") {
      // Plasmawave
      // TODO not in fan
    }
  }

  this->publish_state();
}

void WinixC545Fan::write_aws_sentence_(const WinixStateMap &states) {
  // Nothing to do if empty
  if (states.empty())
    return;

  std::string sentence = "AWS_RECV:A211 12 {";

  for (const auto &state : states) {
    const std::string &key = state.first;
    const uint16_t value = state.second;

    char buffer[32] = {0};
    snprintf(buffer, sizeof(buffer), "\"%s\":\"%d\",", key.c_str(), value);

    sentence += std::string(buffer);
  }

  // Remove final comma and insert end brace
  sentence.pop_back();
  sentence += "}";

  // Write sentence to device
  this->parent_->write_sentence(sentence);
}

void WinixC545Fan::control(const fan::FanCall &call) {
  std::map<const std::string, uint16_t> states;

  if (call.get_state().has_value() && this->state != *call.get_state()) {
    // State has changed
    this->state = *call.get_state();
    states.emplace("A02", state ? 1 : 0);
  }

  if (call.get_speed().has_value() && this->speed != *call.get_speed()) {
    // Speed has changed
    this->speed = *call.get_speed();
    states.emplace("A04", this->speed == 4 ? 5 : this->speed);
  }

  this->write_aws_sentence_(states);
  this->publish_state();
}

}  // namespace winix_c545
}  // namespace esphome