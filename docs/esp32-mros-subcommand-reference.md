# ESP32-S3 MROS Subcommand Reference

Language: English | [Turkish](esp32-mros-subcommand-reference.tr.md)

This document is the detailed reference for the ESP32-S3 `mros` command group. Use it together with [ESP32-S3 mshell Command Reference](esp32-shell-command-reference.md) and [Robot And MROS Runtime Guide](robot-mros-runtime-guide.md).

`mros` is the bridge-level operator namespace. It is not a general Linux compatibility layer and it is not the same as the robot motion command namespace. It groups the commands that inspect the S3 bridge, peers, buses, runtime health, memory, power mode, logs, reports, security, and support artifacts.

## Mental Model

The `mros` namespace has five operational layers:

| Layer | Command families | What it proves |
| --- | --- | --- |
| System health | `overview`, `health`, `doctor`, `perf`, `mem`, `sram`, `rtos`, `power` | The S3 runtime is alive, has enough memory, uses the intended power/RTOS mode, and can run diagnostics |
| Connectivity | `connections`, `bus`, `spi`, `uart`, `i2c`, `pca9685`, `wifi` | The S3 can see peers and local peripheral links |
| Robot bridge view | `robot`, `pid`, `fk`, `ik`, `telemetry` | The S3 bridge can read robot-facing runtime state and math/telemetry status |
| Evidence and support | `log`, `watch`, `record`, `export`, `diag`, `alerts`, `report`, `audit` | The operator can capture repeatable evidence for debugging or release notes |
| Administration | `security`, `users`, `ssh`, `config`, `test`, `downloader` | The operator can inspect or change sessions, users, SSH, config diffs, active tests, and downloaded artifacts |

Keep these proof gates separate:

- `mros health` is a bridge health check.
- `mros spi status all` is a peer/bus state check.
- `robot math ik preview ...` is a math/intent preview.
- Live physical motion is a hardware observation and must be proven separately.

## Quick Start

For a new boot:

```text
mros overview
mros health
mros doctor quick
mros connections status
mros bus errors
mros spi status all
mros uart status
mros power status
mros mem status --detail
```

For a support bundle:

```text
mros doctor all
mros export diagnostics
mros report create
mros audit export
```

For a repeating live monitor:

```text
mros watch -n 5 --count 12 health
mros connections watch -n 5 --count 12
mros spi monitor all -n 5 --count 12
mros log follow -n 50 --cycles 30 --interval 1
```

## System Commands

### `mros overview`

Prints an S3 bridge overview:

- uptime,
- Wi-Fi state and IP,
- heap and PSRAM free memory,
- LittleFS usage,
- connection table for T41, C3, UART logs, PCA9685 and Wi-Fi.

Use it as the first human-readable summary when a board boots.

### `mros health`

Runs a compact health check and returns a warning status if key peers or error counters look unhealthy.

It checks:

- T41 QSPI/SPI peer,
- C3 SPI peer,
- PCA9685 readiness,
- SPI error counters,
- motor power state,
- bearing-health table when configured.

Use it before motion, before update, and before publishing a runtime claim.

### `mros doctor quick|all|wifi|fs|robot|security|peer [--json]`

Runs a structured diagnostic snapshot. In JSON mode it reports storage, T41 QSPI, C3 SPI, Wi-Fi, shell sessions, job pool, transaction state, UART bridge state, and remote filesystem state.

Examples:

```text
mros doctor quick
mros doctor all
mros doctor wifi
mros doctor security
mros doctor all --json
```

Notes:

- `quick` is the default when no target is provided.
- `all` includes extra Wi-Fi and security/audit details.
- JSON mode is the preferred output for issue templates and automation.

### `mros perf status`

Prints performance-oriented runtime information:

