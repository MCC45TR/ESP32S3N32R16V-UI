# ESP32-S3 Comprehensive Operator Guide

Language: English | [Turkish](esp32s3-comprehensive-operator-guide.tr.md)

This guide explains the ESP32-S3 side of MROS-DEUSCARA as a professional UI, network, recovery, digital-twin, and service layer. Use [ESP32-S3 Web UI And API Operator Guide](web-ui-api-operator-guide.md) for the source-derived HTTP, WebSocket, file manager, `mshell`, `mros`, and robot API map.

## System Role

`ESP32S3N32R16V-UI` runs the ESP32-S3 firmware that provides:

- Wi-Fi station and AP/hotspot operation,
- web UI and browser-based HMI,
- WebSocket/API telemetry,
- LittleFS file manager and runtime data store,
- MSHELL-style diagnostic surface,
- recovery/update workflow,
- digital twin and robot process planning,
- UART service link to Teensy,
- classic SPI endpoint for current QSPI-named compatibility bring-up.

ESP32 may stage recipes, visualize robot state, request diagnostics, and present operator controls. It must not be treated as the hard realtime motor-control authority.

## Build Profiles

| Profile | Purpose |
| --- | --- |
| `s3_mros_hub_n32` | Main 32 MB flash profile, current default |
| `s3_mros_hub_n16` | 16 MB flash compatibility |
| `s3_mros_hub_n8` | 8 MB flash compatibility |
| `s3_mros_hub_ddr_n32` | DDR/PSRAM120 variant |
| `*_dbg` | Debug-oriented profiles |
| `recovery_s3` | Recovery firmware under `recovery_app/` |

Build commands:

```powershell
pio run -e s3_mros_hub_n32
pio run -e s3_mros_hub_n16
pio run -e s3_mros_hub_n8
pio run -d recovery_app -e recovery_s3
```

Upload and monitor:

```powershell
pio run -e s3_mros_hub_n32 -t upload
pio device monitor -b 115200
```

## Repository Layout

| Path | Purpose |
| --- | --- |
| `main/` | PlatformIO active source entry |
| `src/core/` | Runtime state, RTOS, power, health, memory |
| `src/shell/` | ESP32 shell command surface |
| `src/web/server/` | HTTP, API, WebSocket, auth, trajectory handlers |
| `src/web/web_kinematics3d.h` | Embedded browser/digital-twin mechanics surface |
| `src/comm_interfaces/uart/` | UART/COBS and Teensy service link |
| `src/comm_interfaces/spi/` | SPI/C3/T41 communication |
| `src/kinematics/` | Kinematics and robot model code |
| `src/control/` | PID and trajectory logic |
| `data/` | LittleFS files, web assets, robot data, materials, logs, user config |
| `recovery_app/` | Recovery firmware |
| `tools/` | Validation and helper scripts |

## LittleFS Data Model

Important runtime data lives under `data/` and is shipped in the filesystem image.

| Area | Files |
| --- | --- |
| Web UI | `index.html`, `setup.html`, `recovery.html`, `css/`, `js/`, `assets/` |
| Robot mechanics | `data/robot/*.csv`, `data/robot/*.json` |
| Materials | `data/materials/*.csv`, `data/materials_manifest.json` |
| User state | `data/ESPUSER/config.json`, logs, scripts, update files |
| Firmware update | `data/ESPUSER/firmware/current/`, `inbox/`, `update.json` |
| Shell config | `data/ESPUSER/mshell_aliases.txt`, `mshell_config.txt`, `pid.cfg` |

Never publish real Wi-Fi credentials, private SSH keys, production tokens, or private host details in LittleFS seed data.

## Web And Digital Twin Runtime

The browser layer loads CAD/robot/material sidecars and provides:

- 3D robot viewer,
- material assignment,
- seed mass/COM/inertia estimates,
- bearing load estimates,
- actuator torque margin estimates,
- structural screening rows,
- process feed planning,
- runtime load export through `force-snapshot.json`.

Useful browser helpers:

```js
window.kin3d_setActiveProcessMode('plasma_air_mild_steel_medium')
window.kin3d_computeProcessFeedPlan({
  mode_id: 'plasma_air_mild_steel_medium',
  material_family: 'mild_steel',
  thickness_mm: 10,
  requested_feed_mm_min: 900,
  vendor_chart_id: 'coupon-chart-row',
  dry_run_passed: true
})
window.kin3d_getRobotProcessSnapshot()
window.kin3d_exportRobotRuntimeLoadsJson()
```

The digital twin is an engineering seed model. Final release decisions require CAD mass properties, real actuator data, measured loads, FEM/test evidence, and Teensy safety acceptance.

## Shell And Operator Surfaces

ESP32 shell command families include:

| Family | Purpose |
| --- | --- |
| `mros` | Runtime/system diagnostics |
| `mshell` | Local and remote shell bridge |
| `wifi` | Wi-Fi/AP/station diagnostics |
| `spi`, `uart` | Link diagnostics |
| `robot` | Robot state, health, process and link tests |
| `update-system`, `mros7dofs3-update`, `recovery` | Update/recovery workflow |
| `ls`, `cat`, `mount`, `lsblk`, `df` | Filesystem diagnostics |
| `project`, `status`, `debug`, `log`, `ps`, `free` | Runtime inspection |

High-value diagnostics:

```text
spi test --duration 10 --json
uart test --duration 10 --json
robot diag linktest uart --duration 10 --json
robot diag linktest qspi --duration 10 --json
robot health --json
update-system status
```

## Security And Publication Rules

- Keep real credentials out of Git.
- Keep `.env`, local Wi-Fi secrets, private keys, production SSH identities, and real update credentials local.
- Redact logs before publication.
- Treat recovery and update binaries as release artifacts with provenance.
- Do not expose a production web UI on an untrusted network without the production security profile.

## Evidence Gates

| Claim | Required evidence |
| --- | --- |
| Build works | PlatformIO log for selected env |
| Device boots | Serial boot log and version |
| Web UI works | Browser load, static assets, console free of critical errors |
| LittleFS works | File listing/read/write/update proof |
| UART works | `uart test --json` with wiring notes |
| SPI/QSPI compatibility works | `spi test --json` plus Teensy `comm qspi` proof |
| Process planner works | CSV validation plus browser helper output |
| Robot action is safe | Teensy acceptance, CAN heartbeat, safety gates, motor-disabled proof |
