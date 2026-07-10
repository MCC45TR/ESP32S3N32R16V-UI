# MROS ESP32-S3 Bearing Health SKF Config

The ESP32-S3 bearing health subsystem reads SKF bearing specifications from:

```text
/ESPUSER/bearing_health/bearings_skf.json
```

Runtime damage counters are checkpointed to:

```text
/ESPUSER/bearing_health/runtime.json
```

The firmware also writes an example file on boot when LittleFS is mounted:

```text
/ESPUSER/bearing_health/bearings_skf.example.json
```

## Commands

```text
robot health
robot health --json
robot bearing table
robot bearing json
robot bearing reload
robot bearing import /ESPUSER/path/to/skf-bearings.json
robot bearing reset-runtime
robot bearing load <placement> <radial_N> <axial_N> [equivalent_N] [source] [ttl_ms]
robot bearing clear-load <placement>
robot health load-json /ESPUSER/path/to/force-snapshot.json 5000
robot bearing load-json /ESPUSER/path/to/bearing-loads.json
robot actuator load-json /ESPUSER/path/to/force-snapshot.json 2000
robot actuator table
robot structural load-json /ESPUSER/path/to/force-snapshot.json 5000
robot structural table
robot bearing config
mros health
```

## SKF Data Policy

Every bearing spec must be sourced from SKF. The firmware rejects specs unless:

- `manufacturer` is `SKF`
- `source_url` contains `skf.com`
- `C_N` is greater than zero
- `fatigue_exponent_p` is greater than zero

Use SKF catalog or product-page values for dimensions, load ratings, speed limits, and load factors.

## Schema

```json
{
  "schema": "mros.bearing_health.v1",
  "specs": [
    {
      "id": "skf-6206-2rs1",
      "manufacturer": "SKF",
      "designation": "6206-2RS1",
      "type": "deep_groove_ball",
      "d_mm": 30,
      "D_mm": 62,
      "B_mm": 16,
      "C_N": 0,
      "C0_N": 0,
      "Pu_N": 0,
      "fatigue_exponent_p": 3,
      "aSKF": 1,
      "X": 1,
      "Y": 0,
      "reference_speed_rpm": 0,
      "limiting_speed_rpm": 0,
      "source_url": "https://www.skf.com/...",
      "source_date": "2026-06-19"
    }
  ],
  "placements": [
    {
      "id": "J1-BRG-A",
      "joint_id": "J1",
      "joint_name": "Base",
      "joint_index": 0,
      "spec_id": "skf-6206-2rs1",
      "count": 1,
      "rotation_ratio_to_joint": 1,
      "load_share_factor": 1,
      "nominal_radial_load_N": 0,
      "nominal_axial_load_N": 0
    }
  ]
}
```

`joint_index` mapping:

```text
0 = turret/base
1..6 = spi_s3_get_joint_deg(index - 1)
```

## Calculation Notes

The first implementation uses ISO/SKF-style rating life with Palmgren-Miner damage accumulation:

```text
L10_rev = 1_000_000 * (C / P)^p
modified_rev = aSKF * L10_rev
damage += delta_bearing_rev / modified_rev
```

At 50 Hz the task samples two joint positions, computes the shortest angular delta, converts it into bearing revolutions using `rotation_ratio_to_joint`, and updates total revolutions, damage percentage, average speed, and remaining life hours.

## Runtime Load Bridge

The life model can accept temporary runtime loads from CAD/dynamics, force sensors, or manual tests. Runtime loads are not persisted as design truth; they expire with a TTL and then the placement falls back to the SKF config's nominal load values.

Manual example:

```text
robot bearing load J1-BRG-A 120 20 0 cad_static_seed 2000
```

JSON batch example:

```text
robot bearing load-json /ESPUSER/bearing_health/bearing-loads.json
```

Accepted JSON shapes are either `rows[]` with `placement`, `radial_load_N`, `axial_load_N`, optional `equivalent_load_N`, `source`, `ttl_ms`, or the web force snapshot style `bearingLoads[]` using `bearing_mount_id` and `estimated_static_load_n`.

The preferred browser bridge is the combined runtime snapshot:

```text
robot health load-json /ESPUSER/robot/force-snapshot.json 5000
```

This single command applies `bearingLoads[]`, `jointLoads[]`, and `structuralChecks[]` to the bearing, actuator, and structural sections of `robot health`. Use the subsystem `load-json` commands only when debugging one section in isolation.

Future joint load estimation can feed the existing `nominal_radial_load_N`, `nominal_axial_load_N`, `X`, `Y`, and `equivalent_load_N` path without changing shell commands or runtime storage.
