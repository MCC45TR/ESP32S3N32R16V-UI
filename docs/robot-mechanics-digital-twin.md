# MROS Robot Mechanics Digital Twin

This document describes the seed mechanics layer used by the ESP32-S3 web CAD viewer.

## Data Model

The mechanics database is stored under `data/` so it can be versioned and shipped in the LittleFS image without recompiling C++ constants.

- `data/materials/materials_index.csv`: searchable material identity, family, alloy/designation, temper and source.
- `data/materials/materials_mechanical.csv`: density, elastic constants, yield/ultimate/shear/fatigue seed values, hardness and thermal properties.
- `data/materials/material_aliases.csv`: user-facing aliases such as `AL7075`, `STEEL4140`, `ST37`, `S235JR`.
- `data/robot/part_material_assignments.csv`: CAD part to material and link-frame mapping.
- `data/robot/part_mass_properties.csv`: authoritative CAD mass, COM and inertia values when final CAD is available.
- `data/robot/bearing_mounts.csv`: bearing center points in their owning joint/link local frame, not global coordinates.
- `data/robot/force_snapshot.example.json`: combined browser runtime-load snapshot for `robot health load-json`.
- `data/robot/link_frames.csv`: named robot frames used by part and bearing rows.
- `data/robot/joint_actuator_profiles.csv`: reducer ratio, nominal motor torque and nominal output torque limits per joint.
- `data/robot/atar_m_series_actuator_specs.csv`: AtarRobot M-series actuator manufacturer data and derived calculation fields for selected PRR SCARA axes.
- `data/robot/prr_scara_axis_map.csv`: selected PRR kinematic order and axis-to-actuator mapping for the current 3-axis robot.
- `data/robot/prr_scara_geometry_seed.csv`: measured/TBD geometry, TCP offsets, soft limits and singularity seed values for the PRR solver.
- `data/robot/canopen_actuator_bus_map.csv`: seed CANopen node, axis and DB9 CAN wiring map.
- `data/robot/robot_process_feed_modes.csv`: seed welding, plasma, oxy-fuel and dry-run feed-mode data for planning.
- `data/robot/robot_process_recipe_templates.csv`: process recipe templates that bind feed mode, material range, TCP profile and vendor/WPS requirements.
- `data/robot/robot_process_interlocks.csv`: process energy and motion interlock gates owned by Teensy, ESP32 or operator.
- `data/robot/robot_process_validation_matrix.csv`: software, HIL and coupon-test validation gates before release.
- `data/robot/robot_data_sources.csv`: source registry for actuator, process and safety data.
- `data/robot/process_recipe.example.json`: example process recipe payload shape.
- `data/robot/actuator_derating_profiles.csv`: seed motor/drive current, temperature, voltage and thermal foldback values per joint.
- `data/robot/structural_design_policies.csv`: seed family-level design factors for yield, shear, ultimate, fatigue, data knockdown and temperature derating.
- `data/robot/payload_profiles.csv`: optional tool/payload mass and COM definitions attached to `J7_TOOL`.
- `data/robot/analysis_todo.json`: release-visible engineering warnings to revisit when CAD is final.

## Runtime Flow

On web CAD startup, `web_kinematics3d.h` loads the CSV/JSON sidecars from:

- `/materials/...`
- `/robot-data/...`

The viewer attaches material metadata to each CAD part, estimates mesh volume from loaded CAD geometry, combines it with material density, and produces a seed force snapshot:

- total mass and weight,
- per-part gravity force and moment about base,
- local-frame bearing centers transformed into current browser world coordinates,
- bearing-pair static equilibrium estimates from downstream part forces and moment arms.

For each `bearing_spacing_pair_id`, the browser computes a support pair center, bearing axis, spacing, downstream frame-subtree resultant force, and moment about the bearing pair center. The force is decomposed into axial and radial components. The perpendicular moment is converted into an additional couple load using `M_perpendicular / spacing`, then assigned to each bearing in the pair. This is still a seed static model, but it is structurally closer to real support loading than equal-share mass division.

The browser also keeps a finite-difference kinematic state for each part COM. On each pose update it estimates COM velocity and filtered acceleration, then uses `support_load = mass * (gravity - COM acceleration)` before the bearing-pair equilibrium step. This is a seed dynamic model; final release dynamics should replace it with joint velocity/acceleration, motor torque, reducer efficiency, payload inertia, and measured external forces.

The same force snapshot now runs a part-level structural screening pass. For every CAD part with material strength and a computed CAD bounding box, the browser estimates a conservative rectangular-section area and minimum section modulus, computes force and bending moment about the owning link-frame origin, then reports axial stress, bending stress, shear stress, a von-Mises-style combined stress, raw material safety factors, and policy-adjusted design safety factors in `structuralChecks[]`.

`structural_design_policies.csv` maps material families such as aluminum, structural steel, carbon steel, stainless, titanium and polymers to seed design factors. The screening applies static yield, shear, ultimate and fatigue factors, a seed-data knockdown, and a temperature derating derived from the highest available actuator telemetry temperature. Fatigue is still approximate: it uses the browser's current dynamic/static stress estimate as an early alternating-stress proxy. `worstStructuralStatus` is `OK`, `LOW_MARGIN`, `FATIGUE_WATCH`, `WATCH`, `CRITICAL`, or `DATA_MISSING`. This is only an early design filter; real signoff still requires final CAD mass properties, actual cross sections, boundary conditions, fasteners, contact loads, fatigue duty cycles, stress concentration factors, and FEM/test validation.

