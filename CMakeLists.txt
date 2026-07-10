cmake_minimum_required(VERSION 3.16.0)
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/managed_components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32S3N32R16-BRIDGE-PID-WEB)

idf_build_get_property(MROS_IDF_TARGET IDF_TARGET)
idf_build_get_property(MROS_PYTHON PYTHON)

set(MROS_RECOVERY_APP_DIR "${CMAKE_SOURCE_DIR}/recovery_app")
set(MROS_RECOVERY_BUILD_DIR "${CMAKE_BINARY_DIR}/rec")
set(MROS_RECOVERY_BIN "${MROS_RECOVERY_BUILD_DIR}/MROS-RECOVERY-UPDATER.bin")
set(MROS_RECOVERY_SDKCONFIG "${MROS_RECOVERY_BUILD_DIR}/sdkconfig")
set(MROS_RECOVERY_SDKCONFIG_DEFAULTS "${MROS_RECOVERY_APP_DIR}/sdkconfig.recovery_s3")
set(MROS_PARTITION_CSV "${CMAKE_SOURCE_DIR}/partitions-N32R16.csv")

if(EXISTS "${MROS_RECOVERY_APP_DIR}/CMakeLists.txt")
    add_custom_target(mros_recovery_app ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MROS_RECOVERY_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -G "${CMAKE_GENERATOR}"
            -S "${MROS_RECOVERY_APP_DIR}"
            -B "${MROS_RECOVERY_BUILD_DIR}"
            -DIDF_TARGET=${MROS_IDF_TARGET}
            -DPYTHON=${MROS_PYTHON}
            -DPYTHON_DEPS_CHECKED=1
            -DSDKCONFIG=${MROS_RECOVERY_SDKCONFIG}
            -DSDKCONFIG_DEFAULTS=${MROS_RECOVERY_SDKCONFIG_DEFAULTS}
        COMMAND ${CMAKE_COMMAND} --build "${MROS_RECOVERY_BUILD_DIR}"
        COMMAND ${MROS_PYTHON}
            "${CMAKE_SOURCE_DIR}/tools/check_partition_image_size.py"
            --partition-csv "${MROS_PARTITION_CSV}"
            --partition recovery
            --image "${MROS_RECOVERY_BIN}"
        BYPRODUCTS "${MROS_RECOVERY_BIN}"
        COMMENT "Building recovery_app for the fixed recovery partition"
        VERBATIM
        USES_TERMINAL
    )

    if(NOT TARGET flash)
        message(FATAL_ERROR "ESP-IDF flash target is unavailable; recovery cannot be added to idf.py flash.")
    endif()
    if(NOT COMMAND esptool_py_flash_to_partition)
        message(FATAL_ERROR "ESP-IDF esptool partition flash helper is unavailable; recovery cannot be added to idf.py flash.")
    endif()

    add_dependencies(flash mros_recovery_app)
    esptool_py_flash_to_partition(flash recovery "${MROS_RECOVERY_BIN}")
endif()
