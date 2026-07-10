#include "src/core/main.h"
#include "src/config/production_security.h"
#include "src/update/update_runtime.h"

extern "C" void app_main(void) {
  mros::update::update_runtime_boot_guard();
  core_main_setup();
  while (true) {
    core_main_loop();
  }
}
