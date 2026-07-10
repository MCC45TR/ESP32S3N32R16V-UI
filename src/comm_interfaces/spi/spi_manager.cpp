#include "spi_manager.h"
#include "spi_t41_link.h"
#include "spi_f4_slave.h"
#include "spi_c3_master.h"

void spi_manager_init() {
  spi_slave_s3_init();
  spi_f4_slave_init();
  spi_c3_master_init();
}

void spi_manager_loop(unsigned long now_ms) {
  spi_slave_s3_loop(now_ms);
  spi_f4_slave_loop(now_ms);
  spi_c3_master_poll();
}
