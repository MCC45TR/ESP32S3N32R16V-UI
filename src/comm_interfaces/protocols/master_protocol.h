#pragma once

#include <stddef.h>
#include <stdint.h>
#include "protocol_def.h"

bool master_protocol_parse_64b(const uint8_t *buf, size_t len);
bool master_protocol_parse_64b(const uint8_t *buf, size_t len, SPI_Packet_STM_to_ESP *out_packet);
bool master_protocol_build_64b(const SPI_Packet_ESP_to_STM &packet, uint8_t *out_buf, size_t out_len);