Payload profiles from `payload_profiles.csv` are included as a virtual part attached to `J7_TOOL`. The active payload defaults to `none`, is stored as `mros_kin3d_active_payload`, and can be changed from the browser console or future UI with `window.kin3d_setActivePayload('tool_light')`.

Joint actuator profiles from `joint_actuator_profiles.csv` define reducer ratio, efficiency, continuous output torque, peak output torque, and optional brake torque. `actuator_derating_profiles.csv` adds motor torque constant, continuous/peak current, bus-voltage range, motor temperature limits, controller temperature limits, minimum thermal factor, reducer hot-efficiency factor, and seed thermal constants.

The PRR/process data is loaded by the browser mechanics sidecar as well. The console helper `window.kin3d_setActiveProcessMode(mode_id)` stores the active mode in `localStorage`, and `window.kin3d_computeProcessFeedPlan({...})` returns a bounded process feed plan. This is a planning helper only: welding modes still require a WPS, plasma modes require the plasma cutter's own cut chart, and oxy-fuel modes require the exact torch/tip/gas chart plus external gas-safety signoff.

The browser computes downstream force moments about each link-frame joint axis and reports `jointLoads[]` with:

- `axis_torque_nm`,
- derated `continuous_limit_nm` and `peak_limit_nm`,
- nominal `nominal_continuous_limit_nm` and `nominal_peak_limit_nm`,
- current-limited torque estimates,
- `actuator_derating` with motor/controller temperature factors, bus-voltage factor, reducer hot factor, and source.

Runtime actuator telemetry can be injected from the browser with:

```js
window.kin3d_setActuatorTelemetry('J2', {
  motor_temp_c: 88,
  controller_temp_c: 72,
  bus_voltage_v: 46.5,
  phase_current_a: 11.2,
  source: 'bench_test'
});
```

Telemetry is stored in `localStorage` as `mros_kin3d_actuator_telemetry`; clear one joint with `window.kin3d_clearActuatorTelemetry('J2')`. Without runtime telemetry the derating engine uses the CSV seed limits at nominal factors. These values are still seed engineering checks; final actuator signoff must use verified motor speed-torque curves, measured reducer efficiency maps, acceleration limits, controller current foldback, thermal soak data, and external load tests.

Each loaded CAD part can be right-clicked in the 3D viewer and assigned a material from the material database. These browser-side selections are runtime overrides stored in `localStorage` as `mros_kin3d_material_overrides`; they immediately update the seed mass and force snapshot. The CSV files remain the authoritative design defaults, so approved overrides should later be copied into `part_material_assignments.csv`.

The current browser surface is intentionally named as a seed mechanics snapshot. Final force distribution must use joint equilibrium, acceleration, payload, reducer torque, external loads and verified CAD mass properties before release decisions.

The firmware bearing-health task can consume runtime bearing loads through `robot bearing load ...` or `robot bearing load-json ...`. This is the bridge point for the web/CAD force snapshot, future sensors, and final dynamics solver. Runtime load inputs expire by TTL and are visible in `robot health` as `Src`, `P(N)`, and JSON `load_source`. The browser export function `window.kin3d_exportRobotRuntimeLoadsJson()` emits radial/axial bearing loads, actuator `jointLoads[]`, and browser-side `structuralChecks[]`. `window.kin3d_downloadRobotRuntimeLoadsJson()` downloads the same payload as `force-snapshot.json`. `window.kin3d_exportBearingLoadsJson()` remains as a compatibility alias.

The same exported JSON includes `jointLoads[]`, `worstJointStatus`, `structuralChecks[]`, and `worstStructuralStatus`. Firmware can consume the complete browser snapshot in one step:

```text
robot health load-json /ESPUSER/robot/force-snapshot.json 5000
robot health
robot health --json
```

For subsystem-level debugging, the same file can still be routed manually:

```text
robot bearing load-json /ESPUSER/robot/force-snapshot.json
robot actuator load-json /ESPUSER/robot/force-snapshot.json 2000
robot structural load-json /ESPUSER/robot/force-snapshot.json 5000
robot bearing table
robot actuator table
robot structural table
robot bearing json
robot actuator json
robot structural json
```

`robot health` prints bearing life first, actuator torque margins second, and structural screening third. `robot health --json` returns `{ "bearing": ..., "actuator": ..., "structural": ... }`. Actuator and structural rows are TTL based; stale rows remain visible as `STALE` so a missing web/CAD update is distinguishable from a healthy margin.

## Coordinate Contract

Bearing rows are always local to the owning link frame. For example `local_z_mm = -30` means Z -30 in that joint/link frame, not global robot/world Z. This is required so the same bearing definition remains valid while the robot moves.

## Quality Notes

The material library and structural policies are engineering seed sets, not a certified design-allowables database. Values must be replaced or source-upgraded with supplier certificates, standards, or internal test data before final structural signoff.

When the robot CAD is complete, update `part_mass_properties.csv` from CAD-exported mass properties and mark the matching rows as verified. The browser will prefer explicit CSV mass properties over mesh-density estimates.
