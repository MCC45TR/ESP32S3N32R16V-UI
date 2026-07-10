# ESP32 Production Security Profile

Bu profil, MROS-DEUSCARA ESP32-S3 firmware'inin sahaya cikmadan once hangi
donanim guvenlik kapilarindan gecmesi gerektigini tanimlar.

## Zorunlu kapilar

1. `MROS_PRODUCTION_BUILD` build flag'i release pipeline'da tanimli olmalidir.
2. Secure Boot V2 etkin olmadan production build gecmemelidir.
3. Flash Encryption release mode etkin olmadan production build gecmemelidir.
4. NVS Encryption etkin olmadan credential veya user partition release'e cikmamalidir.
5. Anti-rollback ve `CONFIG_APP_SECURE_VERSION` release versiyon politikasina baglanmalidir.
6. `MROS_UPDATE_MANIFEST_HMAC_KEY`, recovery pairing token, WiFi local secret ve bootstrap kimlikleri cihaz/ortam ozel provision edilmelidir.

## Partition at-rest policy

Aktif partition CSV'lerinde `nvs`, `nvs_sys_usr`, `littlefs` ve `coredump`
partition'lari `encrypted` flag'i tasir. Bu flag tek basina yeterli degildir;
gercek koruma Flash Encryption eFuse durumuna baglidir.

## Provisioning sirasinda beklenen kanit

- `espefuse.py summary` ciktisinda secure boot, flash encryption ve anti-rollback eFuse durumlari.
- Ilk boot log'unda signed image dogrulama ve flash encryption aktifligi.
- NVS key partition/protected-key akisi icin IDF provisioning log'u.
- Recovery install denemesinde imzasiz artefact'in reddedildigi log.
- Web/file fetch denemesinde HTTP, localhost/private IP ve TLS hatalarinin reddedildigi log.

## Lab istisnalari

HTTP fetch, private host fetch, redirect ve unsigned recovery upload gibi istisnalar
yalnizca acik derleme makrolariyla ve lab firmware'inde acilabilir. Production
build bu makrolari fail-closed kabul etmelidir.
