#pragma once

#include <cstdint>

namespace mros::platform {

uint64_t mros_micros();
uint32_t mros_millis();
void mros_delay_ms(uint32_t delay_ms);

}  // namespace mros::platform
