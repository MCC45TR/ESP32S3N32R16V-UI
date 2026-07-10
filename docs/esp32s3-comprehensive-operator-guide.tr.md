# ESP32-S3 Kapsamlı Operatör Rehberi

Dil: [English](esp32s3-comprehensive-operator-guide.md) | Türkçe

Bu rehber MROS-DEUSCARA sisteminin ESP32-S3 tarafını profesyonel UI, ağ, recovery, dijital ikiz ve servis katmanı olarak açıklar. HTTP, WebSocket, file manager, `mshell`, `mros` ve robot API haritası için [ESP32-S3 Web UI ve API Operatör Rehberi](web-ui-api-operator-guide.tr.md) kullanılmalıdır.

## Sistem Rolü

`ESP32S3N32R16V-UI` şu işlerden sorumludur:

- Wi-Fi station ve AP/hotspot,
- web UI ve browser tabanlı HMI,
- WebSocket/API telemetry,
- LittleFS file manager ve runtime data store,
- MSHELL benzeri diagnostic yüzey,
- recovery/update workflow,
- digital twin ve robot process planning,
- Teensy UART service link,
- mevcut QSPI isimli bring-up için classic SPI endpoint.

ESP32 recipe stage edebilir, robot state görselleştirebilir, diagnostic isteyebilir ve operator control gösterebilir. Hard realtime motor-control otoritesi olarak görülmemelidir.

## Build Profilleri

| Profil | Amaç |
| --- | --- |
| `s3_mros_hub_n32` | Ana 32 MB flash profili, mevcut varsayılan |
| `s3_mros_hub_n16` | 16 MB flash uyumluluğu |
| `s3_mros_hub_n8` | 8 MB flash uyumluluğu |
| `s3_mros_hub_ddr_n32` | DDR/PSRAM120 varyantı |
| `*_dbg` | Debug profilleri |
| `recovery_s3` | `recovery_app/` altındaki recovery firmware |

Build:

```powershell
pio run -e s3_mros_hub_n32
pio run -e s3_mros_hub_n16
pio run -e s3_mros_hub_n8
pio run -d recovery_app -e recovery_s3
```

Upload ve monitor:

```powershell
pio run -e s3_mros_hub_n32 -t upload
pio device monitor -b 115200
```

## LittleFS Data Model

| Alan | Dosyalar |
| --- | --- |
| Web UI | `index.html`, `setup.html`, `recovery.html`, `css/`, `js/`, `assets/` |
| Robot mechanics | `data/robot/*.csv`, `data/robot/*.json` |
| Materials | `data/materials/*.csv`, `data/materials_manifest.json` |
| User state | `data/ESPUSER/config.json`, loglar, scriptler, update dosyaları |
| Firmware update | `data/ESPUSER/firmware/current/`, `inbox/`, `update.json` |
| Shell config | `data/ESPUSER/mshell_aliases.txt`, `mshell_config.txt`, `pid.cfg` |

Gerçek Wi-Fi credential, private SSH key, production token veya private host bilgisi LittleFS seed data içinde publish edilmemelidir.

## Web ve Dijital İkiz Runtime

Browser katmanı CAD/robot/material sidecar dosyalarını yükler ve şunları sağlar:

- 3D robot viewer,
- material assignment,
- seed mass/COM/inertia estimate,
- bearing load estimate,
- actuator torque margin estimate,
- structural screening,
- process feed planning,
- `force-snapshot.json` export.

Örnek browser helper:

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

Dijital ikiz engineering seed modelidir. Release kararı için CAD mass property, gerçek actuator data, ölçülmüş yükler, FEM/test kanıtı ve Teensy safety acceptance gerekir.

## Shell ve Operatör Yüzeyleri

| Aile | Amaç |
| --- | --- |
| `mros` | Runtime/system diagnostics |
| `mshell` | Local ve remote shell bridge |
| `wifi` | Wi-Fi/AP/station diagnostics |
| `spi`, `uart` | Link diagnostics |
| `robot` | Robot state, health, process ve link testleri |
| `update-system`, `mros7dofs3-update`, `recovery` | Update/recovery workflow |
| `ls`, `cat`, `mount`, `lsblk`, `df` | Filesystem diagnostics |
| `project`, `status`, `debug`, `log`, `ps`, `free` | Runtime inspection |

Önemli diagnostikler:

```text
spi test --duration 10 --json
uart test --duration 10 --json
robot diag linktest uart --duration 10 --json
robot diag linktest qspi --duration 10 --json
robot health --json
update-system status
```

## Kanıt Kapıları

| İddia | Gereken kanıt |
| --- | --- |
| Build çalışıyor | Seçilen env için PlatformIO log |
| Device boot ediyor | Serial boot log ve version |
| Web UI çalışıyor | Browser load, static assets, kritik console hatası yok |
| LittleFS çalışıyor | File list/read/write/update kanıtı |
| UART çalışıyor | `uart test --json` ve wiring notları |
| SPI/QSPI compatibility çalışıyor | `spi test --json` ve Teensy `comm qspi` kanıtı |
| Process planner çalışıyor | CSV validation ve browser helper çıktısı |
| Robot action güvenli | Teensy acceptance, CAN heartbeat, safety gate ve motor-disabled proof |
