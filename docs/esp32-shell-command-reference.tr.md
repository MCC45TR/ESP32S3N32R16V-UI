# ESP32-S3 mshell Komut Referansi

Dil: [English](esp32-shell-command-reference.md) | Turkish

Bu sayfa DEUSCARA bridge firmware icindeki ESP32-S3 `mshell` yuzeyini aciklar. Hedef, GitHub'da projeyi ilk kez okuyan birinin cihazin ne yapabildigini anlamasi ve operatorun bring-up, tani, update ve recovery islerinde tekrar edilebilir komutlar kullanabilmesidir.

Bridge-level `mros` namespace'i icin ayrica [ESP32-S3 MROS Alt Komut Referansi](esp32-mros-subcommand-reference.tr.md) sayfasina bakin.

## Shell Nerede Calisir?

`mshell`, ESP32-S3 firmware icine gomulu komut yorumlayicisidir. Seri konsoldan ve firmware/guvenlik konfigurasyonu izin verdiginde SSH veya web uzerinden komut calistirma yuzeylerinden erisilebilir.

ESP32-S3 shell deterministik hareket otoritesi degildir. Bu shell su isler icindir:

- Wi-Fi ve ag durumu,
- LittleFS ve `/ESPUSER` dosya islemleri,
- web/telemetry/servis tanilari,
- recovery update hazirligi,
- robot komut niyeti olusturma,
- Teensy 4.1, ESP32-C3, PCA9685 ve UART/SPI link tanilari.

Fiziksel robotu etkileyebilecek hareketler yine robot kontrol katmani ve aktif guvenlik durumu tarafindan kabul edilmelidir.

## Ilk Komutlar

Boot sonrasinda firmware, depolama, link durumu ve komut sozlugunu gormek icin:

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

Script, AI araci, web istemcisi veya CI cikti toplama isleri icin JSON kullanin:

```text
status --json
robot status --json
robot math status --json
mshell schema commands --json
mshell schema api --json
```

## Komut Aileleri

| Aile | Komutlar | Amac |
| --- | --- | --- |
| Yardim ve shell metadata | `help`, `man`, `mshell`, `history`, `which`, `set`, `config` | Komut kesfi, schema cikarma, shell/session ayarlari |
| Sistem durumu | `status`, `mfetch`, `uptime`, `date`, `uname`, `espfetch`, `free`, `ps`, `htop`, `dmesg` | Firmware, heap, task, boot ve runtime durumunu okuma |
| Dosya ve depolama | `ls`, `tree`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `touch`, `df`, `du`, `mount`, `umount`, `lsblk`, `stat`, `find` | LittleFS, `/ESPUSER`, mount edilen alanlar, loglar, firmware imajlari |
| Metin/binary inceleme | `head`, `tail`, `wc`, `grep`, `sed`, `awk`, `nl`, `xxd`, `hexdump`, `od`, `strings`, `sha256sum`, `md5sum`, `crc32` | Log, manifest, script, tani ve binary payload inceleme |
| Ag | `wifi`, `ping`, `curl`, `wget`, `hostname`, `ssh`, `espnow` | Wi-Fi ayari, dosya indirme, erisilebilirlik testi |
| Cihaz linkleri | `devices`, `mros connections`, `mros bus`, `mros spi`, `mros uart`, `mros i2c`, `gpio`, `i2cdetect`, `pwm`, `adc`, `spi`, `uart` | Kart linkleri ve periferik tanilari |
| Robot arayuzu | `robot`, `mros pid`, `mros fk`, `mros ik` | Robot niyeti olusturma, matematik durumu, preview/apply akislari |
| Proje bilgisi | `project` | Gömülü proje, web sayfasi, hedef, lisans, versiyon ve TODO ozetleri |
| Update ve recovery | `update-system`, `update-c6`, `mros7dofs3_update` | Firmware imajlarini hazirlama ve update akislari |
| Guvenlik ve audit | `whoami`, `id`, `groups`, `su`, `sudo`, `passwd`, `change`, `mros security`, `mros audit` | Kimlik, yetki, session ve audit durumunu okuma |
| Rapor ve destek | `mros report`, `mros diag`, `logger`, `journalctl`, `tar`, `gzip`, `gunzip`, `zcat` | Destek paketi, log okuma, kanit disari aktarma |

## `mshell` Meta Komutlari

`mshell`, shell'i yapisal bir API gibi disari acar. Otomasyon icin en temiz giris burasidir.

| Komut | Anlam |
| --- | --- |
| `mshell schema commands` | Komut registry'sini capability/risk bilgisiyle yazdirir |
| `mshell schema commands --json` | Registry'yi JSON olarak verir |
| `mshell schema api` | Ust seviye API adlarini shell komutlarina map eder |
| `mshell schema openapi-lite --json` | Kucuk OpenAPI-benzeri schema verir |
| `mshell call system.status` | Isimli API cagrisi calistirir |
| `mshell call files.list path=/ESPUSER` | API wrapper ile dosya listeler |
| `mshell call robot.ik x=300 y=300 z=250` | Robot IK API wrapper calistirir |
| `mshell profile safe` | Kisa guvenli saglik profilini calistirir |
| `mshell profile debug` | Daha genis debug profilini calistirir |
| `mshell status s3|t41|all` | Remote shell routing durumunu gosterir |
| `mshell connect s3|t41|all` | Destekleniyorsa remote hedef session acar |
| `mshell disconnect s3|t41|all` | Remote hedef session kapatir |

