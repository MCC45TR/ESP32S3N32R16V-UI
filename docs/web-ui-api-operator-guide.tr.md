# ESP32-S3 Web UI ve API Operatör Rehberi

Dil: [English](web-ui-api-operator-guide.md) | Türkçe

Bu rehber ESP32-S3 web, REST API, WebSocket, file manager, `mshell`, `mros` ve robot-math servis yüzeylerini belgeler. İçerik `src/web/server/web_server.cpp` dosyasından türetilmiştir ve [ESP32-S3 Kapsamlı Operatör Rehberi](esp32s3-comprehensive-operator-guide.tr.md), [ESP32-S3 mshell Komut Referansı](esp32-shell-command-reference.tr.md), [ESP32-S3 MROS Alt Komut Referansı](esp32-mros-subcommand-reference.tr.md) ve [Robot ve MROS Runtime Rehberi](robot-mros-runtime-guide.tr.md) ile birlikte okunmalıdır.

## Sorumluluk Sınırı

ESP32-S3 web katmanı operatör ve servis arayüzüdür. Kullanıcı authenticate edebilir, telemetry sunabilir, LittleFS dosyalarını yönetebilir, shell komutları çalıştırabilir, seçili istekleri Teensy'ye bridge edebilir, robot verisi stage edebilir ve digital twin gösterebilir. Final hard realtime robot-motion otoritesi değildir.

Net dil kullanın:

- Web UI aksiyonu, operatörün bir şeyi istediği veya stage ettiği anlamına gelir.
- API başarısı, ESP32 handler'ın isteği kabul edip işlediği anlamına gelir.
- `mshell`/`mros` çıktısı, shell yüzeyinin cevap verdiği anlamına gelir.
- Robot action, Teensy safety ve motion layer kabul etmeden kanıtlanmış değildir.
- Live robot kanıtı sadece browser cevabı değil, fiziksel kanıt gerektirir.

## Ana Browser Yüzeyleri

| Route | Amaç | Operatör kullanımı |
| --- | --- | --- |
| `/` | Ana HMI ve dashboard | Normal operatör giriş noktası |
| `/login` | Login ekranı | Telemetry veya control öncesi authentication |
| `/setup` | First-run setup | İlk credential ve device profile oluşturma |
| `/debug` | Debug yüzeyi | Mühendislik diagnostikleri |
| `/mcp` | MCP/devtool yüzeyi | Developer-only integration |
| `/main.js`, `/style.css`, `/kinematics3d.js` | Static UI asset'leri | Browser tarafından yüklenir |

Web UI profesyonel servis konsolu olarak anlatılmalıdır: status, robot visualization, file operations, shell bridge ve diagnostics'i birleştirir. Certified safety pendant olarak sunulmamalıdır.

## WebSocket Kanalları

Firmware şu WebSocket path'lerini register eder:

| Kanal | Amaç | Not |
| --- | --- | --- |
| `/ws` | Ana telemetry ve robot/digital-twin delta'ları | Authenticated client'lar state data alır |
| `/ws/telemetry` | Telemetry kanalı | Native primary telemetry route |
| `/ws-shell` | Legacy shell streaming | Compatibility için korunur |
| `/ws/shell` | Güncel shell streaming | Tercih edilen shell WebSocket yolu |
| `/ws/debug` | Debug telemetry | Engineering/debug client'ları |
| `/ws/mcp` | MCP/devtool streaming | Development integration |

Authentication önemlidir. Unauthenticated WebSocket client'ları robot state data almamalıdır. UI akışının beklediği ticket/handshake material için `/api/ws-ticket` kullanılır.

## Authentication ve Session

