# MROS DEUSCARA - ESP32-S3 <-> Teensy 4.1 Baglanti Karari

Bu dosya `src/config/hw_config.h`, `src/comm_interfaces/spi/spi_t41_link.*`, Teensy `src/board/mros_teensy41_pins.*` ve Teensy `src/comm/qspi_esp32s3/*` kodlariyla karsilastirilarak guncellendi.

## Simdi Taksak Calisir mi?

| Hat | Karar | Neden |
|---|---|---|
| UART1 / Serial5 | Evet, bring-up icin uygun | Pinler ve hiz sozlesmesi eslesiyor: ESP32-S3 `UART1` 5 Mbps, Teensy `Serial5` 5 Mbps. Ortak GND ve 3.3 V seviye sart. |
| MSHELL2 / MSH1 UART | Evet, temel uyum aktif | ESP32-S3 tarafinda MSHELL2 metin ve MSH1 COBS ikili tünel var. Teensy tarafinda RX poll + MSHELL2/MSH1 sayaçlari artik aktif. Lab icin `uart-crypto disabled` / plaintext durumda tutulmali. |
| QSPI / SPI realtime link | Evet, SPI uyumluluk modu ile bring-up uygun | ESP32-S3 klasik `spi_slave` 1-bit MOSI/MISO/SCK/CS kullaniyor. Teensy tarafinda FlexIO1 4-bit QSPI pin kontrati korunurken `kFlexioQuadDmaDriverActive=false` durumda sync `SPI1` fallback aktif edildi (IO0/IO1/SCK/CS). |
| CANopen motor bus | ESP32 ile ilgili degil | Teensy-CANopen motor hattidir; ESP32-S3'e dogrudan baglanmaz. Fiziksel katmanda Adafruit CAN Pal 5708 / TJA1051T/3 ve motor tarafinda Motorobit DB9 disi terminal block adapter kullanilir: `1=PU+`, `2=PU-`, `3=DIR+`, `4=DIR-`, `5=WR+`, `6=OZ`, `7=COM`, `8=CANL`, `9=CANH`. Aktif CANopen hatti `7/8/9`; `1-6` motor tarafi pasif yardimci sinyallerdir. |

Sonuc: Bugun kablo takilacaksa UART1/Serial5 hemen calistirilabilir. QSPI adli servis de su an klasik 1-bit SPI uyumluluk yoluyla bring-up yapabilir; gercek 4-bit QuadSPI DMA fiziksel yol henuz lab-validasyon asamasindadir.

## 2026-05-28 Build ve Port Dogrulamasi

| Kontrol | Komut | Sonuc |
|---|---|---|
| Teensy firmware build | `pio run -e teensy41` (`TEENSY41-Brain`) | Basarili |
| ESP32-S3 firmware build | `pio run -e s3_mros_hub_n32` (`ESP32S3N32R16V-UI`) | Basarili |
| Teensy USB port gorunurlugu | `pio device list` | `USB VID:PID=16C0:0483` ile `<SERIAL_PORT>` gorunuyor |
| ESP32 app flash offset kontrolu | `.b/s3_mros_hub_n32/flasher_args.json` | Ana firmware `0x120000` (app0) |

Notlar:
- ESP32 tarafinda boyut/offset tutarliligi icin `platformio.ini` icine `board_build.app_partition_name = app0` eklendi.
- `partitions-N32R16.csv` icinde normal app etiketi `app0` olarak kalir ve `ota_0` subtype ile `0x120000` offsetinden flashlanir.
- Recovery etiketi `recovery` olarak korunur ve `0x20000` offsette tutulur.

## UART1 / Serial5 Kablo Tablosu

| ESP32-S3 GPIO | ESP32-S3 Rol | Teensy 4.1 Pin | Teensy Rol | Not |
|---:|---|---:|---|---|
| 17 | `UART1_TX` | 21 | `Serial5 RX` | ESP32-S3 -> Teensy veri |
| 18 | `UART1_RX` | 20 | `Serial5 TX` | Teensy -> ESP32-S3 veri |
| 16 | `UART1_CTS` | 19 | `Serial5 RTS` | Opsiyonel akış kontrol; ESP32 sadece CTS kullanir |
| GND | Ground | GND | Ground | Ortak ground zorunlu |

