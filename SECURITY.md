# Security Policy

## Reporting

Do not place credentials, private keys, device identifiers, network details or exploit steps in a public issue. Use GitHub private vulnerability reporting or a Security Advisory for this repository. Include the affected commit, target board, partition profile, sanitized reproduction steps and the expected/observed security state.

## Credential Boundary

- `src/config/wifi_secrets.h`, local PlatformIO overrides, generated `sdkconfig` files and private provisioning material must never be committed.
- Public builds contain no infrastructure SSID or password. The setup hotspot remains disabled until a device-specific password is supplied locally.
- NVS and coredump partitions are reserved in the flash layout but are not populated in the public full-flash image.
- Logs, screenshots and release notes must use `<SERIAL_PORT>` and must not include local paths, tokens, passwords or realistic network examples.
- If a credential ever entered Git history or a public artifact, remove it from the publication snapshot and rotate it. History rewriting alone does not make an exposed credential safe again.

## Firmware Safety Boundary

This is research firmware, not a certified functional-safety product. A successful build does not prove safe robot operation. Physical E-stop, stale-command rejection, ESP32-to-Teensy link recovery and motor-power behavior require current hardware evidence before live motion.
