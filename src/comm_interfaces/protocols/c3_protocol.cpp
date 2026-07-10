#include "c3_protocol.h"
#include "src/utils/crc_utils.h"
#include <string.h>

bool c3_protocol_parse(const uint8_t *buf, size_t len) {
  return c3_protocol_parse(buf, len, nullptr);
}

bool c3_protocol_parse(const uint8_t *buf, size_t len, SPI_Packet_C3_to_STM *out_packet) {
  if (!buf || len != SPI4_FRAME_SIZE) return false;
  SPI_Packet_C3_to_STM pkt;
  memcpy(&pkt, buf, sizeof(pkt));
  const uint16_t calc = crc16_ccitt(buf, 13);
  if (calc != pkt.crc16) return false;
  if (pkt.stat == 0) return false;
  if (out_packet) *out_packet = pkt;
  return true;
}

bool c3_protocol_build_frame(const SPI_Packet_C3_to_STM &packet, uint8_t *out_buf, size_t out_len) {
  if (!out_buf || out_len < SPI4_FRAME_SIZE) return false;
  SPI_Packet_C3_to_STM out = packet;
  out.crc16 = 0;
  memcpy(out_buf, &out, sizeof(out));
  const uint16_t crc = crc16_ccitt(out_buf, 13);
  memcpy(out_buf + 13, &crc, sizeof(crc));
  return true;
}
