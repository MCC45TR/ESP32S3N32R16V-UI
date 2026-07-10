#include "master_protocol.h"
#include "src/utils/crc_utils.h"
#include <string.h>

bool master_protocol_parse_64b(const uint8_t *buf, size_t len) {
  return master_protocol_parse_64b(buf, len, nullptr);
}

bool master_protocol_parse_64b(const uint8_t *buf, size_t len,
                               SPI_Packet_STM_to_ESP *out_packet) {
  if (!buf || len != SPI5_FRAME_SIZE) return false;
  SPI_Packet_STM_to_ESP packet;
  memcpy(&packet, buf, sizeof(packet));
  if (packet.start_marker != SPI5_START_MARKER_STM_TO_ESP) return false;
  const uint32_t calc_crc = crc32_ieee(buf, SPI5_FRAME_SIZE - sizeof(uint32_t));
  if (calc_crc != packet.crc32) return false;
  if (out_packet) *out_packet = packet;
  return true;
}

bool master_protocol_build_64b(const SPI_Packet_ESP_to_STM &packet, uint8_t *out_buf,
                               size_t out_len) {
  if (!out_buf || out_len < SPI5_FRAME_SIZE) return false;
  SPI_Packet_ESP_to_STM out = packet;
  out.start_marker = SPI5_START_MARKER_ESP_TO_STM;
  out.crc32 = 0;
  memcpy(out_buf, &out, sizeof(out));
  const uint32_t crc = crc32_ieee(out_buf, SPI5_FRAME_SIZE - sizeof(uint32_t));
  memcpy(out_buf + SPI5_FRAME_SIZE - sizeof(uint32_t), &crc, sizeof(uint32_t));
  return true;
}
