# ESP32-S3 Web UI And API Operator Guide

Language: English | [Turkish](web-ui-api-operator-guide.tr.md)

This guide documents the ESP32-S3 web, REST API, WebSocket, file manager, `mshell`, `mros`, and robot-math service surfaces. It is source-derived from `src/web/server/web_server.cpp` and should be read with [ESP32-S3 Comprehensive Operator Guide](esp32s3-comprehensive-operator-guide.md), [ESP32-S3 mshell Command Reference](esp32-shell-command-reference.md), [ESP32-S3 MROS Subcommand Reference](esp32-mros-subcommand-reference.md), and [Robot And MROS Runtime Guide](robot-mros-runtime-guide.md).

## Responsibility Boundary

The ESP32-S3 web layer is an operator and service interface. It can authenticate users, expose telemetry, manage LittleFS files, run shell commands, bridge selected requests to Teensy, stage robot data, and present the digital twin. It is not the final hard realtime robot-motion authority.

Use precise language:

- Web UI action means the operator requested or staged something.
- API success means the ESP32 handler accepted and processed the request.
- `mshell`/`mros` output means the shell surface answered.
- Robot action is not proven until Teensy safety and motion layers accept the request.
- Live robot proof requires physical evidence, not only a browser response.

## Main Browser Surfaces

| Route | Purpose | Operator use |
| --- | --- | --- |
| `/` | Main HMI and dashboard | Normal operator entry point |
| `/login` | Login screen | Authenticate before telemetry or control |
| `/setup` | First-run setup | Create initial credentials and device profile |
| `/debug` | Debug surface | Engineering diagnostics |
| `/mcp` | MCP/devtool surface | Developer-only integration path |
| `/main.js`, `/style.css`, `/kinematics3d.js` | Static UI assets | Loaded by the browser |

The web UI should be described as a professional service console: it combines status, robot visualization, file operations, shell bridging, and diagnostics. It should not be advertised as a certified safety pendant.

## WebSocket Channels

The firmware registers these WebSocket paths:

| Channel | Purpose | Notes |
| --- | --- | --- |
| `/ws` | Main telemetry and robot/digital-twin deltas | Authenticated clients receive state data |
| `/ws/telemetry` | Telemetry channel | Native primary telemetry route |
| `/ws-shell` | Legacy shell streaming | Kept for compatibility |
| `/ws/shell` | Current shell streaming | Preferred shell WebSocket path |
| `/ws/debug` | Debug telemetry | Engineering/debug clients |
| `/ws/mcp` | MCP/devtool streaming | Development integration |

Authentication matters. Unauthenticated WebSocket clients must not receive robot state data. Use `/api/ws-ticket` to obtain the ticket/handshake material expected by the UI flow.

## Authentication And Sessions

Important routes:

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/auth/state` | Current auth/setup state |
| `POST` | `/api/register` | First user registration |
| `POST` | `/api/login` | Login and session creation |
| `POST` | `/api/logout` | Revoke current session |
| `GET` | `/api/ws-ticket` | WebSocket auth ticket |
| `GET` | `/api/security/users` | User list/admin view |
| `POST` | `/api/security/users/add` | Add user |
| `POST` | `/api/security/users/password` | Change password |
| `POST` | `/api/security/users/delete` | Delete user |
| `POST` | `/api/security/sessions/revoke` | Revoke sessions |
| `POST` | `/api/security/auth-reset` | Auth reset workflow |
| `POST` | `/api/credentials` | Legacy credentials update, guarded by reauth/admin rules |

Security rules for documentation and publication:

- Never publish real Wi-Fi credentials, root passwords, user passwords, SSH private keys, session tokens, CSRF tokens, or generated setup secrets.
- Use obvious placeholders in examples.
- Explain setup and login as required before shell, file, diagnostics, and robot-related actions.
- Treat auth reset, user deletion, credential update, and session revocation as administrative operations.

## System Status And Health APIs

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/status` | Compact status including loop/auth/serial auth state |
| `GET` | `/api/health` | Native health route |
| `GET` | `/api/about` | Project/about metadata |
| `GET` | `/api/assets/manifest` | UI asset manifest |
| `GET` | `/api/services/state` | Service state |
| `POST` | `/api/services/update` | Service settings update |
| `GET` | `/api/devices/status` | T41/C3/SPI/WebSocket device state |
| `POST` | `/api/devices/test` | Device test request |
| `GET` | `/api/c3/status` | C3 peer status |
| `POST` | `/api/c3/reset` | Reset C3 peer |
| `GET` | `/api/c3/failsafe` | C3 failsafe state |
| `POST` | `/api/c3/failsafe` | Change C3 failsafe state |

Recommended read-only health snapshot:

```text
GET /api/status
GET /api/health
GET /api/devices/status
GET /api/c3/status
GET /api/logs/tail
GET /api/mros/doctor?target=quick
```

