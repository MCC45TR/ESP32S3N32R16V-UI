# Robot And MROS Runtime Guide

Language: English | [Turkish](robot-mros-runtime-guide.tr.md)

This guide explains how the ESP32-S3 firmware exposes robot control, robot math, diagnostics, health data, and MROS runtime operations. It complements the shell reference by focusing on real operator workflows: inspect, preview, validate, apply, diagnose, and collect evidence.

For the complete bridge-level `mros` subcommand catalog, see [ESP32-S3 MROS Subcommand Reference](esp32-mros-subcommand-reference.md).

For the canonical FK/IK/Jacobian/trajectory and communication timing equations shared with Teensy, see [TEENSY41 Robot Communication And Math Equations Reference](https://github.com/MCC45TR/TEENSY41-Brain/blob/main/docs/robot-communication-math-equations-reference.md).

## Runtime Responsibility Split

The ESP32-S3 firmware is the bridge and operator runtime. It hosts the web UI, command shell, network services, file storage, telemetry surfaces, update staging, and robot command framework. It can calculate, preview, serialize, route, and request robot actions.

The Teensy 4.1 side remains the deterministic robot-brain boundary for live motion and low-level safety acceptance. A successful ESP32-S3 shell command means the bridge accepted or produced an intent; it does not by itself prove that the physical robot moved safely.

## Safety Model

Use this sequence before any command that can move hardware:

1. Read state: `robot status --json`, `robot safety status`, `mros health`.
2. Check links: `mros connections status`, `mros bus errors`, `robot diag status --json`.
3. Check geometry: `robot math status`, `robot math validate`, `robot math ik preview ...`.
4. Preview the motion: use `preview` verbs before `apply`, `run`, or `set` with motion effect.
5. Confirm the physical envelope: no obstruction, correct power, correct hold/emergency state.
6. Apply only after the preview, link state, and physical environment agree.

Emergency and hold commands:

```text
robot safety status
robot safety emg on
robot safety emg off
robot safety hold on
robot safety hold off
robot safety stop
robot safety reset
```

Power commands:

```text
robot power status
robot power get
robot power set on
robot power set off
```

## Robot Resource Map

| Resource | Typical commands | Role |
| --- | --- | --- |
| Safety | `robot safety status`, `emg`, `hold`, `stop`, `reset` | Emergency, hold, stop, and safety reset flow |
| Power | `robot power status|get|set on|off` | Motor/output power state |
| Status | `robot status summary|full --json` | Compact or full robot state |
| Telemetry | `robot telemetry status` | Web/PID/FK timing and feedback state |
| Turret | `robot turret status|set <deg>|zero|pid status` | Turret target, actual, error, PID output |
| Gripper | `robot gripper status|set <pct>|open|close` | Gripper command surface |
| Joint | `robot joint status|list|set|apply|home|park|zero|jog` | Joint-space state and motion requests |
| Cartesian | `robot cartesian status|set|preview|apply <x y z> ...` | Cartesian target preview or application |
| Move | `robot move <x y z> ...`, `robot move preview|apply --from ... --to ...` | Simple point-to-point motion helper |
| Path | `robot path status|list|add|insert|remove|clear|preview|run|export|import` | Multi-point path queue |
| Motion block | `robot motion block list|add|clear|preview|compile|apply` | Higher-level motion block planning |
| Profile | `robot profile list|status|set|save|delete` | Runtime robot profile selection |
| Model | `robot model list|status|set|describe` | Robot model selection |
| Frame | `robot frame list|status|set|define` | Coordinate frame selection |
| Limits | `robot limits list|status|set|reset` | Runtime limit profile |
| Math | `robot math ...` | FK, IK, Jacobian, backend, solver, trajectory, tuning |
| Calibration | `robot calibration servo|encoder|pca ...` | Calibration utilities |
| Diagnostics | `robot diag status|errors|links|console|t41|pca|linktest ...` | Link, board, and robot diagnostics |
| Health | `robot health`, `robot actuator`, `robot structural`, `robot bearing` | Runtime health datasets and computed tables |

Alias commands exist for fast console use:

```text
robot pos ...
robot mov ...
robot calc ...
robot change ...
robot speed ...
robot emg ...
robot home
robot park
```

For documentation and automation, prefer canonical resource names over aliases.

## Robot Math Architecture

The math layer keeps the bridge, browser, Teensy, and future automation clients aligned around one command vocabulary.

Core status commands:

```text
robot math list
robot math status
robot math validate
robot math benchmark
robot math explain
```

Backend selection:

```text
robot math backend list
robot math backend status
robot math backend set auto
robot math backend set web
robot math backend set onboard-s3
robot math backend set t41-qspi
robot math backend set t41-esp-now
```

Backend meaning:

| Backend | Meaning |
| --- | --- |
| `auto` | Select the best available backend from link and runtime state |
| `web` | Browser/web-side solver path, useful for UI preview and visual feedback |
| `onboard-s3` | ESP32-S3 local math path where implemented |
| `t41-qspi` | Teensy-side backend through the primary high-speed bridge |
| `t41-esp-now` | Teensy-side backend through the failsafe ESP-NOW concept |

Solver and model selection:

```text
robot math solver list
robot math solver set dls
robot math solver set qp
robot math solver set svd-robust
robot math jacobian list
robot math jacobian set numerical
robot math jacobian set geometric
robot math jacobian set spatial
robot math nullspace list
robot math nullspace set joint_centering
robot math nullspace set off
robot math model list
robot math model status
robot math model set mros-7dof-v1
robot math frame list
robot math frame set base
robot math units set mm
```

Planning and tuning:

```text
robot math planner status
robot math planner set path ground
robot math planner set path elevated
robot math planner set turret auto_shortest
robot math planner set ground-z 0
robot math planner set cart-step 8
robot math planner set yaw-step 4
robot math planner set jump 18
robot math planner set negative-z off
robot math tuning status
robot math tuning set pos_tol_mm 0.5
robot math tuning set ori_tol_deg 0.5
robot math tuning set lambda_max 0.5
robot math singularity status
robot math singularity set 5
```

FK and IK:

```text
robot math fk status
robot math fk solve 0 0 0 0 0 0 0 --json
robot math fk preview 0 10 20 30 40 50 60
robot math fk compare
robot math ik status
robot math ik preview 300 300 250 --json
robot math ik solve 300 300 250
robot math ik apply 300 300 250
robot math ik seed list
robot math ik seed set current
```

Trajectory and PID:

```text
robot math trajectory list
robot math trajectory status
robot math trajectory set quintic
robot math trajectory set scurve
robot math trajectory preview
robot math pid status
robot math pid set 1.0 0.0 0.1 10.0
robot math pid profile set safe
robot math pid reset
```

## Cartesian And Joint Motion

Joint-space inspection:

```text
robot joint status
robot joint list
robot joint set 0 0 0 0 0 0 0
robot joint apply 0 0 0 0 0 0 0
robot joint home
robot joint park
robot joint zero
```

Cartesian inspection and preview:

```text
robot cartesian status
robot cartesian preview 300 300 250 --roll 0 --pitch auto --yaw 0 --json
robot cartesian apply 300 300 250 --roll 0 --pitch auto --yaw 0
robot move preview --from 250 250 200 --to 300 300 250 --speed 0.5
robot move apply --from 250 250 200 --to 300 300 250 --speed 0.5
```

Use `preview` when testing model, frame, backend, solver, trajectory, and workspace changes. Use `apply` only after safety state and physical clearance are known.

## Path And Motion Blocks

Path queues are useful for multi-point routes. They should be treated as plans until explicitly run.

```text
robot path status
robot path list
robot path add 300 300 250 1000
robot path insert 0 280 250 220 800
robot path preview
robot path export
robot path import
robot path run
robot path clear
```

Motion blocks are higher-level planning units:

```text
robot motion block list
robot motion block add
robot motion block preview
robot motion block compile
robot motion block apply
robot motion block clear
```

Operator policy:

- Keep `preview` output with the release/test evidence when validating a route.
- Do not reuse a path after changing model, frame, limits, backend, or math tuning without previewing it again.
- Treat imported paths as untrusted until parsed, previewed, and checked against the active limits profile.

## Diagnostics

Robot diagnostics:

```text
robot diag status
robot diag status --json
robot diag errors
robot diag links
robot diag console
robot diag t41
robot diag pca
robot diag linktest uart --duration 10 --json
robot diag linktest s3-uart --duration 10 --json
robot diag linktest qspi --duration 10 --json
robot diag linktest all --duration 10 --json
```

MROS bridge diagnostics:

```text
mros health
mros connections status
mros connections tree
mros bus summary
mros bus errors
mros spi status all
mros uart status
mros i2c status
mros pca9685 status
mros perf status
mros rtos status
mros wifi diag
mros pid status
mros fk status
mros ik status
```

Use `--json` when the output will feed a dashboard, issue template, or automated report.

## Health Data

The ESP32-S3 firmware can hold runtime health datasets for the robot, actuators, structure, and bearings.

```text
robot health --json
robot health load-json /ESPUSER/robot/health.json 5000
robot actuator table
robot actuator json
robot actuator load-json /ESPUSER/robot/actuator_health.json 5000
robot actuator clear
robot structural table
robot structural json
robot structural load-json /ESPUSER/robot/structural_health.json 5000
robot structural clear
robot bearing table
robot bearing json
robot bearing reload
robot bearing import /ESPUSER/robot/bearings.json
robot bearing reset-runtime
robot bearing load base 120 30 130 measured 5000
robot bearing clear-load base
robot bearing load-json /ESPUSER/robot/bearing_loads.json
robot bearing config
```

Health data can come from the web UI, files, scripts, or runtime tooling. Always record the source and TTL when using loaded health snapshots as engineering evidence.

## MROS Runtime Commands

`mros` is the bridge-level command group. It is broader than `robot`: it covers health, connections, buses, logs, memory, security, reports, power mode, and support bundles.

High-value commands:

```text
mros overview
mros health
mros mem status --detail
mros mem snapshot before-test
mros mem diff
mros power mode motion-safe
mros power mode update-safe
mros report create
mros report list
mros audit list
mros security status
mros export diagnostics
mros diag bundle
mros downloader URL /ESPUSER/downloads/file.bin
```

Use power modes intentionally:

| Mode | Use |
| --- | --- |
| `cool` | Thermal or idle-friendly operation |
| `balanced` | General service use |
| `performance` | Heavy diagnostics or web/UI load |
| `motion-safe` | Motion-sensitive runtime profile |
| `update-safe` | Firmware/update staging profile |

## Evidence Recipes

Before motion:

```text
project version
mros health
mros connections status
mros bus errors
robot safety status
robot status --json
robot math status --json
robot diag status --json
```

Before applying a Cartesian target:

```text
robot math backend status
robot math solver list
robot math ik preview 300 300 250 --json
robot cartesian preview 300 300 250 --pitch auto --json
robot safety status
```

After a link fault:

```text
mros health
mros connections tree
mros spi status all
mros uart status
mros bus errors
robot diag links
robot diag errors
mros report create
```

Before an update:

```text
mros power mode update-safe
mfetch --full
df -h
sha256sum /ESPUSER/firmware/app.bin
update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin
mros report create
```

## Publishing Guidance

For GitHub release notes, test reports, or issues, separate these claims:

- build passed,
- firmware uploaded,
- shell responded,
- web UI loaded,
- link diagnostics passed,
- robot command preview succeeded,
- motion command was accepted,
- physical robot behavior was observed.

Do not compress them into one sentence. A professional robotics repository is more trustworthy when each proof gate is named and shown with the command that produced it.
