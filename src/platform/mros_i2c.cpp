#include "src/platform/mros_i2c.h"

#include <array>
#include <cstring>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mros::platform {

namespace {

constexpr int kAddressCount = 128;

I2cBusConfig g_config = {};
i2c_master_bus_handle_t g_bus = nullptr;
std::array<i2c_master_dev_handle_t, kAddressCount> g_devices = {};
SemaphoreHandle_t g_mutex = nullptr;
bool g_ready = false;
bool g_debug_scan_enabled = false;

bool ensure_mutex() {
  if (g_mutex != nullptr) {
    return true;
  }
  g_mutex = xSemaphoreCreateMutex();
  return g_mutex != nullptr;
}

bool lock_bus(const TickType_t timeout = pdMS_TO_TICKS(50)) {
  return ensure_mutex() && xSemaphoreTake(g_mutex, timeout) == pdTRUE;
}

void unlock_bus() {
  if (g_mutex != nullptr) {
    xSemaphoreGive(g_mutex);
  }
}

int resolve_timeout_ms(const int timeout_ms) {
  return timeout_ms >= 0 ? timeout_ms : g_config.timeout_ms;
}

void clear_device_handles() { g_devices.fill(nullptr); }

void remove_all_devices() {
  for (i2c_master_dev_handle_t& handle : g_devices) {
    if (handle != nullptr) {
      (void)i2c_master_bus_rm_device(handle);
      handle = nullptr;
    }
  }
}

bool same_config(const I2cBusConfig& lhs, const I2cBusConfig& rhs) {
  return lhs.port == rhs.port && lhs.sda_pin == rhs.sda_pin &&
         lhs.scl_pin == rhs.scl_pin && lhs.freq_hz == rhs.freq_hz &&
         lhs.glitch_ignore_count == rhs.glitch_ignore_count &&
         lhs.timeout_ms == rhs.timeout_ms &&
         lhs.write_chunk_size == rhs.write_chunk_size &&
         lhs.enable_internal_pullups == rhs.enable_internal_pullups;
}

i2c_master_dev_handle_t ensure_device_handle(const uint8_t address) {
  if (!g_ready || address >= g_devices.size()) {
    return nullptr;
  }
  i2c_master_dev_handle_t& handle = g_devices[address];
  if (handle != nullptr) {
    return handle;
  }

  i2c_device_config_t device_config = {};
  device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  device_config.device_address = address;
  device_config.scl_speed_hz = g_config.freq_hz;
  if (i2c_master_bus_add_device(g_bus, &device_config, &handle) != ESP_OK) {
    handle = nullptr;
    return nullptr;
  }
  return handle;
}

}  // namespace

bool mros_i2c_init(const I2cBusConfig& config) {
  if (!lock_bus()) {
    return false;
  }

  if (g_ready && same_config(g_config, config)) {
    unlock_bus();
    return true;
  }

  if (g_ready) {
    remove_all_devices();
    (void)i2c_del_master_bus(g_bus);
    g_bus = nullptr;
    g_ready = false;
  } else {
    clear_device_handles();
  }

  i2c_master_bus_config_t bus_config = {};
  bus_config.i2c_port = config.port;
  bus_config.sda_io_num = static_cast<gpio_num_t>(config.sda_pin);
  bus_config.scl_io_num = static_cast<gpio_num_t>(config.scl_pin);
  bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_config.glitch_ignore_cnt = config.glitch_ignore_count;
  bus_config.flags.enable_internal_pullup = config.enable_internal_pullups;

  esp_err_t err = i2c_new_master_bus(&bus_config, &g_bus);
  if (err == ESP_OK) {
    g_config = config;
    g_ready = true;
    clear_device_handles();
  }
  unlock_bus();
  return err == ESP_OK;
}

void mros_i2c_deinit() {
  if (!lock_bus()) {
    return;
  }
  if (g_ready) {
    remove_all_devices();
    (void)i2c_del_master_bus(g_bus);
    g_bus = nullptr;
    g_ready = false;
  }
  unlock_bus();
}

bool mros_i2c_is_ready() { return g_ready; }

const I2cBusConfig& mros_i2c_config() { return g_config; }

esp_err_t mros_i2c_probe(const uint8_t address, const int timeout_ms) {
  if (!lock_bus()) {
    return ESP_ERR_TIMEOUT;
  }
  const esp_err_t err =
      g_ready ? i2c_master_probe(g_bus, address, resolve_timeout_ms(timeout_ms))
              : ESP_ERR_INVALID_STATE;
  unlock_bus();
  return err;
}

esp_err_t mros_i2c_transmit(const uint8_t address, const uint8_t* data,
                            const size_t size, const int timeout_ms) {
  if (data == nullptr || size == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock_bus()) {
    return ESP_ERR_TIMEOUT;
  }
  i2c_master_dev_handle_t handle = ensure_device_handle(address);
  const esp_err_t err =
      (handle != nullptr)
          ? i2c_master_transmit(handle, data, size, resolve_timeout_ms(timeout_ms))
          : ESP_ERR_INVALID_STATE;
  unlock_bus();
  return err;
}

esp_err_t mros_i2c_transmit_receive(const uint8_t address,
                                    const uint8_t* write_data,
                                    const size_t write_size,
                                    uint8_t* read_data,
                                    const size_t read_size,
                                    const int timeout_ms) {
  if (write_data == nullptr || write_size == 0U || read_data == nullptr ||
      read_size == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock_bus()) {
    return ESP_ERR_TIMEOUT;
  }
  i2c_master_dev_handle_t handle = ensure_device_handle(address);
  const esp_err_t err =
      (handle != nullptr)
          ? i2c_master_transmit_receive(handle, write_data, write_size, read_data,
                                        read_size, resolve_timeout_ms(timeout_ms))
          : ESP_ERR_INVALID_STATE;
  unlock_bus();
  return err;
}

esp_err_t mros_i2c_write_reg_u8(const uint8_t address, const uint8_t reg,
                                const uint8_t value, const int timeout_ms) {
  const uint8_t payload[2] = {reg, value};
  return mros_i2c_transmit(address, payload, sizeof(payload), timeout_ms);
}

esp_err_t mros_i2c_read_reg_u8(const uint8_t address, const uint8_t reg,
                               uint8_t* value, const int timeout_ms) {
  if (value == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t out = 0U;
  const esp_err_t err =
      mros_i2c_transmit_receive(address, &reg, 1U, &out, 1U, timeout_ms);
  if (err == ESP_OK) {
    *value = out;
  }
  return err;
}

void mros_i2c_set_debug_scan_enabled(const bool enabled) {
  g_debug_scan_enabled = enabled;
}

bool mros_i2c_debug_scan_enabled() { return g_debug_scan_enabled; }

uint8_t mros_i2c_legacy_error_code(const esp_err_t err) {
  switch (err) {
    case ESP_OK:
      return 0U;
    case ESP_ERR_INVALID_SIZE:
      return 1U;
    case ESP_ERR_NOT_FOUND:
      return 2U;
    case ESP_ERR_INVALID_RESPONSE:
      return 3U;
    case ESP_ERR_TIMEOUT:
      return 5U;
    default:
      return 4U;
  }
}

}  // namespace mros::platform
