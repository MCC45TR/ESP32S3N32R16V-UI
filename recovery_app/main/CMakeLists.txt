idf_component_register(
  SRCS "recovery_main.cpp" "recovery_web.cpp"
  INCLUDE_DIRS "."
  REQUIRES app_update joltwallet__littlefs mros_update_shared mbedtls espressif__cjson spi_flash
           esp_http_server esp_wifi nvs_flash esp_netif esp_event espressif__led_strip
  EMBED_FILES "../../data/recovery.html"
)
