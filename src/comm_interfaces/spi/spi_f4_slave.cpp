#include "spi_f4_slave.h"
#include "src/comm_interfaces/protocols/master_protocol.h"
#include "src/drivers/utils/mros_console.h"
#include "src/platform/mros_time.h"

namespace {

SpiF4Status g_status = {};
SPI_Packet_STM_to_ESP g_last_packet = {};
unsigned long g_last_diag_ms = 0;
const unsigned long kTimeoutMs = 1200UL;

}  // namespace

void spi_f4_slave_init() {
  g_status = {};
  g_status.initialized = true;
}

void spi_f4_slave_loop(unsigned long now_ms) {
  if (!g_status.initialized) spi_f4_slave_init();
  g_status.connected = (g_status.last_rx_ms != 0) && ((now_ms - g_status.last_rx_ms) <= kTimeoutMs);
  if (!g_status.connected && g_status.rx_count > 0 && (now_ms - g_last_diag_ms) > 3000UL) {
    g_last_diag_ms = now_ms;
    mros_console.println("[SPI-F4] Link timeout.");
  }
}

bool spi_f4_slave_feed_frame(const uint8_t *buf, size_t len) {
  if (!g_status.initialized) spi_f4_slave_init();
  SPI_Packet_STM_to_ESP parsed;
  if (!buf || len != SPI5_FRAME_SIZE) return false;

  const SPI_Packet_STM_to_ESP *raw = reinterpret_cast<const SPI_Packet_STM_to_ESP *>(buf);
  if (raw->start_marker != SPI5_START_MARKER_STM_TO_ESP) {
    g_status.marker_errors++;
    return false;
  }
  if (!master_protocol_parse_64b(buf, len, &parsed)) {
    g_status.crc_errors++;
    return false;
  }

  g_last_packet = parsed;
  g_status.last_seq = parsed.sequence_id;
  g_status.last_rx_ms = mros::platform::mros_millis();
  g_status.rx_count++;
  g_status.connected = true;
  return true;
}

bool spi_f4_slave_get_last_packet(SPI_Packet_STM_to_ESP *out_packet) {
  if (!out_packet || g_status.rx_count == 0) return false;
  *out_packet = g_last_packet;
  return true;
}

SpiF4Status spi_f4_slave_get_status() { return g_status; }