- heap free/minimum,
- PSRAM free,
- power mode and CPU frequency,
- SRAM floor and largest internal block,
- active power locks and Wi-Fi power-save mode,
- JSON overflow count,
- shell response pool miss/drop,
- mshell job pool status,
- WebSocket client counts,
- FK/PID timing,
- RTOS deadline-slip summary.

Use it when web UI, shell, JSON, or telemetry behavior feels slow or unstable.

## Memory Commands

### `mros mem status [--detail|--json]`

Prints memory monitor state. Use `--detail` when investigating heap pressure and `--json` when collecting machine-readable evidence.

```text
mros mem status
mros mem status --detail
mros mem status --json
```

### `mros mem watch start|stop|status`

Controls the memory watch sampler.

```text
mros mem watch status
mros mem watch start --interval-ms 5000
mros mem watch stop
```

### `mros mem snapshot [name]`

Stores a named memory snapshot.

```text
mros mem snapshot before-web-test
mros mem snapshot after-web-test
```

### `mros mem diff [a b]`

Prints JSON diff data between snapshots. If names are omitted, the implementation uses its default comparison behavior.

```text
mros mem diff before-web-test after-web-test
```

### `mros mem leaks [--json]`

Prints leak-oriented monitor output.

### `mros mem reset`

Resets memory monitor state.

Operator rule: do not run heavy report generation when internal SRAM is already critical; `mros report create` explicitly refuses that case and asks for `mros mem status --detail`.

## SRAM And RTOS

### `mros sram status --detail`

Samples and prints internal SRAM status. Use it when shell pools, report generation, JSON output, or web behavior appears unstable.

### `mros sram reclaim-plan`

Prints the reclaim plan for reducing SRAM pressure. This is a planning/diagnostic command, not a magic allocator reset.

### `mros rtos status`

Prints tracked RTOS task counts, wake/deadline miss totals, maximum deadline miss, and maximum execution time.

### `mros rtos policy get|set observe|balanced|cool|performance|motion-safe|update-safe`

Delegates to the device process manager policy surface. Use it to inspect or change runtime scheduling/power behavior.

### `mros rtos wake <task> [reason]`

Delegates a wake request to the device process manager.

## Power Commands

### `mros power status`

Prints power manager status. Use it before motion or update operations.

### `mros power mode cool|balanced|performance|motion-safe|update-safe`

Reads or changes the power mode.

```text
mros power mode
mros power mode balanced
mros power mode motion-safe
mros power mode update-safe
```

Mode guidance:

| Mode | Use |
| --- | --- |
| `cool` | Idle, thermal-sensitive or long observation periods |
| `balanced` | Normal service operation |
| `performance` | Heavy web, diagnostics, or data export work |
| `motion-safe` | Motion-sensitive operation |
| `update-safe` | Firmware download, staging, and recovery/update preparation |

### `mros power locks`

Prints active power management locks.

### `mros power temp`

Prints temperature if the power manager has a valid reading.

### `mros power trace`

Prints recent power trace information.

### `mros power report`

Delegates to the device process manager report path.

## Connection Commands

### `mros connections list|status|tree`

Prints peer and bus topology.

```text
mros connections list
mros connections status
mros connections tree
```

The status table covers:

- `t41-qspi`: ESP32-S3 to Teensy 4.1 peer path,
- `c3-spi`: ESP32-C3 peer path,
- `t41-uart-log`: Teensy UART log stream,
- `pca9685`: I2C PWM controller,
- `wifi`: station/network state.

### `mros connections watch -n 5 [--count 12]`

Runs the connection status table repeatedly using the generic `mros watch` engine.

## Bus And Peripheral Commands

### `mros bus summary|errors`

`summary` prints the same high-level connection table. `errors` focuses on CRC, marker, timeout and peer-specific error details.

```text
mros bus summary
mros bus errors
```

### `mros spi list`

Lists SPI peers known to the bridge.

### `mros spi status [t41|c3|all]`

Prints per-peer SPI state.

For T41 it includes connection state, transaction count, loop timing, CRC/marker counters, last marker, last sequence, and device status.

