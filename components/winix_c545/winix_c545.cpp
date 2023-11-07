
#include "winix_c545.h"

#include <map>

#include "esphome/core/log.h"

namespace esphome {
namespace winix_c545 {

static const char *const TAG = "winix_c545";

void WinixC545Component::write_sentence_(const std::string &sentence) {
  ESP_LOGD(TAG, "Sending sentence: %s%s", TX_PREFIX.c_str(), sentence.c_str());

  // Send over UART
  this->write_str(TX_PREFIX.c_str());
  this->write_str(sentence.c_str());
  this->write_str("\r\n");
}

void WinixC545Component::write_state(const WinixStateMap &states) {
  constexpr uint32_t BUFFER_SIZE = 16;

  // Nothing to do if empty
  if (states.empty())
    return;

  std::string sentence = "AWS_RECV:A211 12 {";

  // Reserve storage for each possible state
  sentence.reserve(states.size() * BUFFER_SIZE);

  for (const auto &state : states) {
    const std::string &key = state.first;
    const uint16_t value = state.second;

    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "\"%s\":\"%d\",", key.c_str(), value);

    sentence.append(buffer);
  }

  // Replace final comma with an end brace
  sentence.at(sentence.size() - 1) = '}';

  // Write sentence to device
  this->write_sentence_(sentence);
}

void WinixC545Component::publish_state_() {
  if (this->states_.empty())
    return;

  for (const auto &state : this->states_) {
    const std::string &key = state.first;
    const uint16_t value = state.second;

    // Handle sensor states and other non-fan states
    if (key == KEY_AQI_INDICATOR && this->aqi_indicator_text_sensor_ != nullptr) {
      // AQI LED indicator
      switch (value) {
        case 1:
          this->aqi_indicator_text_sensor_->publish_state("Good");
          break;
        case 2:
          this->aqi_indicator_text_sensor_->publish_state("Fair");
          break;
        case 3:
          this->aqi_indicator_text_sensor_->publish_state("Poor");
          break;
      }
    } else if (key == KEY_AQI && this->aqi_sensor_ != nullptr) {
      // AQI
      if (value != this->aqi_sensor_->raw_state)
        this->aqi_sensor_->publish_state(value);
    } else if (key == KEY_LIGHT && this->light_sensor_ != nullptr) {
      // Light
      if (value != this->light_sensor_->raw_state)
        this->light_sensor_->publish_state(value);
    } else if (key == KEY_FILTER_AGE && this->filter_age_sensor_ != nullptr) {
      // Filter age
      if (value != this->filter_age_sensor_->raw_state)
        this->filter_age_sensor_->publish_state(value);
    } else if (key == KEY_PLASMAWAVE && this->plasmawave_switch_ != nullptr) {
      // Plasmawave
      bool state = value == 1;
      if (state != this->plasmawave_switch_->state)
        this->plasmawave_switch_->publish_state(state);
    } else if (key == KEY_AUTO && this->auto_switch_ != nullptr) {
      // Auto
      bool state = value == 1;
      if (state != this->auto_switch_->state)
        this->auto_switch_->publish_state(state);
    } else if (key == KEY_SPEED && this->sleep_switch_ != nullptr) {
      // Sleep is a speed value
      bool state = value == 6;
      if (state != this->sleep_switch_->state)
        this->sleep_switch_->publish_state(state);
    }
  }

  // Pass states to underlying fan if it exists
  if (this->fan_ != nullptr) {
    this->fan_->update_state(this->states_);
  }

  // All states published, clear contents
  this->states_.clear();
}

void WinixC545Component::parse_aws_sentence_(char *sentence) {
  uint16_t api_code = 0;
  if (sscanf(sentence, "AWS_SEND=A%3d", &api_code) != 1) {
    ESP_LOGE(TAG, "Failed to extract API code from sentence: %s", sentence);
    return;
  }

  bool valid = false;
  switch (api_code) {
    case 102:  // Wifi disconnect
    {
      // Acknowledge the message
      this->write_sentence_("AWS_SEND:OK");
      this->write_sentence_("AWS_IND:SEND OK");
      this->write_sentence_("AWS_IND:DISCONNECTED");

      // Reset handshake state
      this->handshake_state_ = HandshakeState::Reset;
      this->last_handshake_event_ = millis();
      return;
    }

    case 210:  // Overall device state
    case 220:  // Sensor update
    {
      ESP_LOGI(TAG, "State update: %s", sentence);

      // Advance sentence to first token
      sentence += strlen("AWS_SEND=A2XX {");

      // Parse each token into a KV pair
      char *token = strtok(sentence, ",");
      while (token != NULL) {
        char key[4] = {0};
        uint32_t value = 0;
        if (sscanf(token, "\"%3s\":\"%d\"", key, &value) != 2) {
          ESP_LOGE(TAG, "Failed to extract from token: %s", token);
          return;
        }

        // Add token to map
        this->states_[std::string(key)] = value;

        token = strtok(NULL, ",");
      }

      valid = true;
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
    this->write_sentence_("AWS_SEND:OK");
    this->write_sentence_("AWS_IND:SEND OK");

    // If a valid packet was received, force connected state
    this->handshake_state_ = HandshakeState::Connected;
    this->last_handshake_event_ = millis();
  }
}

void WinixC545Component::parse_sentence_(char *sentence) {
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

  // Parse AWS sentences from MCU
  if (strncmp(sentence, "AWS_SEND", strlen("AWS_SEND")) == 0) {
    this->parse_aws_sentence_(sentence);
    return;
  }

  // Handle MCU_READY message
  if (strncmp(sentence, "MCU_READY", strlen("MCU_READY")) == 0) {
    this->handshake_state_ = HandshakeState::McuReady;
    this->last_handshake_event_ = millis();

    ESP_LOGI(TAG, "MCU_READY");
    this->write_sentence_("MCU_READY:OK");
    return;
  }

  // Handle MIB=32 message
  if (strncmp(sentence, "MIB=32", strlen("MIB=32")) == 0) {
    this->handshake_state_ = HandshakeState::MIB;
    this->last_handshake_event_ = millis();

    ESP_LOGI(TAG, "MIB:OK");
    this->write_sentence_("MIB:OK 7595");  // 7595 is version of OEM wifi module
    return;
  }

  // Handle SETMIB messages
  if (strncmp(sentence, "SETMIB", strlen("SETMIB")) == 0) {
    ESP_LOGI(TAG, "SETMIB:OK");
    this->write_sentence_("SETMIB:OK");
    return;
  }

  // Handle SMODE messages
  if (strncmp(sentence, "SMODE", strlen("SMODE")) == 0) {
    ESP_LOGI(TAG, "SMODE:OK");
    this->write_sentence_("SMODE:OK");
    return;
  }

  ESP_LOGW(TAG, "Unsupported sentence: %s", sentence);
}

bool WinixC545Component::readline_(char data, char *buffer, int max_length) {
  static int position = 0;

  // Read failed
  if (data < 0) return false;

  switch (data) {
    case '\n':  // Ignore new-lines
      break;

    case '\r': {             // Return on CR
      buffer[position] = 0;  // Ensure buffer is null terminated
      position = 0;          // Reset position for next line
      return true;
    }

    default:
      if (position < max_length - 1)
        buffer[position++] = data;

      break;
  }

  // No end of line has been found
  return false;
}

void WinixC545Component::update_handshake_state_() {
  switch (this->handshake_state_) {
    case HandshakeState::Connected:
      // Protocol is connected, all good
      return;

    case HandshakeState::Reset:
    case HandshakeState::DeviceReady: {
      // If there was activity recently assume the handshake is in progress
      if ((millis() - this->last_handshake_event_) < 10000)
        return;

      // Indicate device is ready to start handshake with MCU
      this->handshake_state_ = HandshakeState::DeviceReady;
      this->last_handshake_event_ = millis();

      ESP_LOGI(TAG, "DEVICEREADY");
      this->write_sentence_("DEVICEREADY");
      break;
    }

    case HandshakeState::MIB: {
      this->handshake_state_ = HandshakeState::Connected;
      this->last_handshake_event_ = millis();

      // Some subset of these may be needed
      // *ICT*ASSOCIATED:0
      // *ICT*IPALLOCATED:10.100.1.250 255.255.255.0 10.100.1.1 10.100.1.6
      // *ICT*AWS_IND:MQTT OK
      // *ICT*AWS_IND:SUBSCRIBE OK
      // *ICT*AWS_IND:CONNECT OK
      ESP_LOGI(TAG, "CONNECTED");
      this->write_sentence_("AWS_IND:CONNECT OK");
      break;
    }
  }
}

void WinixC545Component::loop() {
  static char buffer[MAX_LINE_LENGTH];

  // Handle protocol handshake state
  this->update_handshake_state_();

  // Publish states as needed
  this->publish_state_();

  if (this->available() < RX_PREFIX.size()) return;

  while (this->available() > 0) {
    char data = this->read();
    bool found = this->readline_(data, buffer, MAX_LINE_LENGTH);
    if (!found) continue;

    // Line received, parse it
    this->parse_sentence_(buffer);
    return;
  }
}

void WinixC545Component::dump_config() {
  ESP_LOGCONFIG(TAG, "Winix C545:");

#ifdef USE_FAN
  if (this->fan_) this->fan_->dump_config();
#endif

#ifdef USE_SENSOR
  LOG_SENSOR("  ", "Filter Age Sensor", this->filter_age_sensor_);
  LOG_SENSOR("  ", "AQI Sensor", this->aqi_sensor_);
  LOG_SENSOR("  ", "Light Sensor", this->light_sensor_);
#endif

#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "AQI Indicator Text Sensor", this->aqi_indicator_text_sensor_);
#endif

#ifdef USE_SWITCH
  LOG_SWITCH("  ", "Plasmawave Switch", this->plasmawave_switch_);
  LOG_SWITCH("  ", "Auto Switch", this->auto_switch_);
  LOG_SWITCH("  ", "Sleep Switch", this->sleep_switch_);
#endif
}

void WinixC545Component::setup() {
  // Restore state
  // auto restore = this->restore_state_();
  // if (restore.has_value()) {
  //   restore->apply(*this);
  //   this->write_state_();
  // }

  // Reset handshake
  this->handshake_state_ = HandshakeState::Reset;
  this->last_handshake_event_ = millis();
}

void WinixC545Fan::dump_config() {
#ifdef USE_FAN
  LOG_FAN("  ", "Fan", this);
#endif
}

void WinixC545Fan::update_state(const WinixStateMap &states) {
  // Nothing to do if empty
  if (states.empty())
    return;

  bool publish = false;
  for (const auto &state : states) {
    const std::string &key = state.first;
    const uint16_t value = state.second;

    // Handle fan states
    if (key == KEY_POWER) {
      // Power on/off
      bool state = value == 1 ? true : false;
      if (state != this->state) {
        // State has changed, publish
        this->state = state;
        publish = true;
      }
    } else if (key == KEY_SPEED) {
      uint8_t speed = 0;
      // Speed
      if (value == 5)  // Turbo
        speed = 4;
      else if (value == 6)  // Sleep
        speed = 0; // TODO?
      else
        speed = value;

      if (speed != this->speed) {
        // Speed has changed, publish
        this->speed = speed;
        publish = true;
      }
    }
  }

  if (publish)
    this->publish_state();
}

void WinixC545Fan::control(const fan::FanCall &call) {
  WinixStateMap states;

  if (call.get_state().has_value() && this->state != *call.get_state()) {
    // State has changed
    this->state = *call.get_state();
    states.emplace(KEY_POWER, state ? 1 : 0);
  }

  if (call.get_speed().has_value() && this->speed != *call.get_speed()) {
    // Speed has changed
    this->speed = *call.get_speed();
    states.emplace(KEY_SPEED, this->speed == 4 ? 5 : this->speed);
  }

  this->parent_->write_state(states);
  this->publish_state();
}

void WinixC545Switch::write_state(bool state) {
  WinixStateMap states;

  if (state != this->state) {
    // State has changed
    states.emplace(this->key_, state ? this->on_value_ : this->off_value_);
  }

  this->parent_->write_state(states);
  this->publish_state(state);
}

}  // namespace winix_c545
}  // namespace esphome