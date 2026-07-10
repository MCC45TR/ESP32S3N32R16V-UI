# ESP32S3N32R16V-UI Documentation

Language: English | [Turkish](README.tr.md)

This directory documents the ESP32-S3 side of MROS-DEUSCARA. English is the default language for newly created canonical pages. Turkish companion versions are linked from the top of each matching document.

## Canonical English Documents

| Document | Purpose |
| --- | --- |
| `esp32s3-comprehensive-operator-guide.md` | ESP32-S3 firmware overview, build profiles, LittleFS, web UI, shell, update, security and evidence gates |
| `web-ui-api-operator-guide.md` | Source-derived web UI, REST API, WebSocket, file manager, mshell/mros bridge, robot math API and evidence workflow guide |
| `esp32-shell-command-reference.md` | ESP32-S3 `mshell` command families, Wi-Fi, files, diagnostics, update, API wrapper and evidence recipes |
| `esp32-mros-subcommand-reference.md` | Detailed `mros` bridge-level subcommand reference for health, memory, power, buses, reports, security and support evidence |
| `robot-mros-runtime-guide.md` | Robot command framework, MROS runtime, safety flow, math backend, path planning, health data and diagnostics |
| `teensy-link-runtime-guide.md` | ESP32-S3 to Teensy UART, SPI/QSPI compatibility, robot runtime, digital twin and process-data bridge |
| [TEENSY41 robot equations](https://github.com/MCC45TR/TEENSY41-Brain/blob/main/docs/robot-communication-math-equations-reference.md) | Canonical cross-system communication and robot math equations reference |
| [TEENSY41 MATLAB/Simulink manual](https://github.com/MCC45TR/TEENSY41-Brain/blob/main/docs/matlab-simulink-operator-manual.md) | Source-derived MATLAB/Simulink USB RT bridge, state/health vector, timing and experiment evidence manual |
| `robot-mechanics-digital-twin.md` | Existing digital twin mechanics data model |
| `robot-prr-scara-process-plan.md` | Existing PRR SCARA actuator and process planning document |
| `bearing-health-skf-config.md` | Bearing/actuator/structural health configuration |
| `security/esp32-production-security-profile.md` | Production security profile |
| `../TEENSY41_CONNECTIONS.md` | Cross-repo wiring and link decision document |

## Turkish Companion Documents

| English | Turkish |
| --- | --- |
| `esp32s3-comprehensive-operator-guide.md` | `esp32s3-comprehensive-operator-guide.tr.md` |
| `web-ui-api-operator-guide.md` | `web-ui-api-operator-guide.tr.md` |
| `esp32-shell-command-reference.md` | `esp32-shell-command-reference.tr.md` |
| `esp32-mros-subcommand-reference.md` | `esp32-mros-subcommand-reference.tr.md` |
| `robot-mros-runtime-guide.md` | `robot-mros-runtime-guide.tr.md` |
| `teensy-link-runtime-guide.md` | `teensy-link-runtime-guide.tr.md` |

## ESP32-S3 Role Boundary

The ESP32-S3 is the operator and service controller:

- web UI and HMI,
- Wi-Fi station/AP and local web service,
- WebSocket/API telemetry and command transport,
- LittleFS file manager and robot data store,
- recovery/update workflow,
- digital twin and process planning,
- Teensy UART service bridge and SPI/QSPI peer endpoint.

It is not the final robot-motion safety authority. Teensy owns deterministic acceptance/rejection of live motion and motor commands.

## Build Shortlist

```powershell
pio run -e s3_mros_hub_n32
pio run -e s3_mros_hub_n16
pio run -e s3_mros_hub_n8
pio run -d recovery_app -e recovery_s3
pio run -e s3_mros_hub_n32 -t upload
pio device monitor -b 115200
```

Robot process data validation:

```powershell
.\tools\validate_robot_process_data.ps1
```

Do not claim live robot proof from an ESP32 build alone. Web state, LittleFS files, and process plans must be accepted by the Teensy safety/motion layer before they become robot action.