Önemli route'lar:

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/auth/state` | Güncel auth/setup state |
| `POST` | `/api/register` | İlk kullanıcı kaydı |
| `POST` | `/api/login` | Login ve session oluşturma |
| `POST` | `/api/logout` | Güncel session revoke |
| `GET` | `/api/ws-ticket` | WebSocket auth ticket |
| `GET` | `/api/security/users` | User list/admin view |
| `POST` | `/api/security/users/add` | Kullanıcı ekleme |
| `POST` | `/api/security/users/password` | Password değişimi |
| `POST` | `/api/security/users/delete` | Kullanıcı silme |
| `POST` | `/api/security/sessions/revoke` | Session revoke |
| `POST` | `/api/security/auth-reset` | Auth reset workflow |
| `POST` | `/api/credentials` | Reauth/admin kurallarıyla legacy credential update |

Security kuralları:

- Gerçek Wi-Fi credential, root password, user password, SSH private key, session token, CSRF token veya setup secret publish edilmez.
- Örneklerde bariz placeholder kullanın.
- Shell, file, diagnostics ve robot-related aksiyonlardan önce setup/login gerektiğini açıklayın.
- Auth reset, user deletion, credential update ve session revocation administrative operation'dır.

## Sistem Status ve Health API'leri

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/status` | Loop/auth/serial auth state içeren kompakt status |
| `GET` | `/api/health` | Native health route |
| `GET` | `/api/about` | Project/about metadata |
| `GET` | `/api/assets/manifest` | UI asset manifest |
| `GET` | `/api/services/state` | Service state |
| `POST` | `/api/services/update` | Service settings update |
| `GET` | `/api/devices/status` | T41/C3/SPI/WebSocket device state |
| `POST` | `/api/devices/test` | Device test request |
| `GET` | `/api/c3/status` | C3 peer status |
| `POST` | `/api/c3/reset` | C3 peer reset |
| `GET` | `/api/c3/failsafe` | C3 failsafe state |
| `POST` | `/api/c3/failsafe` | C3 failsafe state değişimi |

Önerilen salt okunur health snapshot:

```text
GET /api/status
GET /api/health
GET /api/devices/status
GET /api/c3/status
GET /api/logs/tail
GET /api/mros/doctor?target=quick
```

## Wi-Fi ve Network API'leri

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/wifi/scan` | Yakındaki ağları tarar |
| `POST` | `/api/wifi/connect` | Ağa bağlanır |
| `POST` | `/api/wifi/save` | Wi-Fi credential kaydeder |
| `POST` | `/api/wifi/action` | Disconnect/retry gibi Wi-Fi aksiyonu |
| `GET` | `/api/wifi/state` | Güncel Wi-Fi state |
| `GET` | `/api/wifi/diag` | Wi-Fi diagnostikleri |

Credential kuralı: örneklerde `YOUR_WIFI_SSID` ve `YOUR_WIFI_PASSWORD` gibi placeholder kullanın. Gerçek credential'lar Git içinde değil cihaz-local storage içinde kalmalıdır.

## Power, DPM, Memory ve Debug

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/dpm/status` | Device process manager status |
| `GET` | `/api/dpm/decision` | Güncel DPM kararı |
| `GET` | `/api/dpm/tasks` | Runtime task özeti |
| `POST` | `/api/dpm/policy` | DPM policy set eder |
| `POST` | `/api/dpm/wake` | Task/service wake |
| `GET` | `/api/dpm/frequency` | Frequency/performance state |
| `GET` | `/api/power/status` | Power state |
| `GET` | `/api/power/locks` | Aktif power lock'lar |
| `POST` | `/api/power/mode` | Power mode değiştirir |
| `GET` | `/api/memory/status` | Memory status |
| `GET` | `/api/memory/sram` | Internal SRAM state |
| `GET` | `/api/memory/leaks` | Leak diagnostikleri |
| `GET` | `/api/debug/sysinfo` | Debug sysinfo |
| `GET` | `/api/debug/web` | Web/debug metrics |
| `GET` | `/api/debug/heap-trace` | Heap trace state |
| `POST` | `/api/debug/heap-trace` | Heap trace start/stop |
| `GET` | `/api/debug/coredump` | Coredump metadata |
| `GET` | `/api/debug/coredump/download` | Coredump indir |
| `DELETE` | `/api/debug/coredump` | Coredump temizle |

Önce read-only route'ları kullanın. Power mode, DPM policy, wake, heap trace, coredump deletion ve recovery reboot operatör aksiyonu olarak loglanmalıdır.

## Shell, MSHELL ve MROS API Bridge

ESP32 hem local shell session API'leri hem de `mshell`/`mros` bridge API'leri sunar.