Ornek API adlari:

| API | Arkadaki komut |
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

## Wi-Fi Ve Ag

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

Notlar:

- `wifi connect` credential kaydeder ve baglanmayi dener.
- `wifi save` credential kaydeder ama hemen reconnect zorlamaz.
- `wifi state hotspot` AP/captive portal modunu zorlar.
- `wifi state espnow`, C3SPI ve Teensy ESP-NOW davranisi icin C3 failsafe yolunu secer.
- Gercek sifreler GitHub issue, screenshot veya dokuman orneklerine konmamalidir.

## Dosya Sistemi Ve Kanit

Operator kullanici alani genellikle `/ESPUSER` dizinidir. Firmware imajlari, raporlar, scriptler, robot verileri ve tani ciktilari burada tutulur.

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

Iyi bir kanit paketi genellikle sunlari icerir:

- `mfetch --full`,
- `mros health`,
- `mros connections status`,
- `mros bus errors`,
- `robot status --json`,
- `robot diag status --json`,
- ilgili `/ESPUSER` loglari,
- tam firmware versiyonu ve build profili.

## Cihaz Ve Link Tanilari

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

MROS tani komutlari:

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

Komut bazli davranis, operator kurallari ve kanit receteleri icin [ESP32-S3 MROS Alt Komut Referansi](esp32-mros-subcommand-reference.tr.md) kullanin.

Sinir:

- `t41-qspi`, ESP32-S3 ile Teensy SPI/QSPI bridge durumunu anlatir.
- `t41-uart-log`, Teensy tarafindan gelen UART log/console aktivitesini anlatir.
- `c3-spi`, ESP32-C3 yolunu anlatir.
- `pca9685`, servo/PWM cikis kontrolcusu hazirligini anlatir.
- Build veya shell cevabi tek basina guvenli fiziksel hareket kaniti degildir.

## Update Ve Recovery

`update-system`, LittleFS uzerindeki ESP32-S3 firmware imajini recovery-mode kurulum icin hazirlar.

```text
update-system --list
update-system --dry-run
update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin
update-system --verify /fs/ESPUSER/firmware/app.bin
update-system --prepare-only /fs/ESPUSER/firmware/app.bin
update-system --no-reboot /fs/ESPUSER/firmware/app.bin
```

Davranis:

- Dosya verilmezse once `/ESPUSER/firmware`, sonra LittleFS fallback aranir.
- `--dry-run`, manifest yazmadan hedefi cozer.
- `--verify`, imaji hash'ler ve `app0` icine sigdigini dogrular.
- `--prepare-only`, manifest yazar ama recovery secmez/reboot etmez.
- `--no-reboot`, recovery'yi secer ama reboot'u operatora birakir.
- Imzali manifest icin build konfigurasyonunda `MROS_UPDATE_MANIFEST_HMAC_KEY` gerekir.

Onerilen on kontrol:

```text
mfetch --full
df -h
ls -l /ESPUSER/firmware
sha256sum /ESPUSER/firmware/app.bin
update-system --dry-run --verify /fs/ESPUSER/firmware/app.bin
```

## Project Komutu

`project`, cihaz uzerinde gomulu proje dokumantasyonu yazdirir.

```text
project status
project info
project web-page
project goals
project todo
project license
project version
```

Bu komut, canli cihaz eldeyken repo acik degilse bile bridge mimarisi, web UI rolu, MATLAB/reference-model yonu, hedefler, yol haritasi, lisans ve runtime versiyonunu ozetler.

## Robot Giris Komutlari

Robot ailesi ayrintili olarak [Robot And MROS Runtime Guide](robot-mros-runtime-guide.md) icinde aciklanir. Kisa referans:

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

Guvenlik kurali: `apply` veya `run` oncesinde `preview` kullanin; motor power, emergency stop, hold, Teensy link durumu ve fiziksel boslugu ayri ayri kontrol edin.

## Guvenlik Ve Audit

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

`su`, `sudo`, `passwd`, `change`, `mros security logout-all` ve `mros audit clear` guvenlik durumunu degistirebilir. Bunlar idari aksiyon olarak ele alinmalidir.

## Onerilen Bring-Up Akisi

Yeni build flash sonrasi veya kablolama degisikligi sonrasi:

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

Release kaniti veya hata raporu yayinlarken tam komut ciktilarini saklayin. Web sayfasi ekran goruntusu faydalidir ama link, heap, partition, update ve safety durumlari icin shell ciktisinin yerine gecmez.
