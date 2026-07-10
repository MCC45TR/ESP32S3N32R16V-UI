# ESP32-S3 MROS Alt Komut Referansi

Dil: [English](esp32-mros-subcommand-reference.md) | Turkish

Bu dokuman ESP32-S3 `mros` komut grubunun ayrintili referansidir. [ESP32-S3 mshell Command Reference](esp32-shell-command-reference.md) ve [Robot And MROS Runtime Guide](robot-mros-runtime-guide.md) ile birlikte kullanilmalidir.

`mros`, bridge seviyesindeki operator namespace'idir. Genel Linux uyumluluk katmani degildir ve robot hareket komut namespace'i ile ayni sey degildir. S3 bridge, peer'lar, bus'lar, runtime sagligi, bellek, power mode, log, rapor, guvenlik ve destek artifact'lerini inceleyen komutlari toplar.

## Zihinsel Model

`mros` namespace'i bes operasyon katmanina ayrilir:

| Katman | Komut aileleri | Neyi kanitlar |
| --- | --- | --- |
| Sistem sagligi | `overview`, `health`, `doctor`, `perf`, `mem`, `sram`, `rtos`, `power` | S3 runtime canli, bellek yeterli, power/RTOS modu dogru ve tani calisabiliyor |
| Baglanti | `connections`, `bus`, `spi`, `uart`, `i2c`, `pca9685`, `wifi` | S3 peer ve lokal periferik linklerini gorebiliyor |
| Robot bridge gorunumu | `robot`, `pid`, `fk`, `ik`, `telemetry` | S3 bridge robot-facing runtime state ve math/telemetry durumunu okuyabiliyor |
| Kanit ve destek | `log`, `watch`, `record`, `export`, `diag`, `alerts`, `report`, `audit` | Operator debug veya release note icin tekrar edilebilir kanit toplayabiliyor |
| Yonetim | `security`, `users`, `ssh`, `config`, `test`, `downloader` | Session, user, SSH, config diff, aktif test ve indirilen artifact'ler incelenebiliyor veya degistirilebiliyor |

Bu kanit kapilarini ayri tutun:

- `mros health` bridge health check'tir.
- `mros spi status all` peer/bus durumudur.
- `robot math ik preview ...` matematik/niyet preview'udur.
- Canli fiziksel hareket ayri donanim gozlemiyle kanitlanmalidir.

## Hizli Baslangic

Yeni boot icin:

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

Destek paketi icin:

```text
mros doctor all
mros export diagnostics
mros report create
mros audit export
```

Tekrarlayan canli monitor icin:

```text
mros watch -n 5 --count 12 health
mros connections watch -n 5 --count 12
mros spi monitor all -n 5 --count 12
mros log follow -n 50 --cycles 30 --interval 1
```

## Sistem Komutlari

### `mros overview`

S3 bridge ozetini yazdirir:

- uptime,
- Wi-Fi state ve IP,
- heap ve PSRAM bos bellek,
- LittleFS kullanim durumu,
- T41, C3, UART loglari, PCA9685 ve Wi-Fi icin connection table.

Board boot ettiginde ilk okunabilir ozet olarak kullanin.

### `mros health`

Kisa health check calistirir ve ana peer'lar veya error counter'lar sagliksiz gorunuyorsa warning status dondurur.

Kontrol ettikleri:

- T41 QSPI/SPI peer,
- C3 SPI peer,
- PCA9685 hazirligi,
- SPI error counter'lari,
- motor power state,
- configure edilmisse bearing-health tablosu.

Hareketten, update'ten ve runtime iddiasi yayinlamadan once kullanin.

### `mros doctor quick|all|wifi|fs|robot|security|peer [--json]`

Yapisal diagnostic snapshot calistirir. JSON mode; storage, T41 QSPI, C3 SPI, Wi-Fi, shell session'lari, job pool, transaction state, UART bridge state ve remote filesystem state alanlarini verir.

```text
mros doctor quick
mros doctor all
mros doctor wifi
mros doctor security
mros doctor all --json
```

Notlar:

- Target verilmezse default `quick` olur.
- `all`, ek Wi-Fi ve security/audit detaylari ekler.
- Issue template ve otomasyon icin JSON cikti tercih edilmelidir.