### Shell Session

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/shell/sessions` | Session listesi |
| `POST` | `/api/shell/sessions` | Session başlatma |
| `DELETE` | `/api/shell/sessions` | Session sil/cancel |
| `GET` | `/api/console` | Console snapshot |
| `GET` | `/api/console/delta` | Console delta |

### MSHELL Bridge

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/mshell/devices` | Remote shell device report |
| `POST` | `/api/mshell/connect` | Bridge target connect |
| `GET` | `/api/mshell/schema` | Shell call schema |
| `POST` | `/api/mshell/call` | Structured `mshell call` request |
| `GET` | `/api/mshell/jobs` | Shell jobs listesi |
| `POST` | `/api/mshell/jobs` | Shell job başlat |
| `DELETE` | `/api/mshell/jobs` | Shell job cancel |
| `GET` | `/api/mshell/tx` | Transaction state |
| `POST` | `/api/mshell/tx` | Begin/stage/commit/rollback transaction |
| `POST` | `/api/settings/uart-shell` | UART shell bridge mode set |

Web server, `mshell` command string kurmadan önce parametreleri doğrular. Dokümantasyon da bu modeli korumalıdır: raw untrusted command string yerine structured parameter ve safe atom kullanın.

### MROS Support API'leri

| Method | Route | Shell karşılığı |
| --- | --- |
| `GET` | `/api/mros/doctor` | `mros doctor <target> --json` |
| `GET` | `/api/mros/report` | `mros report list` veya `mros report show <path>` |
| `POST` | `/api/mros/report` | `mros report create` |
| `DELETE` | `/api/mros/report` | `mros report delete <path>` |
| `GET` | `/api/mros/audit` | `mros audit list` |

Support package için bunları kullanın. Profesyonel bug report `mros doctor quick`, status, logs, memory, power, device state ve varsa report file path içermelidir.

## File Manager ve LittleFS API'leri

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/files/mounts` | Mounted filesystem listesi |
| `POST` | `/api/files/mount` | Target mount/unmount |
| `GET` | `/api/files/list` | Dosya listesi |
| `GET` | `/api/files/info` | File metadata |
| `GET` | `/api/files/download` | Dosya indir |
| `POST` | `/api/files/mkdir` | Dizin oluştur |
| `POST` | `/api/files/delete` | Dosya/dizin sil |
| `POST` | `/api/files/rename` | Dosya rename |
| `POST` | `/api/files/copy` | Dosya kopyala |
| `GET` | `/api/files/fetch/check` | Remote fetch preflight |
| `POST` | `/api/files/fetch/start` | Remote fetch/download başlat |
| `GET` | `/api/files/fetch/status` | Fetch progress |
| `POST` | `/api/files/fetch/cancel` | Fetch cancel |
| `POST` | `/api/files/upload` | Dosya upload |
| `POST` | `/api/files/save` | Dosya içeriği kaydet |

File manager kanıtı path, size, gerekiyorsa checksum ve dosyanın seed/runtime/download/upload/generated evidence sınıfını içermelidir.

Local secret'ları screenshot veya örneklerle expose etmeyin. `/ESPUSER/auth`, credentials dosyaları, Wi-Fi state, setup token'ları, loglar ve downloaded firmware artifact'leri dikkat ister.

## Robot, Math, PID, Trajectory ve Digital Twin API'leri

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/robot/math/onboard` | Onboard math state/config |
| `POST` | `/api/robot/math/onboard` | Onboard math settings update |
| `POST` | `/api/robot/math/onboard/run` | Onboard math operation çalıştır |
| `GET` | `/api/trajectory/preview` | Trajectory preview |
| `GET` | `/api/trajectory/stats` | Trajectory statistics |
| `GET` | `/api/pid` | PID state |
| `POST` | `/api/pid` | PID settings update |
| `GET` | `/api/robot/mechanics/manifest` | Robot mechanics manifest |
| `GET` | `/api/materials/manifest` | Materials manifest |
| `GET` | `/api/cad/manifest` | CAD manifest |
| `GET` | `/api/cad/version` | CAD data version |
| `GET` | `/api/svg` | SVG/visual asset helper |
| `GET` | `/api/pca/cal` | PCA calibration state |
| `POST` | `/api/pca/cal` | Calibration upload/apply |
| `POST` | `/api/pca/cal/reset` | Calibration reset |
| `GET` | `/api/pca/osc` | PCA oscillator setting |
| `POST` | `/api/pca/osc` | PCA oscillator set |
| `POST` | `/api/pca/test` | PCA test |
| `GET` | `/api/turret/output_lock` | Output lock state |
| `POST` | `/api/turret/output_lock` | Output lock değiştir |

