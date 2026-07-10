#pragma once

namespace mros::platform {

enum class GpioMode {
  Input = 0,
  InputPullup,
  Output,
};

bool mros_gpio_config(int pin, GpioMode mode);
bool mros_gpio_write(int pin, bool level);
int mros_gpio_read(int pin);

}  // namespace mros::platform