For C3 it includes connection state, receive count, loop rate, CRC/marker counters, last-good age, and ESP-NOW flags.

### `mros spi errors --last 25`

Prints the T41 error log JSON and C3 counters. The current implementation accepts the documented command form; the `--last` value is informational for compatibility with the help text.

### `mros spi reset-stats`

Resets T41 and C3 SPI error counters.

### `mros spi monitor [t41|c3|all] -n 5 [--count 12]`

Repeatedly prints SPI status through `mros watch`.

### `mros uart list|status|tail|monitor|log-mode|shell`

UART commands inspect the Teensy log path and the remote shell bridge.

```text
mros uart list
mros uart status
mros uart tail -n 20
mros uart monitor encoder -n 5 --count 12
mros uart log-mode
mros uart log-mode quiet
mros uart log-mode normal
mros uart log-mode verbose
mros uart shell status
mros uart shell reset
```

Use `quiet` when serial log noise hides important lines, and `verbose` when diagnosing the UART bridge itself.

### `mros i2c scan|status`

Scans I2C or prints I2C status.

### `mros pca9685 status|channels`

Prints PCA9685 readiness or channel information. Use it when servo/PWM output behavior is suspected.

### `mros wifi diag`

Prints Wi-Fi diagnostic summary:

- Wi-Fi phase and manager enabled state,
- STA/IP/RSSI/channel,
- last-good SSID/channel/BSSID,
- fast-path success and attempt counters,
- scan age and reconnect backoff,
- last connect duration.

## Robot Bridge View

These commands read bridge-side robot state. They complement, but do not replace, the canonical `robot ...` command family.

### `mros robot state`

Prints turret target/actual, PID output/error, joint values, Cartesian coordinates, gripper value, and trajectory active flag.

### `mros robot errors`

Prints T41 error code, device status, T41 QSPI CRC/marker counts, C3 SPI CRC/marker counts, and turret PID error.

### `mros pid status`

Prints turret PID target/actual, error/output, gains, output lock, motor/PCA state, loop timing and local control diagnostic counters.

### `mros fk status`

Prints FK-related status including T41 coordinates, live web FK timing, active IK backend and live preview state.

### `mros ik status`

Prints IK preference, effective backend, T41 QSPI/ESP-NOW availability, target coordinates and trajectory scale.

### `mros telemetry status`

Prints console revision, last web feedback age, PID timing, CPU frequency and WebSocket exposure notes.

## Log, Watch, Record, Export

### `mros log tail|follow [-n LINES] [--cycles NUM] [--interval SEC]`

Reads the runtime log tail once or repeatedly.

```text
mros log tail -n 50
mros log follow -n 50 --cycles 30 --interval 1
```

### `mros watch -n 5 [--count 12] <mros-subcommand>`

Runs any non-watch `mros` subcommand repeatedly.

```text
mros watch -n 5 --count 12 health
mros watch -n 2 --count 20 bus errors
```

Nested watch commands are rejected.

### `mros record start spi [--seconds NUM]`

Writes `/ESPUSER/diag_spi.csv` with one sample per second for up to 30 seconds.

Columns include:

- monotonic milliseconds,
- T41 connection state,
- T41 transaction count,
- T41 CRC and marker counters,
- T41 sequence,
- C3 receive and CRC counters,
- C3 loop rate,
- C3 position and speed.

### `mros export diagnostics`

Writes `/ESPUSER/diagnostics.txt` with a plain text diagnostic snapshot.

### `mros diag bundle`

Creates a diagnostic bundle using the implemented report/export paths.

### `mros alerts`

Prints active alert summary.

## Reports And Audit

### `mros report create|list|show|delete`

Report commands manage JSON support reports under `/ESPUSER/reports`.

```text
mros report create
mros report list
mros report show /ESPUSER/reports/report-123.json
mros report delete /ESPUSER/reports/report-123.json
```

