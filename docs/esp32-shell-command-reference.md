# ESP32-S3 mshell Command Reference

Language: English | [Turkish](esp32-shell-command-reference.tr.md)

This page explains the ESP32-S3 `mshell` surface used by the DEUSCARA bridge firmware. It is written for a GitHub reader who needs to understand what the device can do before touching hardware, and for an operator who needs repeatable commands during bring-up, diagnostics, update, and recovery.

For the bridge-level `mros` namespace specifically, see [ESP32-S3 MROS Subcommand Reference](esp32-mros-subcommand-reference.md).

## Where The Shell Runs

`mshell` is the command interpreter embedded in the ESP32-S3 firmware. It can be reached from the serial console and, when enabled by the firmware/security configuration, through network shell surfaces such as SSH or web-mediated command execution.

The ESP32-S3 shell is not the deterministic motion authority. It is the service and operator shell for:

- Wi-Fi and network state,
- LittleFS and `/ESPUSER` file operations,
- web/telemetry/service diagnostics,
- recovery update staging,
- robot command intent creation,
- bridge diagnostics for Teensy 4.1, ESP32-C3, PCA9685, and UART/SPI links.

Motion that can physically affect the robot must still be accepted by the robot-control layer and by the active safety state.

## First Commands

Use these commands after boot to identify the firmware, storage, link state, and command vocabulary.

```text
help
man mshell
status
status --json
mfetch --full
project status
project info
mros health
mros connections status
devices status
```

Use JSON when the output is being consumed by a script, AI tool, web client, or CI capture:

```text
status --json
robot status --json
robot math status --json
mshell schema commands --json
mshell schema api --json
```

## Command Families

| Family | Commands | Purpose |
| --- | --- | --- |
| Help and shell metadata | `help`, `man`, `mshell`, `history`, `which`, `set`, `config` | Discover commands, inspect shell schemas, manage shell/session configuration |
| System state | `status`, `mfetch`, `uptime`, `date`, `uname`, `espfetch`, `free`, `ps`, `htop`, `dmesg` | Read firmware, heap, task, boot, and runtime state |
| Files and storage | `ls`, `tree`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `touch`, `df`, `du`, `mount`, `umount`, `lsblk`, `stat`, `find` | Work with LittleFS, `/ESPUSER`, mounted providers, logs, firmware images, reports |
| Text and binary inspection | `head`, `tail`, `wc`, `grep`, `sed`, `awk`, `nl`, `xxd`, `hexdump`, `od`, `strings`, `sha256sum`, `md5sum`, `crc32` | Inspect logs, manifests, scripts, diagnostics, binary payloads |
| Network | `wifi`, `ping`, `curl`, `wget`, `hostname`, `ssh`, `espnow` | Configure Wi-Fi, fetch files, test reachability, inspect shell-over-network access |
| Device links | `devices`, `mros connections`, `mros bus`, `mros spi`, `mros uart`, `mros i2c`, `gpio`, `i2cdetect`, `pwm`, `adc`, `spi`, `uart` | Inspect and test board-level links and peripherals |
| Robot interface | `robot`, `mros pid`, `mros fk`, `mros ik` | Create robot intents, inspect math state, preview or apply commands depending on safety and backend |
| Project information | `project` | Print embedded project, web page, goals, license, version, and TODO summaries |
| Update and recovery | `update-system`, `update-c6`, `mros7dofs3_update` | Stage firmware images and manage update-related flows |
| Security and audit | `whoami`, `id`, `groups`, `su`, `sudo`, `passwd`, `change`, `mros security`, `mros audit` | Inspect identity, privilege, session, and audit state |
| Reports and support | `mros report`, `mros diag`, `logger`, `journalctl`, `tar`, `gzip`, `gunzip`, `zcat` | Create support bundles, read logs, export and compress evidence |

## `mshell` Meta Commands

The `mshell` command exposes the shell as a structured API. This is the cleanest entry point for tooling because it can list available commands, export an API schema, route calls, and present consistent JSON.

| Command | Meaning |
| --- | --- |
| `mshell schema commands` | Print command registry with capability/risk metadata |
| `mshell schema commands --json` | Export the registry as JSON |
| `mshell schema api` | Print high-level API names mapped to shell commands |
| `mshell schema openapi-lite --json` | Export a small OpenAPI-like view for automation |
| `mshell call system.status` | Execute a named API call through the shell wrapper |
| `mshell call files.list path=/ESPUSER` | List files through the API wrapper |
| `mshell call robot.ik x=300 y=300 z=250` | Run the robot IK API wrapper |
| `mshell profile safe` | Run the short safe health profile |
| `mshell profile debug` | Run a broader debug profile |
| `mshell status s3|t41|all` | Inspect remote shell routing status |
| `mshell connect s3|t41|all` | Open a remote target session when supported |
| `mshell disconnect s3|t41|all` | Close a remote target session |

Known API names include:

| API | Backing command |
| --- | --- |
| `system.status` | `status --json` |
| `system.info` | `mfetch --json` |
| `wifi.status` | `wifi info` |
| `wifi.scan` | `wifi scan` |
| `files.list` | `ls -l {path}` |
| `files.mounts` | `mount status` |
| `robot.status` | `robot status --json` |
| `robot.math.status` | `robot math status --json` |
| `robot.math.benchmark` | `robot math benchmark --json` |
| `robot.fk` | `robot math fk solve {joints} --json` |
| `robot.ik` | `robot math ik preview {x} {y} {z} --json` |
| `devices.status` | `devices status` |
| `devices.test` | `devices test {target}` |
| `security.status` | `mros security status` |
| `report.create` | `mros report create` |
| `audit.list` | `mros audit list` |

## Wi-Fi And Network Commands

