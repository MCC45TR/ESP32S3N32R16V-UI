#include "uart_encoder.h"
#include "src/drivers/spi/spi_c3_master.h"

void uart_encoder_init() { spi_c3_master_init(); }

void uart_encoder_loop(unsigned long /*now_ms*/) { spi_c3_master_poll(); }

bool uart_encoder_is_connected() { return spi_c3_is_connected(); }

void uart_encoder_request_reset() { spi_c3_request_encoder_reset(); }

float uart_encoder_get_position_deg() { return spi_c3_get_position_deg(); }

float uart_encoder_get_speed_deg_s() { return spi_c3_get_speed_deg_s(); }

float uart_encoder_get_accel_deg_s2() { return spi_c3_get_accel_deg_s2(); }
