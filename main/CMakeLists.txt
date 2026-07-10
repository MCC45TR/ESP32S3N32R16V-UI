file(GLOB_RECURSE app_sources "../src/*.cpp")

set(extra_app_sources
  "${CMAKE_CURRENT_LIST_DIR}/../src/experimental/experimental_worker.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/core/health/bearing_health.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/core/power/power_manager.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/core/rtos/device_process_manager.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/shell/commands/mros_shell_cmd_devices.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/shell/commands/mros_shell_cmd_dpm.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/shell/commands/mros_shell_cmd_text_tools.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/../src/utils/mros_json_writer.cpp"
)
foreach(extra_app_source ${extra_app_sources})
  if(EXISTS "${extra_app_source}")
    list(FIND app_sources "${extra_app_source}" extra_app_source_index)
    if(extra_app_source_index EQUAL -1)
      list(APPEND app_sources "${extra_app_source}")
    endif()
  endif()
endforeach()

list(FILTER app_sources EXCLUDE REGEX ".*/src/shell/commands/mros_shell_cmd_(espnow|kill|math|update_c6)\\.cpp$")
list(FILTER app_sources EXCLUDE REGEX ".* \\([0-9]+\\)\\.cpp$")
list(FILTER app_sources EXCLUDE REGEX ".*/src/platform/(wifi_compat|dns_server_compat)\\.cpp$")

idf_component_register(
  SRCS ${app_sources}
  INCLUDE_DIRS "../src" ".."
  REQUIRES
    mros_update_shared
    app_update
    espressif__cjson
    esp_adc
    esp_event
    esp_rom
    mbedtls
    esp_driver_gpio
    esp_http_server
    esp_http_client
    esp_driver_i2c
    esp_driver_spi
    esp_driver_uart
    esp_driver_usb_serial_jtag
    esp_netif
    esp_pm
    esp_psram
    esp_driver_tsens
    esp_timer
    esp_wifi
    espressif__led_strip
    joltwallet__littlefs
    lwip
    spi_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  MROS_FULL_IDF_ONLY=1
  MROS_NATIVE_HTTP_PRIMARY=1
  MROS_WIFI_IDF_EVENT_PRIMARY=1
)

littlefs_create_partition_image(littlefs ../data FLASH_IN_PROJECT)