UART sozlesmesi:

| Alan | Deger |
|---|---|
| Baud | `5,000,000` |
| Data | `8N1` |
| ESP32 taraf | `UART_NUM_1`, `PIN_UART1_TX=17`, `PIN_UART1_RX=18`, `PIN_UART1_CTS=16`, `PIN_UART1_RTS=-1` |
| Teensy taraf | `Serial5`, `RX=21`, `TX=20`, `RTS=19`, `CTS_RESERVED=18` pasif |
| Plaintext bring-up | Uygun |
| Sifreli UART | Iki tarafta da ayni `MUS1`/PSK acilmadan kullanma |

## UART Protokol Katmanlari

| Katman | ESP32-S3 durumu | Teensy durumu | Karar |
|---|---|---|---|
| Plain text log | Var | Var | Lab bring-up icin en guvenli ilk test |
| `UART_Wifi_Status_t` COBS frame | ESP32-S3 her 1 s gonderiyor; `user` alani `clients:N` tasir | Teensy CRC/durum consumer aktif; LCD IP banner icin kullanir | Wi-Fi bagli ve `clients:0` iken LCD IP gosterir |
| `MSHELL2:` text frame | ESP32-S3 parse ediyor | Teensy RX poll + counter aktif | Iki yonlu temel uyum |
| `MSH1` COBS binary tunnel | ESP32-S3 parse ediyor | Teensy COBS decode + binary frame counter aktif | Iki yonlu temel uyum |
| `MUS1` UART crypto | Teensy tarafinda var | ESP32-S3 tarafinda acik consumer gorulmedi | Simdilik kapali/lab plaintext |

## QSPI / SPI Durumu

### ESP32-S3 Kodundaki Aktif SPI Pinleri

| ESP32-S3 GPIO | Rol | Not |
|---:|---|---|
| 10 | `PIN_SPI_CS` | Klasik SPI slave CS |
| 11 | `PIN_SPI_MOSI` | Klasik SPI MOSI |
| 12 | `PIN_SPI_SCK` | Klasik SPI clock |
| 13 | `PIN_SPI_MISO` | Klasik SPI MISO |
| 14 | `PIN_DATA_READY` / `ESP_READY` | ESP32-S3 -> Teensy ready |
| 15 | `PIN_T41_READY` | Teensy -> ESP32-S3 ready input |
| 39 | `PIN_TEENSY_IRQ` | ESP32-S3 -> Teensy IRQ |
| 40 | `PIN_TEENSY_RESET` | Teensy -> ESP32-S3 reset input |

### Teensy Kodundaki QSPI Pin Sozlesmesi

| Teensy Pin | Rol | Not |
|---:|---|---|
| 2 | `QSPI_SCK` | FlexIO1 QuadSPI hedef clock |
| 3 | `QSPI_IO0` | Quad data lane |
| 4 | `QSPI_IO1` | Quad data lane |
| 33 | `QSPI_IO2` | Quad data lane |
| 5 | `QSPI_IO3` | Quad data lane |
| 6 | `QSPI_CS` | Active-low select |
| 8 | `T41_READY` | Teensy -> ESP32-S3 |
| 9 | `ESP_READY` | ESP32-S3 -> Teensy |
| 10 | `ESP_IRQ` | ESP32-S3 -> Teensy |
| 11 | `ESP_RESET` | Teensy -> ESP32-S3 |

### QSPI Servisindeki Mevcut Fiziksel Durum