### `mros perf status`

Performans odakli runtime bilgisi verir:

- heap free/minimum,
- PSRAM free,
- power mode ve CPU frekansi,
- SRAM floor ve en buyuk internal block,
- aktif power lock'lari ve Wi-Fi power-save mode,
- JSON overflow count,
- shell response pool miss/drop,
- mshell job pool status,
- WebSocket client sayilari,
- FK/PID timing,
- RTOS deadline-slip ozeti.

Web UI, shell, JSON veya telemetry yavas/kararsiz hissedildiginde kullanin.

## Bellek Komutlari

### `mros mem status [--detail|--json]`

Memory monitor durumunu yazdirir. Heap baskisi arastirirken `--detail`, makine-okunur kanit icin `--json` kullanin.

```text
mros mem status
mros mem status --detail
mros mem status --json
```

### `mros mem watch start|stop|status`

Memory watch sampler'ini kontrol eder.

```text
mros mem watch status
mros mem watch start --interval-ms 5000
mros mem watch stop
```

### `mros mem snapshot [name]`

Isimli memory snapshot saklar.

```text
mros mem snapshot before-web-test
mros mem snapshot after-web-test
```

### `mros mem diff [a b]`

Snapshot'lar arasinda JSON diff verisi yazdirir. Isimler verilmezse implementation default karsilastirma davranisini kullanir.

### `mros mem leaks [--json]`

Leak odakli monitor ciktisi verir.

### `mros mem reset`

Memory monitor state'ini sifirlar.

Operator kurali: internal SRAM kritikse agir rapor uretimi calistirmayin; `mros report create` bu durumda zaten reddeder ve `mros mem status --detail` ister.

## SRAM Ve RTOS

### `mros sram status --detail`

Internal SRAM durumunu ornekler ve yazdirir. Shell pool, report generation, JSON output veya web davranisi kararsizsa kullanin.

### `mros sram reclaim-plan`

SRAM baskisini azaltma planini yazdirir. Bu planlama/tani komutudur; sihirli allocator reset degildir.

### `mros rtos status`

Tracked task sayisi, wake/deadline miss toplamları, maksimum deadline miss ve maksimum execution time bilgisini verir.

### `mros rtos policy get|set observe|balanced|cool|performance|motion-safe|update-safe`

Device process manager policy yuzeyine gider. Runtime scheduling/power davranisini okumak veya degistirmek icin kullanilir.

### `mros rtos wake <task> [reason]`

Device process manager'a wake request gonderir.

## Power Komutlari

### `mros power status`

Power manager durumunu yazdirir. Hareket veya update oncesinde kullanin.

### `mros power mode cool|balanced|performance|motion-safe|update-safe`

Power mode okur veya degistirir.

```text
mros power mode
mros power mode balanced
mros power mode motion-safe
mros power mode update-safe
```

| Mode | Kullanim |
| --- | --- |
| `cool` | Idle, termal hassas veya uzun gozlem |
| `balanced` | Normal servis calismasi |
| `performance` | Agir web, diagnostic veya export isi |
| `motion-safe` | Harekete hassas operasyon |
| `update-safe` | Firmware download, staging ve recovery/update hazirligi |

### `mros power locks`

Aktif power management lock'larini yazdirir.

### `mros power temp`

Power manager gecerli okuma tutuyorsa sicakligi yazdirir.

### `mros power trace`

Son power trace bilgisini yazdirir.

### `mros power report`

Device process manager report yoluna gider.

## Baglanti Komutlari

### `mros connections list|status|tree`

Peer ve bus topology bilgisini yazdirir.

```text
mros connections list
mros connections status
mros connections tree
```

Status tablosu sunlari kapsar:

- `t41-qspi`: ESP32-S3 ile Teensy 4.1 peer yolu,
- `c3-spi`: ESP32-C3 peer yolu,
- `t41-uart-log`: Teensy UART log stream,
- `pca9685`: I2C PWM controller,
- `wifi`: station/network durumu.

### `mros connections watch -n 5 [--count 12]`

Generic `mros watch` engine ile connection status tablosunu tekrarlar.

## Bus Ve Periferik Komutlari

### `mros bus summary|errors`