Robot API kuralları:

- Runtime setting apply etmeden önce preview/stat endpoint'lerini kullanın.
- PID, oscillator, calibration, output lock ve onboard math değişimleri operatör aksiyonudur.
- Browser/digital-twin verisi Teensy safety, limit, stale-command policy ve motion authority doğrulayana kadar planning evidence'tır.
- Force/process snapshot'ları kullanılan robot-data ve material manifest versiyonlarıyla birlikte saklayın.

## Config, Calibration, Profile, Recovery

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/settings/device` | Device settings |
| `GET` | `/api/settings/schema` | Settings schema |
| `GET` | `/api/settings/popup` | User popup/profile settings |
| `POST` | `/api/profile` | Profile update |
| `GET` | `/api/profile` | Profile oku |
| `GET` | `/api/config/download` | Config indir |
| `POST` | `/api/config/upload` | Config upload |
| `POST` | `/api/calibration/save` | Calibration save |
| `POST` | `/api/system/reboot-recovery` | Recovery'ye reboot |
| `POST` | `/set` | Legacy settings endpoint |

Recovery veya update flow kullanmadan önce current status, firmware profile, LittleFS state ve rollback plan yakalayın.

## Logs, SPI ve Kanıt

| Method | Route | Amaç |
| --- | --- | --- |
| `GET` | `/api/logs` | Log view |
| `GET` | `/api/logs/tail` | Log tail |
| `GET` | `/api/spi/errors` | SPI/T41 error state |

Önerilen evidence bundle:

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

## Operatör İş Akışları

### First Setup

1. `/setup` aç.
2. Placeholder olmayan admin user oluştur.
3. `/login` ile giriş yap.
4. `/api/auth/state` kontrol et.
5. UI veya `/api/wifi/save` ile private device-local credential kullanarak Wi-Fi kaydet.
6. `/api/status`, `/api/health` ve `/api/devices/status` doğrula.

### Bridge Bring-Up

1. Authenticated UI aç.
2. `/api/mshell/devices` kontrol et.
3. Hedef için `/api/mshell/connect` kullan.
4. `/api/settings/uart-shell` kontrol et.
5. `/api/mros/doctor` üzerinden `mros doctor quick` çalıştır.
6. ESP32 bridge state'i Teensy MSHELL diagnostikleriyle karşılaştır.

### Robot Math Review

1. Robot mechanics ve material manifestlerini yükle.
2. Math mode için `/api/robot/math/onboard` kullan.
3. `/api/robot/math/onboard/run` veya shell `robot ... preview` kullan.
4. Process/force snapshot sakla.
5. Teensy'de `robot math validate` ve ilgili FK/IK/Jacobian komutlarıyla eşdeğer matematiği doğrula.

### Support Package

1. Status, health, devices, memory, power, SPI errors ve logs yakala.
2. `/api/mros/doctor?target=quick` çalıştır.
3. `POST /api/mros/report` ile support report oluştur.
4. `GET /api/mros/report` ile report'u listele veya göster.
5. Public issue içinde secret, password, token veya private network detayı paylaşma.

## GitHub İddia Sınırları

Şu ifadeleri tutarlı kullanın:

- "The ESP32-S3 firmware exposes authenticated web UI, REST API, and WebSocket service surfaces."
- "The file manager operates on LittleFS/runtime paths; file presence is not motion proof."
- "`/api/mros/*` endpoints wrap `mros` support commands for diagnostics and reports."
- "`/api/mshell/*` endpoints bridge structured shell work; raw untrusted command strings are not the design contract."
- "Robot math preview and digital-twin output require Teensy safety acceptance before becoming live robot action."

