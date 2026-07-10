# Release Process

The public release surface is intentionally smaller than the internal build package.

## Public Assets

A GitHub firmware release contains only:

- one N32R16 production `full-flash.bin` image;
- `SHA256SUMS` containing the checksum of that image.

Split bootloader, partition, OTA, recovery, application and LittleFS images may remain internal workflow artifacts. They are not public release assets.

## Preconditions

1. Build from a clean export of the proposed Git tree, not from a maintainer working tree.
2. Confirm the export contains no `wifi_secrets.h`, generated `sdkconfig`, `dependencies.lock`, local PlatformIO override, planning note or vendor demo private key.
3. Run the hygiene tests, public-release scan, project secret scan and a full snapshot Gitleaks scan.
4. Build `s3_mros_hub_n32`, build LittleFS and verify all flash offsets against `partitions-N32R16.csv` and the generated flash arguments.
5. Merge bootloader, partition table, OTA data, recovery, application and LittleFS images with `esptool` using the build-produced offsets.
6. Verify the merged image with `esptool image_info`, calculate SHA256 and download the GitHub asset again after publication to confirm the remote hash.

The manual release workflow may create a draft only from `s3_mros_hub_n32`. Other production/debug profiles can create internal artifacts but cannot create a GitHub release.

## Hardware Evidence

Compilation and image validation do not prove USB enumeration, boot, PSRAM, LittleFS, Wi-Fi, web UI, OTA, ESP32-to-Teensy communication or robot safety. Publish the image as a release candidate until those checks have been repeated on the target hardware.
