# Robot Ve MROS Runtime Rehberi

Dil: [English](robot-mros-runtime-guide.md) | Turkish

Bu rehber ESP32-S3 firmware icindeki robot kontrol, robot matematik, tani, saglik verisi ve MROS runtime komutlarini aciklar. Shell referansini tamamlar; odak noktasi gercek operator akislari: incele, preview al, dogrula, uygula, tani koy ve kanit topla.

Tam bridge-level `mros` alt komut katalogu icin [ESP32-S3 MROS Alt Komut Referansi](esp32-mros-subcommand-reference.tr.md) sayfasina bakin.

Teensy ile paylasilan canonical FK/IK/Jacobian/trajectory ve communication timing denklemleri icin [TEENSY41 Robot Iletisim Ve Matematik Denklem Referansi](https://github.com/MCC45TR/TEENSY41-Brain/blob/main/docs/robot-communication-math-equations-reference.tr.md) sayfasina bakin.

## Runtime Sorumluluk Ayrimi

ESP32-S3 firmware bridge ve operator runtime katmanidir. Web UI, komut shell'i, ag servisleri, dosya depolama, telemetry yuzeyleri, update hazirlama ve robot komut framework'unu tasir. Robot aksiyonlarini hesaplayabilir, preview edebilir, serialize edebilir, route edebilir ve talep edebilir.

Teensy 4.1 tarafi canli hareket ve dusuk seviye guvenlik kabulunde deterministik robot-brain siniridir. ESP32-S3 shell komutunun basarili olmasi, bridge'in bir niyeti kabul ettigi veya urettiği anlamina gelir; fiziksel robotun guvenli hareket ettigini tek basina kanitlamaz.

## Guvenlik Modeli

Donanimi hareket ettirebilecek komutlardan once:

1. Durumu oku: `robot status --json`, `robot safety status`, `mros health`.
2. Linkleri kontrol et: `mros connections status`, `mros bus errors`, `robot diag status --json`.
3. Geometriyi kontrol et: `robot math status`, `robot math validate`, `robot math ik preview ...`.
4. Hareketi preview et: `apply`, `run` veya hareket etkili `set` oncesinde `preview` kullan.
5. Fiziksel alani onayla: engel yok, power dogru, hold/emergency durumu dogru.
6. Sadece preview, link durumu ve fiziksel ortam uyumluysa uygula.

Emergency ve hold:

```text
robot safety status
robot safety emg on
robot safety emg off
robot safety hold on
robot safety hold off
robot safety stop
robot safety reset
```

Power:

```text
robot power status
robot power get
robot power set on
robot power set off
```

## Robot Kaynak Haritasi

| Kaynak | Tipik komutlar | Rol |
| --- | --- | --- |
| Safety | `robot safety status`, `emg`, `hold`, `stop`, `reset` | Emergency, hold, stop ve safety reset |
| Power | `robot power status|get|set on|off` | Motor/output power durumu |
| Status | `robot status summary|full --json` | Kisa veya tam robot durumu |
| Telemetry | `robot telemetry status` | Web/PID/FK timing ve feedback |
| Turret | `robot turret status|set <deg>|zero|pid status` | Turret hedefi, gercek deger, hata, PID cikisi |
| Gripper | `robot gripper status|set <pct>|open|close` | Gripper komut yuzeyi |
| Joint | `robot joint status|list|set|apply|home|park|zero|jog` | Joint-space durum ve hareket talebi |
| Cartesian | `robot cartesian status|set|preview|apply <x y z> ...` | Cartesian hedef preview veya uygulama |
| Move | `robot move <x y z> ...`, `robot move preview|apply --from ... --to ...` | Basit point-to-point yardimcisi |
| Path | `robot path status|list|add|insert|remove|clear|preview|run|export|import` | Cok noktali path queue |
| Motion block | `robot motion block list|add|clear|preview|compile|apply` | Ust seviye motion planlama |
| Profile | `robot profile list|status|set|save|delete` | Runtime robot profili |
| Model | `robot model list|status|set|describe` | Robot modeli |
| Frame | `robot frame list|status|set|define` | Koordinat frame secimi |
| Limits | `robot limits list|status|set|reset` | Runtime limit profili |
| Math | `robot math ...` | FK, IK, Jacobian, backend, solver, trajectory, tuning |
| Calibration | `robot calibration servo|encoder|pca ...` | Kalibrasyon araclari |
| Diagnostics | `robot diag status|errors|links|console|t41|pca|linktest ...` | Link, kart ve robot tanilari |
| Health | `robot health`, `robot actuator`, `robot structural`, `robot bearing` | Runtime saglik veri setleri ve tablolar |

Hizli konsol icin alias komutlari vardir:

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

Dokumantasyon ve otomasyon icin alias yerine canonical kaynak adlarini tercih edin.

## Robot Matematik Mimarisi

Matematik katmani bridge, browser, Teensy ve gelecekteki otomasyon istemcilerini tek komut sozlugu etrafinda hizalar.

Durum komutlari:

```text
robot math list
robot math status
robot math validate
robot math benchmark
robot math explain
```

Backend secimi:

```text
robot math backend list
robot math backend status
robot math backend set auto
robot math backend set web
robot math backend set onboard-s3
robot math backend set t41-qspi
robot math backend set t41-esp-now
```

Backend anlamlari:

| Backend | Anlam |
| --- | --- |
| `auto` | Link ve runtime durumuna gore en iyi backend'i secer |
| `web` | Browser/web tarafli solver yolu; UI preview ve gorsel feedback icin |
| `onboard-s3` | Uygulanan yerlerde ESP32-S3 lokal matematik yolu |
| `t41-qspi` | Primary high-speed bridge uzerinden Teensy backend |
| `t41-esp-now` | Failsafe ESP-NOW konsepti uzerinden Teensy backend |

Solver ve model:

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

Planlama ve tuning:

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

FK ve IK:

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

Trajectory ve PID:

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

## Cartesian Ve Joint Hareket

Joint-space:

```text
robot joint status
robot joint list
robot joint set 0 0 0 0 0 0 0
robot joint apply 0 0 0 0 0 0 0
robot joint home
robot joint park
robot joint zero
```

Cartesian preview:

```text
robot cartesian status
robot cartesian preview 300 300 250 --roll 0 --pitch auto --yaw 0 --json
robot cartesian apply 300 300 250 --roll 0 --pitch auto --yaw 0
robot move preview --from 250 250 200 --to 300 300 250 --speed 0.5
robot move apply --from 250 250 200 --to 300 300 250 --speed 0.5
```

Model, frame, backend, solver, trajectory ve workspace degisikliklerini test ederken `preview` kullanin. `apply` sadece safety durumu ve fiziksel bosluk biliniyorsa kullanilmalidir.

## Path Ve Motion Block

Path queue cok noktali rota icin kullanilir. Acikca calistirilana kadar plan olarak ele alinmalidir.

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

Motion block:

```text
robot motion block list
robot motion block add
robot motion block preview
robot motion block compile
robot motion block apply
robot motion block clear
```

Operator politikasi:

- Rota dogrularken `preview` ciktisini release/test kanitiyla saklayin.
- Model, frame, limit, backend veya tuning degistiyse eski path'i preview etmeden kullanmayin.
- Import edilen path'ler parse, preview ve aktif limit profiline gore kontrol edilene kadar guvenilmez kabul edilmelidir.

## Tanilar

Robot tanilari:

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

MROS bridge tanilari:

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

Cikti dashboard, issue template veya otomatik rapora gidecekse `--json` kullanin.

## Saglik Verisi

ESP32-S3 firmware robot, actuator, structural ve bearing saglik veri setlerini runtime'da tutabilir.

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

Saglik verisi web UI, dosya, script veya runtime araclarindan gelebilir. Saglik snapshot'lari muhendislik kaniti olarak kullanilacaksa kaynak ve TTL mutlaka kaydedilmelidir.

## MROS Runtime Komutlari

`mros`, bridge seviyesindeki komut grubudur. `robot` komutundan daha genistir: health, connections, bus, log, memory, security, report, power mode ve destek paketlerini kapsar.

Yuksek degerli komutlar:

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

Power mode anlamlari:

| Mode | Kullanim |
| --- | --- |
| `cool` | Termal veya idle dostu calisma |
| `balanced` | Genel servis kullanimi |
| `performance` | Agir tani veya web/UI yuku |
| `motion-safe` | Harekete hassas runtime profili |
| `update-safe` | Firmware/update hazirlama profili |

## Kanit Receteleri

Hareketten once:

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

Cartesian hedef uygulamadan once:

```text
robot math backend status
robot math solver list
robot math ik preview 300 300 250 --json
robot cartesian preview 300 300 250 --pitch auto --json
robot safety status
```

Link hatasindan sonra:

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

Update oncesi:

```text
mros power mode update-safe
mfetch --full
df -h
sha256sum /ESPUSER/firmware/app.bin
update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin
mros report create
```

## Yayinlama Rehberi

GitHub release notlari, test raporlari veya issue'larda su iddialari ayri tutun:

- build basarili,
- firmware upload edildi,
- shell cevap verdi,
- web UI yuklendi,
- link tanilari gecti,
- robot komut preview basarili,
- motion komutu kabul edildi,
- fiziksel robot davranisi gozlemlendi.

Bunlari tek cumleye sikistirmayin. Profesyonel bir robotik deposu, her kanit kapisini adiyla ve onu ureten komutla gosterdiginde daha guvenilir olur.
