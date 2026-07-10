#pragma once

// Legacy UART encoder abstraction kept for compatibility.
// Backend is currently routed to SPI C3 master telemetry.

void uart_encoder_init();
void uart_encoder_loop(unsigned long now_ms);
bool uart_encoder_is_connected();
void uart_encoder_request_reset();
float uart_encoder_get_position_deg();
float uart_encoder_get_speed_deg_s();
float uart_encoder_get_accel_deg_s2();
