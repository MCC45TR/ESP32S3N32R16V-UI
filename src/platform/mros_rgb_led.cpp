#include "src/platform/mros_rgb_led.h"

#include "src/config/pin_config.h"

#if defined(__has_include)
#if __has_include(<led_strip.h>)
#include <led_strip.h>
#define MROS_HAS_LED_STRIP 1
#else
#define MROS_HAS_LED_STRIP 0
#endif
#else
#define MROS_HAS_LED_STRIP 0
#endif

#include <esp_err.h>
#include <esp_log.h>

namespace mros::platform {
namespace {

#if MROS_HAS_LED_STRIP
led_strip_handle_t g_led_strip = nullptr;
#endif
bool g_led_init_attempted = false;

bool ensure_rgb_led_ready() {
  if (g_led_init_attempted) {
#if MROS_HAS_LED_STRIP
    return g_led_strip != nullptr;
#else
    return false;
#endif
  }
  g_led_init_attempted = true;

#if MROS_HAS_LED_STRIP
  led_strip_config_t strip_config = {};
  strip_config.strip_gpio_num = PIN_ONBOARD_RGB_LED;
  strip_config.max_leds = 1;
  strip_config.led_model = LED_MODEL_WS2812;
  strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  strip_config.flags.invert_out = false;

  led_strip_rmt_config_t rmt_config = {};
  rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
  rmt_config.resolution_hz = 10000000;
  rmt_config.mem_block_symbols = 64;
  rmt_config.flags.with_dma = false;

  if (led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip) != ESP_OK ||
      g_led_strip == nullptr) {
    ESP_LOGW("RGB", "led_strip init failed; onboard RGB status LED disabled");
    g_led_strip = nullptr;
    return false;
  }
  ESP_LOGI("RGB", "onboard RGB status LED ready on GPIO%d", PIN_ONBOARD_RGB_LED);
  return true;
#else
  ESP_LOGW("RGB", "led_strip component unavailable; onboard RGB status LED disabled");
  return false;
#endif
}

}  // namespace

void mros_rgb_led_set(const uint8_t red, const uint8_t green, const uint8_t blue) {
#if MROS_HAS_LED_STRIP
  if (!ensure_rgb_led_ready()) {
    return;
  }
  if (led_strip_set_pixel(g_led_strip, 0, red, green, blue) != ESP_OK) {
    return;
  }
  (void)led_strip_refresh(g_led_strip);
#else
  (void)red;
  (void)green;
  (void)blue;
  (void)ensure_rgb_led_ready();
#endif
}

}  // namespace mros::platform
