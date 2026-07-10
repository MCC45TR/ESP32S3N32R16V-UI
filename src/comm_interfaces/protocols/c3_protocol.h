#pragma once

#include <stddef.h>
#include <stdint.h>
#include "protocol_def.h"

bool c3_protocol_parse(const uint8_t *buf, size_t len);
bool c3_protocol_parse(const uint8_t *buf, size_t len, SPI_Packet_C3_to_STM *out_packet);
bool c3_protocol_build_frame(const SPI_Packet_C3_to_STM &packet, uint8_t *out_buf, size_t out_len);
