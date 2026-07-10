#include "src/platform/mros_gpio.h"

#include <driver/gpio.h>

namespace mros::platform {

namespace {

bool valid_pin(const int pin) {
  return pin >= 0 && pin < GPIO_NUM_MAX;
}

}  // namespace

bool mros_gpio_config(const int pin, const GpioMode mode) {
  if (!valid_pin(pin)) {
    return false;
  }

  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << static_cast<unsigned>(pin);
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;

  switch (mode) {
    case GpioMode::Input:
      config.mode = GPIO_MODE_INPUT;
      config.pull_up_en = GPIO_PULLUP_DISABLE;
      break;
    case GpioMode::InputPullup:
      config.mode = GPIO_MODE_INPUT;
      config.pull_up_en = GPIO_PULLUP_ENABLE;
      break;
    case GpioMode::Output:
      config.mode = GPIO_MODE_OUTPUT;
      config.pull_up_en = GPIO_PULLUP_DISABLE;
      break;
    default:
      return false;
  }

  return gpio_config(&config) == ESP_OK;
}

bool mros_gpio_write(const int pin, const bool level) {
  if (!valid_pin(pin)) {
    return false;
  }
  return gpio_set_level(static_cast<gpio_num_t>(pin), level ? 1 : 0) == ESP_OK;
}

int mros_gpio_read(const int pin) {
  if (!valid_pin(pin)) {
    return 0;
  }
  return gpio_get_level(static_cast<gpio_num_t>(pin));
}

}  // namespace mros::platform
