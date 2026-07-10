#pragma once

#include <stddef.h>
#include <stdint.h>
#include "src/comm_interfaces/protocols/protocol_def.h"

struct SpiF4Status {
  bool initialized;
  bool connected;
  uint32_t rx_count;
  uint32_t crc_errors;
  uint32_t marker_errors;
  uint32_t last_rx_ms;
  uint8_t last_seq;
};

void spi_f4_slave_init();
void spi_f4_slave_loop(unsigned long now_ms);
bool spi_f4_slave_feed_frame(const uint8_t *buf, size_t len);
bool spi_f4_slave_get_last_packet(SPI_Packet_STM_to_ESP *out_packet);
SpiF4Status spi_f4_slave_get_status();
