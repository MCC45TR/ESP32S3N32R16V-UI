idf_component_register(
  SRCS "mros_update_shared.cpp"
  INCLUDE_DIRS "include"
  REQUIRES espressif__cjson mbedtls nvs_flash
)