`summary`, high-level connection table yazdirir. `errors`, CRC, marker, timeout ve peer-specific hata detaylarina odaklanir.

### `mros spi list`

Bridge tarafindan bilinen SPI peer'larini listeler.

### `mros spi status [t41|c3|all]`

Peer bazli SPI state yazdirir.

T41 icin connection state, transaction count, loop timing, CRC/marker counter, last marker, last sequence ve device status verir.

C3 icin connection state, receive count, loop rate, CRC/marker counter, last-good age ve ESP-NOW flag'lerini verir.

### `mros spi errors --last 25`

T41 error log JSON ve C3 counter'larini yazdirir. Mevcut implementation dokumandaki komut formunu kabul eder; `--last` degeri help metniyle uyumluluk icin bilgi niteligindedir.

### `mros spi reset-stats`

T41 ve C3 SPI error counter'larini sifirlar.

### `mros spi monitor [t41|c3|all] -n 5 [--count 12]`

`mros watch` uzerinden SPI durumunu tekrar tekrar yazdirir.

### `mros uart list|status|tail|monitor|log-mode|shell`

UART komutlari Teensy log yolunu ve remote shell bridge'i inceler.

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

Seri log gurultusu onemli satirlari kapatiyorsa `quiet`, UART bridge'i tani koyuyorsaniz `verbose` kullanin.

### `mros i2c scan|status`

I2C scan yapar veya I2C status yazdirir.

### `mros pca9685 status|channels`

PCA9685 hazirligini veya channel bilgisini yazdirir. Servo/PWM output supheli ise kullanin.

### `mros wifi diag`

Wi-Fi diagnostic summary yazdirir:

- Wi-Fi phase ve manager enabled state,
- STA/IP/RSSI/channel,
- last-good SSID/channel/BSSID,
- fast-path success ve attempt counter'lari,
- scan age ve reconnect backoff,
- last connect duration.

## Robot Bridge Gorunumu

Bu komutlar bridge-side robot state okur. Canonical `robot ...` komut ailesinin yerine gecmez, onu tamamlar.

### `mros robot state`

Turret target/actual, PID output/error, joint values, Cartesian coordinates, gripper value ve trajectory active flag yazdirir.

### `mros robot errors`

T41 error code, device status, T41 QSPI CRC/marker count, C3 SPI CRC/marker count ve turret PID error yazdirir.

### `mros pid status`

Turret PID target/actual, error/output, gain'ler, output lock, motor/PCA state, loop timing ve local control diagnostic counter'larini verir.

### `mros fk status`

FK durumunu verir: T41 coordinate, live web FK timing, active IK backend ve live preview state.

### `mros ik status`

IK preference, effective backend, T41 QSPI/ESP-NOW availability, target coordinates ve trajectory scale yazdirir.

### `mros telemetry status`

Console revision, last web feedback age, PID timing, CPU frequency ve WebSocket exposure notlarini verir.

## Log, Watch, Record, Export

### `mros log tail|follow [-n LINES] [--cycles NUM] [--interval SEC]`

Runtime log tail'i bir kez veya tekrarli okur.

```text
mros log tail -n 50
mros log follow -n 50 --cycles 30 --interval 1
```

### `mros watch -n 5 [--count 12] <mros-subcommand>`

Watch olmayan herhangi bir `mros` alt komutunu tekrarli calistirir.

```text
mros watch -n 5 --count 12 health
mros watch -n 2 --count 20 bus errors
```

Nested watch reddedilir.

### `mros record start spi [--seconds NUM]`

En fazla 30 saniye boyunca saniyede bir ornekle `/ESPUSER/diag_spi.csv` yazar.

Kolonlar:

- monotonic milliseconds,
- T41 connection state,
- T41 transaction count,
- T41 CRC ve marker counter,
- T41 sequence,
- C3 receive ve CRC counter,
- C3 loop rate,
- C3 position ve speed.

### `mros export diagnostics`

`/ESPUSER/diagnostics.txt` icine plain-text diagnostic snapshot yazar.

### `mros diag bundle`

Mevcut report/export yollariyla diagnostic bundle olusturur.

### `mros alerts`

Aktif alert ozetini yazdirir.

## Rapor Ve Audit