The Wi-Fi command manages station mode, hotspot mode, reconnects, scans, and the ESP-NOW failsafe mode.

```text
wifi connect "SSID" "PASSWORD"
wifi save "SSID" "PASSWORD"
wifi state on
wifi state off
wifi state hotspot
wifi state espnow
wifi reconnect
wifi scan
wifi info
wifi list now
wifi list saved
```

Operator notes:

- `wifi connect` both saves credentials and attempts a connection.
- `wifi save` stores credentials without forcing an immediate reconnect.
- `wifi state hotspot` forces AP/captive-portal mode.
- `wifi state espnow` selects the C3 failsafe path intended for C3SPI plus Teensy ESP-NOW behavior.
- Secrets should not be copied into GitHub issues, screenshots, or documentation examples.

Use network utility commands for reachability and downloads:

```text
ping 192.168.1.1
curl -I https://example.com
wget -O /ESPUSER/firmware/app.bin https://example.com/app.bin
```

## Filesystem And Evidence Commands

The operator-facing user area is normally `/ESPUSER`, backed by LittleFS or the configured storage alias. Use it for firmware images, reports, scripts, robot data, and diagnostic exports.

```text
df -h
mount status
lsblk
ls -l /ESPUSER
tree /ESPUSER
find /ESPUSER -name "*.bin"
sha256sum /ESPUSER/firmware/app.bin
tail -n 80 /ESPUSER/logs/runtime.log
grep error /ESPUSER/logs/runtime.log
tar -cf /ESPUSER/reports/support.tar /ESPUSER/reports
gzip /ESPUSER/reports/support.tar
```

Good evidence capture usually includes:

- `mfetch --full`,
- `mros health`,
- `mros connections status`,
- `mros bus errors`,
- `robot status --json`,
- `robot diag status --json`,
- relevant logs under `/ESPUSER`,
- the exact firmware version and build environment.

## Device And Link Diagnostics

Use `devices` for quick health checks and `mros` for deeper bus-specific diagnostics.

```text
devices status
devices test status
devices test t41
devices test uart
devices test s3-uart
devices test wifi
devices test pca
devices test web
devices test all
```

Useful MROS diagnostics:

```text
mros health
mros connections list
mros connections status
mros connections tree
mros bus summary
mros bus errors
mros spi list
mros spi status all
mros spi errors
mros uart status
mros uart tail
mros uart log-mode quiet
mros i2c scan
mros pca9685 status
mros log tail -n 120
mros diag bundle
mros alerts
```

For command-by-command behavior, operator rules, and evidence recipes, use [ESP32-S3 MROS Subcommand Reference](esp32-mros-subcommand-reference.md).

Interpretation boundary:

- `t41-qspi` reports the ESP32-S3 to Teensy SPI/QSPI bridge state.
- `t41-uart-log` reports UART log/console activity from the Teensy side.
- `c3-spi` reports the ESP32-C3 side path.
- `pca9685` reports the servo/PWM output controller readiness.
- A build or shell response does not prove safe physical motion. Physical proof requires the live robot safety chain, link state, and motion acceptance.

## Update And Recovery Commands

`update-system` stages an ESP32-S3 firmware image from LittleFS for recovery-mode installation.

```text
update-system --list
update-system --dry-run
update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin
update-system --verify /fs/ESPUSER/firmware/app.bin
update-system --prepare-only /fs/ESPUSER/firmware/app.bin
update-system --no-reboot /fs/ESPUSER/firmware/app.bin
```

Behavior:

- Without an explicit file, it searches `/ESPUSER/firmware` first and then the LittleFS fallback.
- `--dry-run` resolves the target without writing a manifest.
- `--verify` hashes the selected image and checks that it fits `app0`.
- `--prepare-only` writes the manifest without selecting or rebooting into recovery.
- `--no-reboot` selects recovery but leaves reboot to the operator.
- Signed manifests require `MROS_UPDATE_MANIFEST_HMAC_KEY` in the firmware build configuration.

Recommended update preflight:

```text
mfetch --full
df -h
ls -l /ESPUSER/firmware
sha256sum /ESPUSER/firmware/app.bin
update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin
```

## Project Command

`project` prints embedded project documentation directly on the device.

```text
project status
project info
project web-page
project goals
project todo
project license
project version
```

This is useful when someone has a live device but not the repository open. It summarizes the bridge architecture, web UI role, MATLAB/reference-model direction, project goals, roadmap, license, and runtime version.

## Robot Command Entry Points

The `robot` family is documented in detail in [Robot And MROS Runtime Guide](robot-mros-runtime-guide.md). The shortest useful reference is:

```text
robot status --json
robot safety status
robot power status
robot telemetry status
robot joint list
robot cartesian status
robot path status
robot math status
robot math backend status
robot math ik preview 300 300 250 --json
robot diag status --json
robot health --json
```

Safety rule: use `preview` commands before `apply` or `run`, and keep motor power, emergency stop, hold state, Teensy link state, and physical clearance separate in your checklist.

## Security And Audit

Use security commands to understand the current session and to collect support evidence.

```text
whoami
id
groups
mros security status
mros security audit
mros audit list
mros audit export
mros report create
mros report list
```

Commands such as `su`, `sudo`, `passwd`, `change`, `mros security logout-all`, and `mros audit clear` can alter security state. Treat them as administrative actions.

## Suggested Bring-Up Script

Run this manually after flashing a new build or after a wiring change:

```text
project version
mfetch --full
status --json
df -h
mount status
wifi info
mros health
mros connections status
mros bus errors
mros spi status all
mros uart status
devices test status
robot safety status
robot status --json
robot diag status --json
```

Record the exact command output when publishing release evidence or reporting a fault. A screenshot of a web page is helpful, but it does not replace shell output for link, heap, partition, update, and safety state.
