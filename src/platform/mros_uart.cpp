#include "src/platform/mros_uart.h"

#include <array>
#include <cstring>

namespace mros::platform {

namespace {

constexpr size_t kMaxPorts = static_cast<size_t>(UART_NUM_MAX);

struct UartState {
  bool ready = false;
  UartConfig config = {};
  QueueHandle_t event_queue = nullptr;
};

std::array<UartState, kMaxPorts> g_uart = {};

bool same_config(const UartConfig& lhs, const UartConfig& rhs) {
  return lhs.port == rhs.port && lhs.tx_pin == rhs.tx_pin &&
         lhs.rx_pin == rhs.rx_pin && lhs.rts_pin == rhs.rts_pin &&
         lhs.cts_pin == rhs.cts_pin && lhs.baud_rate == rhs.baud_rate &&
         lhs.data_bits == rhs.data_bits && lhs.parity == rhs.parity &&
         lhs.stop_bits == rhs.stop_bits && lhs.flow_control == rhs.flow_control &&
         lhs.source_clock == rhs.source_clock &&
         lhs.rx_buffer_size == rhs.rx_buffer_size &&
         lhs.tx_buffer_size == rhs.tx_buffer_size &&
         lhs.queue_size == rhs.queue_size && lhs.intr_flags == rhs.intr_flags;
}

bool valid_port(const uart_port_t port) {
  return port >= UART_NUM_0 && static_cast<size_t>(port) < g_uart.size();
}

UartState* state_for(const uart_port_t port) {
  return valid_port(port) ? &g_uart[static_cast<size_t>(port)] : nullptr;
}

}  // namespace

bool mros_uart_init(const UartConfig& config, QueueHandle_t* out_event_queue) {
  UartState* state = state_for(config.port);
  if (state == nullptr) {
    return false;
  }
  if (!state->ready && uart_is_driver_installed(config.port)) {
    state->ready = true;
    state->config = config;
    state->event_queue = nullptr;
    if (out_event_queue != nullptr) {
      *out_event_queue = nullptr;
    }
    return true;
  }
  if (state->ready && same_config(state->config, config)) {
    if (out_event_queue != nullptr) {
      *out_event_queue = state->event_queue;
    }
    return true;
  }

  if (state->ready) {
    (void)uart_driver_delete(config.port);
    state->ready = false;
    state->event_queue = nullptr;
  }

  uart_config_t uart_config = {};
  uart_config.baud_rate = config.baud_rate;
  uart_config.data_bits = config.data_bits;
  uart_config.parity = config.parity;
  uart_config.stop_bits = config.stop_bits;
  uart_config.flow_ctrl = config.flow_control;
  uart_config.rx_flow_ctrl_thresh = 0;
  uart_config.source_clk = config.source_clock;

  if (uart_param_config(config.port, &uart_config) != ESP_OK) {
    return false;
  }
  if (uart_set_pin(config.port, config.tx_pin, config.rx_pin, config.rts_pin,
                   config.cts_pin) != ESP_OK) {
    return false;
  }
  QueueHandle_t event_queue = nullptr;
  QueueHandle_t* event_queue_out = config.queue_size > 0 ? &event_queue : nullptr;
  if (uart_driver_install(config.port, config.rx_buffer_size, config.tx_buffer_size,
                          config.queue_size, event_queue_out, config.intr_flags) != ESP_OK) {
    return false;
  }

  state->ready = true;
  state->config = config;
  state->event_queue = event_queue;
  if (out_event_queue != nullptr) {
    *out_event_queue = event_queue;
  }
  return true;
}

bool mros_uart_is_ready(const uart_port_t port) {
  const UartState* state = state_for(port);
  return state != nullptr && state->ready;
}

QueueHandle_t mros_uart_event_queue(const uart_port_t port) {
  const UartState* state = state_for(port);
  return state != nullptr ? state->event_queue : nullptr;
}

int mros_uart_writable(const uart_port_t port) {
  if (!mros_uart_is_ready(port)) {
    return 0;
  }
  size_t free_size = 0U;
  if (uart_get_tx_buffer_free_size(port, &free_size) != ESP_OK) {
    return 0;
  }
  return static_cast<int>(free_size);
}

int mros_uart_available(const uart_port_t port) {
  if (!mros_uart_is_ready(port)) {
    return 0;
  }
  size_t available = 0U;
  if (uart_get_buffered_data_len(port, &available) != ESP_OK) {
    return 0;
  }
  return static_cast<int>(available);
}

int mros_uart_read(const uart_port_t port, void* data, const size_t size,
                   const TickType_t timeout) {
  if (!mros_uart_is_ready(port) || data == nullptr || size == 0U) {
    return 0;
  }
  return uart_read_bytes(port, data, static_cast<uint32_t>(size), timeout);
}

bool mros_uart_read_byte(const uart_port_t port, uint8_t* value,
                         const TickType_t timeout) {
  if (value == nullptr) {
    return false;
  }
  return mros_uart_read(port, value, 1U, timeout) == 1;
}

int mros_uart_write(const uart_port_t port, const void* data, const size_t size) {
  if (!mros_uart_is_ready(port) || data == nullptr || size == 0U) {
    return 0;
  }
  return uart_write_bytes(port, data, size);
}

int mros_uart_write_line(const uart_port_t port, const char* text) {
  if (text == nullptr) {
    return 0;
  }
  int written = mros_uart_write(port, text, std::strlen(text));
  written += mros_uart_write(port, "\n", 1U);
  return written;
}

void mros_uart_flush_input(const uart_port_t port) {
  if (mros_uart_is_ready(port)) {
    (void)uart_flush_input(port);
  }
}

}  // namespace mros::platform