## Wi-Fi And Network APIs

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/wifi/scan` | Scan nearby networks |
| `POST` | `/api/wifi/connect` | Connect to a network |
| `POST` | `/api/wifi/save` | Save Wi-Fi credentials |
| `POST` | `/api/wifi/action` | Wi-Fi action such as disconnect/retry |
| `GET` | `/api/wifi/state` | Current Wi-Fi state |
| `GET` | `/api/wifi/diag` | Wi-Fi diagnostics |

Credential handling rule: examples must use placeholders such as `YOUR_WIFI_SSID` and `YOUR_WIFI_PASSWORD`. Real credentials belong in device-local storage, not in Git.

## Power, DPM, Memory, And Debug

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/dpm/status` | Device process manager status |
| `GET` | `/api/dpm/decision` | Current DPM decision |
| `GET` | `/api/dpm/tasks` | Runtime task summary |
| `POST` | `/api/dpm/policy` | Set DPM policy |
| `POST` | `/api/dpm/wake` | Wake a task/service |
| `GET` | `/api/dpm/frequency` | Frequency/performance state |
| `GET` | `/api/power/status` | Power state |
| `GET` | `/api/power/locks` | Active power locks |
| `POST` | `/api/power/mode` | Change power mode |
| `GET` | `/api/memory/status` | Memory status |
| `GET` | `/api/memory/sram` | Internal SRAM state |
| `GET` | `/api/memory/leaks` | Leak diagnostics |
| `GET` | `/api/debug/sysinfo` | Debug sysinfo |
| `GET` | `/api/debug/web` | Web/debug metrics |
| `GET` | `/api/debug/heap-trace` | Heap trace state |
| `POST` | `/api/debug/heap-trace` | Start/stop heap trace |
| `GET` | `/api/debug/coredump` | Coredump metadata |
| `GET` | `/api/debug/coredump/download` | Download coredump |
| `DELETE` | `/api/debug/coredump` | Clear coredump |

Use read-only routes first. Power mode, DPM policy, wake, heap trace, coredump deletion, and recovery reboot actions should be logged as operator actions.

## Shell, MSHELL, And MROS API Bridge

The ESP32 exposes both local shell session APIs and `mshell`/`mros` bridge APIs.

### Shell Sessions

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/shell/sessions` | List sessions |
| `POST` | `/api/shell/sessions` | Start session |
| `DELETE` | `/api/shell/sessions` | Delete/cancel session |
| `GET` | `/api/console` | Console snapshot |
| `GET` | `/api/console/delta` | Console delta |

### MSHELL Bridge

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/mshell/devices` | Remote shell device report |
| `POST` | `/api/mshell/connect` | Connect bridge target |
| `GET` | `/api/mshell/schema` | Schema for shell calls |
| `POST` | `/api/mshell/call` | Execute structured `mshell call` request |
| `GET` | `/api/mshell/jobs` | List shell jobs |
| `POST` | `/api/mshell/jobs` | Start shell job |
| `DELETE` | `/api/mshell/jobs` | Cancel shell job |
| `GET` | `/api/mshell/tx` | Transaction state |
| `POST` | `/api/mshell/tx` | Begin/stage/commit/rollback transaction |
| `POST` | `/api/settings/uart-shell` | Set UART shell bridge mode |

The web server validates command parameters before building `mshell` command strings. Documentation should keep that model: use structured parameters and safe atoms, not raw untrusted command strings.

### MROS Support APIs

| Method | Route | Shell equivalent |
| --- | --- |
| `GET` | `/api/mros/doctor` | `mros doctor <target> --json` |
| `GET` | `/api/mros/report` | `mros report list` or `mros report show <path>` |
| `POST` | `/api/mros/report` | `mros report create` |
| `DELETE` | `/api/mros/report` | `mros report delete <path>` |
| `GET` | `/api/mros/audit` | `mros audit list` |

Use these for support packages. A professional bug report should include `mros doctor quick`, status, logs, memory, power, device state, and a report file path when available.

