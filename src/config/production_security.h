#pragma once

#include "sdkconfig.h"

// MROS_PRODUCTION_BUILD is the release fuse: lab builds may stay flexible, but
// production images must not leave the device readable or unsigned at rest.
#if defined(MROS_PRODUCTION_BUILD)

#if !defined(CONFIG_SECURE_BOOT) || !CONFIG_SECURE_BOOT
#error "MROS_PRODUCTION_BUILD requires CONFIG_SECURE_BOOT=y"
#endif

#if !defined(CONFIG_FLASH_ENCRYPTION_ENABLED) || !CONFIG_FLASH_ENCRYPTION_ENABLED
#error "MROS_PRODUCTION_BUILD requires CONFIG_FLASH_ENCRYPTION_ENABLED=y"
#endif

#if !defined(CONFIG_NVS_ENCRYPTION) || !CONFIG_NVS_ENCRYPTION
#error "MROS_PRODUCTION_BUILD requires CONFIG_NVS_ENCRYPTION=y"
#endif

#if !defined(CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK) || !CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
#error "MROS_PRODUCTION_BUILD requires CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y"
#endif

#if !defined(CONFIG_APP_SECURE_VERSION)
#error "MROS_PRODUCTION_BUILD requires CONFIG_APP_SECURE_VERSION to be set"
#endif

#endif  // defined(MROS_PRODUCTION_BUILD)
