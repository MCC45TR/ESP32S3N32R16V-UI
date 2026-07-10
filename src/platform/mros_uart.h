#pragma once

#include <cstddef>
#include <cstdint>

#include <driver/uart.h>
#include <esp_err.h>
#include <freertos/queue.h>

namespace mros::platform {

struct UartConfig {
  uart_port_t port = UART_NUM_1;
  int tx_pin = UART_PIN_NO_CHANGE;
  int rx_pin = UART_PIN_NO_CHANGE;
  int rts_pin = UART_PIN_NO_CHANGE;
  int cts_pin = UART_PIN_NO_CHANGE;
  int baud_rate = 115200;
  uart_word_length_t data_bits = UART_DATA_8_BITS;
  uart_parity_t parity = UART_PARITY_DISABLE;
  uart_stop_bits_t stop_bits = UART_STOP_BITS_1;
  uart_hw_flowcontrol_t flow_control = UART_HW_FLOWCTRL_DISABLE;
  uart_sclk_t source_clock = UART_SCLK_DEFAULT;
  int rx_buffer_size = 4096;
  int tx_buffer_size = 4096;
  int queue_size = 20;
  int intr_flags = 0;
};

bool mros_uart_init(const UartConfig& config, QueueHandle_t* out_event_queue = nullptr);
bool mros_uart_is_ready(uart_port_t port);
QueueHandle_t mros_uart_event_queue(uart_port_t port);
int mros_uart_writable(uart_port_t port);
int mros_uart_available(uart_port_t port);
int mros_uart_read(uart_port_t port, void* data, size_t size, TickType_t timeout);
bool mros_uart_read_byte(uart_port_t port, uint8_t* value, TickType_t timeout);
int mros_uart_write(uart_port_t port, const void* data, size_t size);
int mros_uart_write_line(uart_port_t port, const char* text);
void mros_uart_flush_input(uart_port_t port);

}  // namespace mros::platform