### `mros report create|list|show|delete`

Report komutlari `/ESPUSER/reports` altindaki JSON support report'larini yonetir.

```text
mros report create
mros report list
mros report show /ESPUSER/reports/report-123.json
mros report delete /ESPUSER/reports/report-123.json
```

Guvenlik notlari:

- Report creation oncesinde ve sonrasinda memory sample alir.
- Internal SRAM kritikse report creation calismaz.
- Report delete icin path `/ESPUSER/reports/` altinda olmalidir.

### `mros audit list|export|clear`

Shell/security audit ring okur, export eder veya temizler.

```text
mros audit list
mros audit export
mros audit clear
```

`export`, `/ESPUSER/audit.txt` yazar. `clear`, admin/root ister.

## Security, Users Ve SSH

### `mros security status|logout-all|audit`

```text
mros security status
mros security audit
mros security logout-all
```

`status`; HTTP session, login fail/lockout, serial auth, WebSocket auth count, shell session count, capability mask ve UART shell bridge mode bilgisini verir.

`logout-all`, HTTP/WS session'larini gecersiz kilar ve admin/root gerektirir.

### `mros users list|roles|add|passwd|disable`

```text
mros users list
mros users roles
mros users add "Display Name" username "password" admin sudo
mros users passwd username "new-password"
mros users disable username
```

Kurallar:

- User eklemek admin/root ister.
- Implementation normal extra user sayisini sinirlar.
- Password degisikligi admin/root veya self ister; root password degisikligi root session ister.
- Sadece extra user'lar disable edilebilir.

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

Notlar:

- Password 8-96 karakter olmalidir.
- `root-passwd`, root shell session ister.
- Public key `ssh-ed25519` veya `ssh-rsa` ile baslamalidir.
- Gercek credential'lari GitHub dokumanina veya issue comment'lerine yapistirmayin.

## Config, Test Ve Downloader

### `mros config diff`

Bridge configuration difference bilgisini yazdirir.

### `mros test spi|uart|i2c|all`

Secilen subsystem icin aktif hafif test calistirir.

### `mros downloader URL [TARGET]`

Dosyayi `/ESPUSER` veya `/user` storage'a indirir.

```text
mros downloader "https://example.com/app.bin" auto
mros downloader "https://example.com/app.bin" /ESPUSER/firmware/app.bin
```

Kurallar:

- Wi-Fi bagli olmalidir.
- LittleFS mount edilmis olmalidir.
- Target `/ESPUSER` veya `/user` icinde olmalidir.
- HTTPS URL, gecerli TLS ve public host gerekir.
- Target `auto` ise update-like veya `.bin` adlari updates dizinine, digerleri downloads dizinine gider.

## Kanit Receteleri

### Bridge Health Kaniti

```text
mros overview
mros health
mros doctor quick --json
mros mem status --detail
mros perf status
mros power status
```

### Link Kaniti

```text
mros connections status
mros connections tree
mros bus errors
mros spi status all
mros uart status
mros i2c status
mros pca9685 status
```

### Robot Bridge Kaniti

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

### Support Package Kaniti

```text
mros record start spi --seconds 30
mros export diagnostics
mros audit export
mros report create
ls -l /ESPUSER
ls -l /ESPUSER/reports
```

## Fazla Iddia Etmeyin

GitHub dokumantasyonunda net dil kullanin:

| Iddia | Minimum kanit |
| --- | --- |
| S3 runtime canli | `mros overview` veya `status --json` |
| Bridge health kabul edilebilir | `mros health` ve `mros doctor quick` |
| T41/C3 linkleri gorunuyor | `mros connections status` ve `mros spi status all` |
| UART log bridge aktif | `mros uart status` veya `mros uart tail` |
| I2C/PCA yolu gorunuyor | `mros i2c status` ve `mros pca9685 status` |
| Bellek report/export icin stabil | `mros mem status --detail` |
| Support report var | `mros report create` ve `mros report list` |
| Robot komutu preview edildi | `robot ... preview --json` |
| Fiziksel hareket oldu | canli donanim gozlemi, loglar ve safety checklist |

Bu ayrim, hobi README'si ile profesyonel robotik reposu arasindaki farktir.