- ESP32-S3 endpoint'i artik `quad-ready`/`classic` modunu runtime secer. `PIN_SPI_WP` ve `PIN_SPI_HD` atanmadiysa klasik 1-bit SPI (`quadwp_io_num=-1`, `quadhd_io_num=-1`) kullanilir.
- `PIN_SPI_WP`/`PIN_SPI_HD` atanirsa bus 4-bit lane pinleriyle acilir (`quad-ready-4bit`), ancak master tarafindaki fiziksel 4-bit yol yine ayri dogrulama ister.
- Teensy tarafinda `kFlexioQuadDmaDriverActive=false` iken sync `SPI1` fallback devreye girer ve `SCK=2`, `IO0=3`, `IO1=4`, `CS=6` hatlariyla transfer baslatir.
- Gercek 4-bit QuadSPI DMA (IO0/IO1/IO2/IO3) hatti halen `driver=pending` durumundadir.

## Guvenli Bring-up Sirasi

1. Sadece UART1/Serial5 ve GND bagla.
2. Iki karti da USB'den ayri ayri besle veya ortak power planini dogrula; 5 V tolerant varsayma.
3. ESP32-S3 shell'de `set uart-shell-bridge listen` veya gereken test icin `set uart-shell-bridge on` kullan.
4. Teensy USB mshell'de `uart status`, `mshell status s3`, `mshell connect s3` komutlariyla sayaçlari kontrol et.
5. SPI uyumluluk testi icin ek olarak su baglantiyi yap: ESP32 `GPIO12(SCK)->T41 pin2`, `GPIO11(MOSI)->T41 pin3`, `GPIO13(MISO)<-T41 pin4`, `GPIO10(CS)<->T41 pin6`, `GPIO14(ESP_READY)->T41 pin9`, `GPIO15(T41_READY)<-T41 pin8`.
6. Realtime 4-bit QuadSPI beklentisini simdilik acma; bu adim icin FlexIO1 DMA yolunun ayrica dogrulanmasi gerekiyor.

## Mshell Link Diagnostic v1 Komut Sozlesmesi

ESP32-S3 tarafinda yeni komutlar:

- `spi test [--duration 10] [--json]`
- `uart test [--duration 10] [--json]`
- `robot diag linktest [uart|qspi|all] [--duration 10] [--json]`

Teensy tarafinda esit komutlar:

- `comm qspi test [--duration 10] [--json]`
- `comm qspi clock status|suggest|set <hz>|apply-suggested [--yes] [--json]`
- `uart test [--duration 10] [--json]`
- `devices test p4|uart|s3-uart|all [--json]`

Not:
- `c3` hedefi bu topolojide devre disidir; eski alias cagrilarinda komut acikca `disabled` doner.

Varsayilan test ciktilari:

- Sirali metin satirlari
- Varsayilan cikti sadece metin
- `--json` verilirse yalnizca JSON basilir

Sira formati:

- `Cihaz ile iletisim: OK/FAIL`
- `Yanit alindi (UART DIAG): OK/FAIL`
- `Yanit alindi (QSPI Frame|UART Frame): OK/FAIL`
- `Saglik: %NN.N`
- `Veri hizi: X.XXX Mbps`
- `Oneri: Hiz ...`

Ortak DIAG mesaj cercevesi (UART/mshell):

- `MSHELL:DIAG:PING:<id>:<t_ms>`
- `MSHELL:DIAG:PONG:<id>:<t_ms>:OK`
- `MSHELL:DIAG:CLOCK_PREP:<new_hz>:<nonce>`
- `MSHELL:DIAG:CLOCK_ACK:<new_hz>:<nonce>:OK|BUSY|ERR`
- `MSHELL:DIAG:CLOCK_COMMIT:<new_hz>:<nonce>`
- `MSHELL:DIAG:CLOCK_ROLLBACK:<old_hz>:<nonce>`

Saat onerme merdiveni (Hz):

- `780000`
- `1000000`
- `1456000`
- `2000000`
- `2567000`
- `3200000`

## Duzeltilen Derleme Notu

`spi_t41_slave.h` adi artik yok; ESP32-S3 kodu `spi_t41_link.h` dosyasina bakmali. Bu include sozlesmesi kodda guncellendi.
