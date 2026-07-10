# ESP32S3N32R16V-UI Dokümantasyonu

Dil: [English](README.md) | Türkçe

Bu klasör MROS-DEUSCARA sisteminin ESP32-S3 tarafını belgeler. Yeni canonical sayfalarda varsayılan dil İngilizcedir. Her eşleşen dosyanın en üstünde Türkçe karşılık bağlantısı bulunur.

## Canonical Türkçe Dokümanlar

| Doküman | Amaç |
| --- | --- |
| `esp32s3-comprehensive-operator-guide.tr.md` | ESP32-S3 firmware genel bakış, build profilleri, LittleFS, web UI, shell, update, security ve kanıt kapıları |
| `web-ui-api-operator-guide.tr.md` | Web UI, REST API, WebSocket, file manager, mshell/mros bridge, robot math API ve kanıt workflow'u için kaynak tabanlı rehber |
| `esp32-shell-command-reference.tr.md` | ESP32-S3 `mshell` komut aileleri, Wi-Fi, dosya sistemi, tanı, update, API wrapper ve kanıt reçeteleri |
| `esp32-mros-subcommand-reference.tr.md` | Health, bellek, power, bus, rapor, security ve support evidence için ayrıntılı `mros` bridge-level alt komut referansı |
| `robot-mros-runtime-guide.tr.md` | Robot komut framework'ü, MROS runtime, güvenlik akışı, matematik backend'i, path planlama, health data ve tanılar |
| `teensy-link-runtime-guide.tr.md` | ESP32-S3 ile Teensy UART, SPI/QSPI compatibility, robot runtime, dijital ikiz ve process-data köprüsü |
| [TEENSY41 robot denklemleri](https://github.com/MCC45TR/TEENSY41-Brain/blob/main/docs/robot-communication-math-equations-reference.tr.md) | Cross-system communication ve robot matematik denklemleri için canonical referans |
| [TEENSY41 MATLAB/Simulink manual'i](https://github.com/MCC45TR/TEENSY41-Brain/blob/main/docs/matlab-simulink-operator-manual.tr.md) | MATLAB/Simulink USB RT bridge, state/health vector, timing ve deney kanıtları için kaynak tabanlı manual |

## ESP32-S3 Rol Sınırı

ESP32-S3 operatör ve servis controller'dır:

- web UI ve HMI,
- Wi-Fi station/AP ve local web service,
- WebSocket/API telemetry ve command transport,
- LittleFS file manager ve robot data store,
- recovery/update workflow,
- digital twin ve process planning,
- Teensy UART service bridge ve SPI/QSPI peer endpoint.

Final robot-motion safety otoritesi değildir. Canlı hareket ve motor komutlarını deterministik olarak kabul veya reddetme yetkisi Teensy tarafındadır.

## Build Kısa Liste

```powershell
pio run -e s3_mros_hub_n32
pio run -e s3_mros_hub_n16
pio run -e s3_mros_hub_n8
pio run -d recovery_app -e recovery_s3
pio run -e s3_mros_hub_n32 -t upload
pio device monitor -b 115200
```

Robot process data validasyonu:

```powershell
.\tools\validate_robot_process_data.ps1
```

ESP32 build başarısı canlı robot kanıtı değildir. Web state, LittleFS dosyaları ve process planları robot action olmadan önce Teensy safety/motion katmanı tarafından kabul edilmelidir.
