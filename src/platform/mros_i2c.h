#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_err.h>

namespace mros::platform {

struct I2cBusConfig {
  i2c_port_num_t port = I2C_NUM_0;
  int sda_pin = -1;
  int scl_pin = -1;
  uint32_t freq_hz = 100000UL;
  uint32_t glitch_ignore_count = 7U;
  int timeout_ms = 20;
  uint8_t write_chunk_size = 4U;
  bool enable_internal_pullups = true;
};

bool mros_i2c_init(const I2cBusConfig& config);
void mros_i2c_deinit();
bool mros_i2c_is_ready();
const I2cBusConfig& mros_i2c_config();

esp_err_t mros_i2c_probe(uint8_t address, int timeout_ms = -1);
esp_err_t mros_i2c_transmit(uint8_t address, const uint8_t* data, size_t size,
                            int timeout_ms = -1);
esp_err_t mros_i2c_transmit_receive(uint8_t address, const uint8_t* write_data,
                                    size_t write_size, uint8_t* read_data,
                                    size_t read_size, int timeout_ms = -1);
esp_err_t mros_i2c_write_reg_u8(uint8_t address, uint8_t reg, uint8_t value,
                                int timeout_ms = -1);
esp_err_t mros_i2c_read_reg_u8(uint8_t address, uint8_t reg, uint8_t* value,
                               int timeout_ms = -1);

void mros_i2c_set_debug_scan_enabled(bool enabled);
bool mros_i2c_debug_scan_enabled();
uint8_t mros_i2c_legacy_error_code(esp_err_t err);

}  // namespace mros::platform
