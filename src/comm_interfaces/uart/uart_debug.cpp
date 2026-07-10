#include "uart_debug.h"

#include <esp_log.h>

#include "src/platform/mros_uart.h"

namespace {
constexpr const char* kUartDebugTag = "UARTDBG";
}

void uart_debug_init() {
  if (mros::platform::mros_uart_is_ready(UART_NUM_0)) {
    return;
  }
  mros::platform::UartConfig config = {};
  config.port = UART_NUM_0;
  config.tx_pin = UART_PIN_NO_CHANGE;
  config.rx_pin = UART_PIN_NO_CHANGE;
  config.baud_rate = 115200;
  config.rx_buffer_size = 1024;
  config.tx_buffer_size = 1024;
  config.queue_size = 0;
  (void)mros::platform::mros_uart_init(config);
}

void uart_debug_log(const char *msg) {
  if (msg == nullptr) {
    return;
  }
  if (mros::platform::mros_uart_is_ready(UART_NUM_0)) {
    (void)mros::platform::mros_uart_write_line(UART_NUM_0, msg);
    return;
  }
  ESP_LOGI(kUartDebugTag, "%s", msg);
}
