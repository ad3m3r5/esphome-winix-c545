#pragma once

#include <string>

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

#ifdef USE_FAN
#include "esphome/components/fan/fan.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome {
namespace winix_c545 {

class WinixC545Fan : public fan::Fan {
  fan::FanTraits get_traits() override;

 protected:
  // Fan control
  void control(const fan::FanCall &call) override;
  void write_state_();
};

class WinixC545Component : public uart::UARTDevice, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

#ifdef USE_FAN
  void set_fan(fan::Fan *fan) { this->fan_ = fan; };
#endif

#ifdef USE_SENSOR
  // Functions for configuration
  void set_aqi_sensor(sensor::Sensor *sensor) { this->aqi_sensor_ = sensor; }
  void set_aqi_stoplight_sensor(sensor::Sensor *sensor);
  void set_light_sensor(sensor::Sensor *sensor);
  void set_filter_age_sensor(sensor::Sensor *sensor);
#endif

#ifdef USE_SWITCH
  void set_plasmawave_switch(switch_::Switch *switch);
  void set_auto_switch(switch_::Switch *switch);
  void set_sleep_switch(switch_::Switch *switch);
#endif

 protected:
  static constexpr uint32_t MAX_LINE_LENGTH = 255;
  bool readline_(char, char *, int);

#ifdef USE_FAN
  fan::Fan *fan_{nullptr};
#endif

#ifdef USE_SENSOR
  sensor::Sensor *aqi_sensor_{nullptr};
  sensor::Sensor *aqi_stoplight_sensor_{nullptr};

  sensor::Sensor *light_sensor_{nullptr};

  sensor::Sensor *filter_age_sensor_{nullptr};
#endif

#ifdef USE_SWITCH
  switch_::Switch *plasmawave_switch_ = {nullptr};

  // TODO the following belong as presets, not switches
  switch_::Switch *auto_switch_{nullptr};
  switch_::Switch *sleep_switch_{nullptr};
#endif
};

}  // namespace winix_c545
}  // namespace esphome