## File Manager And LittleFS APIs

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/files/mounts` | List mounted filesystems |
| `POST` | `/api/files/mount` | Mount/unmount target |
| `GET` | `/api/files/list` | List files |
| `GET` | `/api/files/info` | File metadata |
| `GET` | `/api/files/download` | Download file |
| `POST` | `/api/files/mkdir` | Create directory |
| `POST` | `/api/files/delete` | Delete file/directory |
| `POST` | `/api/files/rename` | Rename file |
| `POST` | `/api/files/copy` | Copy file |
| `GET` | `/api/files/fetch/check` | Preflight remote fetch |
| `POST` | `/api/files/fetch/start` | Start remote fetch/download |
| `GET` | `/api/files/fetch/status` | Fetch progress |
| `POST` | `/api/files/fetch/cancel` | Cancel fetch |
| `POST` | `/api/files/upload` | Upload file |
| `POST` | `/api/files/save` | Save file content |

File manager proof should include path, size, checksum if relevant, and whether the file is seed data, runtime data, downloaded data, uploaded data, or generated evidence.

Never expose local secrets through file screenshots or examples. Be careful with `/ESPUSER/auth`, credentials files, Wi-Fi state, setup tokens, logs, and downloaded firmware artifacts.

## Robot, Math, PID, Trajectory, And Digital Twin APIs

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/robot/math/onboard` | Onboard math state/config |
| `POST` | `/api/robot/math/onboard` | Update onboard math settings |
| `POST` | `/api/robot/math/onboard/run` | Run onboard math operation |
| `GET` | `/api/trajectory/preview` | Preview trajectory |
| `GET` | `/api/trajectory/stats` | Trajectory statistics |
| `GET` | `/api/pid` | PID state |
| `POST` | `/api/pid` | Update PID settings |
| `GET` | `/api/robot/mechanics/manifest` | Robot mechanics manifest |
| `GET` | `/api/materials/manifest` | Materials manifest |
| `GET` | `/api/cad/manifest` | CAD manifest |
| `GET` | `/api/cad/version` | CAD data version |
| `GET` | `/api/svg` | SVG/visual asset helper |
| `GET` | `/api/pca/cal` | PCA calibration state |
| `POST` | `/api/pca/cal` | Upload/apply calibration |
| `POST` | `/api/pca/cal/reset` | Reset calibration |
| `GET` | `/api/pca/osc` | PCA oscillator setting |
| `POST` | `/api/pca/osc` | Set PCA oscillator |
| `POST` | `/api/pca/test` | PCA test |
| `GET` | `/api/turret/output_lock` | Output lock state |
| `POST` | `/api/turret/output_lock` | Change output lock |

Robot API rules:

- Use preview/stat endpoints before applying any runtime setting.
- Treat PID, oscillator, calibration, output lock, and onboard math changes as operator actions.
- Browser/digital-twin data is planning evidence until Teensy validates safety, limits, stale-command policy, and motion authority.
- Keep force/process snapshots with the exact robot-data and material manifest versions used.

## Config, Calibration, Profile, Recovery

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/settings/device` | Device settings |
| `GET` | `/api/settings/schema` | Settings schema |
| `GET` | `/api/settings/popup` | User popup/profile settings |
| `POST` | `/api/profile` | Update profile |
| `GET` | `/api/profile` | Read profile |
| `GET` | `/api/config/download` | Download config |
| `POST` | `/api/config/upload` | Upload config |
| `POST` | `/api/calibration/save` | Save calibration |
| `POST` | `/api/system/reboot-recovery` | Reboot into recovery |
| `POST` | `/set` | Legacy settings endpoint |

Before using recovery or update flows, capture current status, firmware profile, LittleFS state, and a rollback plan.

## Logs, SPI, And Evidence

| Method | Route | Purpose |
| --- | --- | --- |
| `GET` | `/api/logs` | Log view |
| `GET` | `/api/logs/tail` | Tail logs |
| `GET` | `/api/spi/errors` | SPI/T41 error state |

Suggested evidence bundle:

```text
GET /api/status
GET /api/health
GET /api/devices/status
GET /api/spi/errors
GET /api/mros/doctor?target=quick
GET /api/memory/status
GET /api/power/status
GET /api/logs/tail
POST /api/mros/report
GET /api/mros/report
```

## Operator Workflows

### First Setup

1. Open `/setup`.
2. Register a non-placeholder admin user.
3. Log in through `/login`.
4. Check `/api/auth/state`.
5. Save Wi-Fi through the UI or `/api/wifi/save` using private device-local credentials.
6. Verify `/api/status`, `/api/health`, and `/api/devices/status`.

### Bridge Bring-Up

1. Open authenticated UI.
2. Check `/api/mshell/devices`.
3. Use `/api/mshell/connect` for the intended target.
4. Check `/api/settings/uart-shell`.
5. Run `mros doctor quick` through `/api/mros/doctor`.
6. Compare ESP32 bridge state with Teensy MSHELL diagnostics.

### Robot Math Review

1. Load robot mechanics and material manifests.
2. Use `/api/robot/math/onboard` to inspect math mode.
3. Use `/api/robot/math/onboard/run` or shell `robot ... preview`.
4. Save process/force snapshot.
5. Validate equivalent math on Teensy with `robot math validate` and the relevant FK/IK/Jacobian commands.

### Support Package

1. Capture status, health, devices, memory, power, SPI errors, and logs.
2. Run `/api/mros/doctor?target=quick`.
3. Create a support report with `POST /api/mros/report`.
4. List or show the report with `GET /api/mros/report`.
5. Do not include secrets, passwords, tokens, or private network details in public issues.

## Claim Boundaries For GitHub

Use these phrases consistently:

- "The ESP32-S3 firmware exposes authenticated web UI, REST API, and WebSocket service surfaces."
- "The file manager operates on LittleFS/runtime paths; file presence is not motion proof."
- "`/api/mros/*` endpoints wrap `mros` support commands for diagnostics and reports."
- "`/api/mshell/*` endpoints bridge structured shell work; raw untrusted command strings are not the design contract."
- "Robot math preview and digital-twin output require Teensy safety acceptance before becoming live robot action."