Safety notes:

- Report creation samples memory before and after.
- Report creation refuses to run when internal SRAM is critical.
- Report delete requires a path under `/ESPUSER/reports/`.

### `mros audit list|export|clear`

Audit commands read, export, or clear the shell/security audit ring.

```text
mros audit list
mros audit export
mros audit clear
```

`export` writes `/ESPUSER/audit.txt`. `clear` requires admin/root.

## Security, Users And SSH

### `mros security status|logout-all|audit`

```text
mros security status
mros security audit
mros security logout-all
```

`status` prints HTTP session, login fail/lockout, serial auth, WebSocket auth counts, shell session counts, capability mask and UART shell bridge mode.

`logout-all` invalidates HTTP/WS sessions and requires admin/root.

### `mros users list|roles|add|passwd|disable`

```text
mros users list
mros users roles
mros users add "Display Name" username "password" admin sudo
mros users passwd username "new-password"
mros users disable username
```

Rules:

- Adding users requires admin/root.
- The implementation limits normal extra users.
- Password changes require admin/root or self, and root password changes require a root session.
- Only extra users can be disabled.

### `mros ssh status|enable|set|list`

```text
mros ssh status
mros ssh enable
mros ssh set disable
mros ssh set port 5378
mros ssh set passwd "new-pass"
mros ssh set root-passwd "new-root-pass"
mros ssh set pubkey "ssh-ed25519 ..."
mros ssh list
```

Notes:

- Passwords must be 8 to 96 characters.
- `root-passwd` requires a root shell session.
- Public keys must begin with `ssh-ed25519` or `ssh-rsa`.
- Do not paste real credentials into GitHub documentation or issue comments.

## Config, Test And Downloader

### `mros config diff`

Prints bridge configuration difference information.

### `mros test spi|uart|i2c|all`

Runs active lightweight tests for the selected subsystem.

### `mros downloader URL [TARGET]`

Downloads a file to `/ESPUSER` or `/user` storage.

```text
mros downloader "https://example.com/app.bin" auto
mros downloader "https://example.com/app.bin" /ESPUSER/firmware/app.bin
```

Rules:

- Wi-Fi must be connected.
- LittleFS must be mounted.
- Target must be inside `/ESPUSER` or `/user`.
- HTTPS URL, valid TLS and a public host are required.
- When target is `auto`, update-like or `.bin` names are placed under an updates directory; other names go under downloads.

## Evidence Recipes

### Bridge Health Evidence

```text
mros overview
mros health
mros doctor quick --json
mros mem status --detail
mros perf status
mros power status
```

### Link Evidence

```text
mros connections status
mros connections tree
mros bus errors
mros spi status all
mros uart status
mros i2c status
mros pca9685 status
```

### Robot Bridge Evidence

```text
mros robot state
mros robot errors
mros pid status
mros fk status
mros ik status
mros telemetry status
robot status --json
robot diag status --json
```

### Support Package Evidence

```text
mros record start spi --seconds 30
mros export diagnostics
mros audit export
mros report create
ls -l /ESPUSER
ls -l /ESPUSER/reports
```

## Do Not Overclaim

Use precise language in GitHub documentation:

| Claim | Minimum evidence |
| --- | --- |
| S3 runtime is alive | `mros overview` or `status --json` |
| Bridge health is acceptable | `mros health` and `mros doctor quick` |
| T41/C3 links are visible | `mros connections status` and `mros spi status all` |
| UART log bridge is active | `mros uart status` or `mros uart tail` |
| I2C/PCA path is visible | `mros i2c status` and `mros pca9685 status` |
| Memory is stable for report/export | `mros mem status --detail` |
| A support report exists | `mros report create` plus `mros report list` |
| Robot command is previewed | `robot ... preview --json` |
| Physical motion happened | live hardware observation, logs, and safety checklist |

This separation is the difference between a hobby README and a professional robotics repository.
