---
id: autodoor-architecture
type: architecture-design
title: AutoDoorV2 mimarisi — ESP-NOW, BLE ve OTA hattı
status: active
parent: autodoor-v2
---

# AutoDoorV2 mimarisi

## Sözlük

- **gate** = **kapı** = panjur/kepenk/garaj kapısı kontrol cihazı. Koddaki, env adlarındaki (`gate`) ve bu
  spec'teki "gate" bu cihazdır; ayrı bir kavram değildir.
- **vehicle** = araç ünitesi (araçtaki ESP32).
- **panel** = tarayıcıda çalışan Web Bluetooth arayüzü (kapı paneli / araç paneli).
- **tracker** = bu ürünün parçası **değil**; public yayın deposunu paylaşan ayrı bir ürünün firmware'i
  (`tracker_firmware_*.bin`).

## Topoloji

- **Kapı/panjur kontrol cihazı** (`gate-firmware`) — motor, ışık, buzzer sürücüsü; ESP-NOW alıcısı; BLE
  sunucusu (panel ayarları + OTA).
- **Araç cihazı** (`vehicle-firmware`, `ekranli` varyantı) — ESP-NOW gönderici/alıcı; BLE sunucusu
  (ayarlar + PIN).
- **Paneller** (`web-panels`) — tarayıcıda Web Bluetooth istemcisi; kapı paneli ayrıca OTA istemcisi.

Bağlantılar:

- **Kapı ↔ Araç: ESP-NOW**, 2.4 GHz kanal 6 (iki tarafta da `FIXED_WIFI_CHANNEL 6`). Broadcast
  `FF:FF:FF:FF:FF:FF` (keşif, tarama, beacon) ve unicast (anahtar teslimi, komut). Ağ/altyapı yok.
- **Panel ↔ cihaz: BLE GATT**, tarayıcı `navigator.bluetooth`. Cihazda HTTP sunucusu yoktur.
- **Panel ↔ GitHub**: OTA sürüm kontrolü (`version.json`) ve firmware indirme, HTTPS.

Kod bağı: iki firmware yalnız `src/common.cpp`'yi paylaşır — BLE UUID'leri, `PMK`, ESP-NOW sabitleri ve
yardımcılar (`logMessage`, `macToString`, `calculateHmac`, `encryptDecrypt`, `getFirmwareVersion`). Kapı ve
araç kodu birbirini include etmez; aralarındaki tek sözleşme aşağıdaki tel protokolüdür.

## ESP-NOW tel protokolü

JSON, UTF-8, `msgType` alanı zorunlu, kanal 6 sabit. Peer'ler `encrypt = false` ile eklenir; `lmk = PMK`
yazılsa da ESP-NOW donanım şifrelemesi **kullanılmaz** (broadcast peer'ı zaten şifrelenemez). Tüm koruma
uygulama katmanındadır.

| Mesaj | Yön | Alanlar | Ne zaman |
|---|---|---|---|
| `DISCOVERY_PROBE` | kapı → broadcast | — | Panelden tarama başlatılınca, 1 sn'de bir |
| `DISCOVERY_RESPONSE` | araç → kapı | `deviceName` | Araç kendini tanıtır, sonra peer'i siler |
| `SCAN_REQUEST` | kapı → broadcast | — | Butonla tarama modunda, 1 sn'de bir |
| `AUTH_ACK` | araç → kapı | `deviceName` | Araç tarama modundayken cevap verir |
| `KEY_DELIVERY` | kapı → araç | `key` | 16 baytlık ortak anahtar, PMK ile XOR'lanmış; 500 ms'de bir, 5 sn boyunca |
| `KEY_ACK` | araç → kapı | — | Eşleştirme tamam |
| `BEACON` | kapı → broadcast | `nonce` (yalnız yetkilendirme açıksa) | Boşta 2 sn'de bir |
| `COMMAND` | araç → kapı | `command` (`WARN` \| `OPEN` \| `CLOSE`), `nonce`, `hmac` | Araç, eşleşmiş kapının beacon'ını görünce |

Komut imzası: `HMAC-SHA256(sharedKey, <araç MAC> + command + nonce)` → hex. `nonce`/`hmac` yalnız güvenli
modda (beacon `nonce` taşıyorsa) eklenir; taşımıyorsa araç imzasız gönderir ve yetkilendirme açık kapı bunu
reddeder. Kapı araç MAC'ini ESP-NOW kaynak adresinden alır, araç ise imzayı `WiFi.macAddress()` ile atar —
**bu iki değer aynı olmak zorundadır**, farklıysa tüm güvenli komutlar sessizce reddedilir. Nonce tek
kullanımlık değildir; kapı her beacon'ta yenisini üretir ve tekrar kullanımı engellemez.

## BLE GATT sözleşmesi

İki ayrı servis; UUID'ler `src/common.cpp`'de tanımlıdır.

- **Kapı servisi** (`GATE_SERVICE_UUID`): `CHAR_SETTINGS_UUID` (R/W, JSON ayarlar), `CHAR_PIN_AUTH_UUID`
  (R/W, ham PIN), `CHAR_STATUS_UUID` (notify, JSON olaylar), `CHAR_DEVICE_MGMT_UUID` (R/W, kayıtlı araç
  listesi + `action` komutları), `CHAR_OTA_CONTROL_UUID` (W, JSON `command`), `CHAR_OTA_DATA_UUID`
  (W no-response, 4 bayt CRC32 + parça).
- **Araç servisi** (`VEHICLE_SERVICE_UUID`): settings, PIN auth, status. OTA ve cihaz yönetimi yoktur.

Karakteristik bazındaki alan listeleri modül spec'lerinde tutulur (`gate-firmware`, `vehicle-firmware`);
istemci tarafı `web-panels`.

## OTA hattı

1. Panel yayın deposundaki **kendi kanalının** manifestini çeker: `gate/version.json` → `product`,
   `latest_version`, `firmware_url`, `firmware_hash`. İstek `Accept: application/vnd.github.v3.raw`
   başlığıyla ve önbellek kırıcı `?t=<zaman>` parametresiyle yapılır; `firmware_url` bir GitHub Contents API
   adresi olabilir, aynı başlık onu da ham bayt olarak indirir. `product` alanı `gate` değilse panel hiçbir
   şey yüklemez, hata gösterir (fail-closed); sürüm karşılaştırması da açıktır — yalnız `latest_version`
   cihazın sürümünden büyükse güncelleme önerilir.
2. Firmware'i indirir, SHA-256'sını doğrular ve cihaza BLE ile bağlanır; cihaz bağlantıda `ota_config`
   (`windowSize`, `chunkSize`) gönderir.
3. Panel `{"command":"start","size":<bayt>,"hash":"<sha256>"}` yazar; cihaz `Update.begin(size)` yapar.
4. Parçalar `ota_data`'ya 4 bayt CRC32 + veri olarak yazılır; CRC uyuşmazsa cihaz `ota_nack` bildirir.
5. Her `windowSize` parçada cihaz `{"event":"ota_ack","id":<pencere>}` bildirir; panel ACK gelmeden sonraki
   pencereye geçmez. 5 sn ACK ilerlemesi olmazsa cihaz ACK'i tekrar gönderir.
6. Panel `{"command":"end"}` yazar; `Update.end(true)` başarılıysa cihaz `update_status` + `reboot:true`
   gönderir.
7. **Kopmada devam**: cihaz ilerlemeyi RAM'de tutar (reboot'ta kaybolur). Panel yeniden bağlanıp aynı hash
   ile `start` gönderdiğinde cihaz `ota_ready` ile `progress`/`windowId` verir ve kaldığı yerden sürer;
   hash değişmişse güncellemeyi sıfırdan başlatır.

**Yayın topolojisi**: kaynak depo `NimeCloud/AutoDoorV2` (bu repo); yayın deposu
`NimeCloud/GateControlFirmware` (public). `release/` bu projede `.gitignore`'ludur ve **yayın deposunun
yerel klonudur** (kullanıcı onayı); `release.bat` orada `git add/commit/pull/push` çalıştırır, böylece
artefaktlar public depoya gider.

**Ürün kendi kanalında yayınlanır**: kapının artefaktları `gate/` alt klasöründedir —
`gate/gate_firmware_<build>.bin` ve `gate/version.json`. Depo kökü, `version.json` dahil, bu ürüne ait
değildir ve **değiştirilmez**; yanlış manifestin cihaza yazılmasını manifestteki `product` alanı engeller.
Bu makinede `release/` henüz o deponun klonu değil; ilk yayından önce `git clone … release` yapılmalıdır
(yayın adımı bu değişikliğin kapsamı dışında bırakıldı).

**Yayın deposu tek bir OTA kanalıdır**: kökte `<ürün>_firmware_<build>.bin` dosyaları ve **tek bir**
`version.json` bulunur. Manifest alanı ürünler arasında paylaşılır — en son yayınlayan ürün kanalın sahibi
olur.

Doğrulanmış geçmiş (GitHub API, bu oturumda):

- **2025-07-09 → 2025-07-17** depo bu ürünündü; commit'ler `release.bat`'ın `"Release Build N surumu
yayinlandi."` mesajlarını taşır. 2025-07-17 anında kökte 8 adet `gate_firmware_*.bin` (build 154..162) +
  `version.json` vardı.
- **2025-10-23'ten itibaren** depo ayrı bir **araç takip (tracker)** projesine geçti ve 2026-02-28'e kadar
  onunla kullanıldı. Bugün kökte 263 `tracker_firmware_*.bin` (build 1..1561) var, **hiç
  `gate_firmware_*.bin` yok** — kapı artefaktları silinmiş.
- Bugünkü `version.json`: `latest_version: 1.1.1561`, `firmware_url` → `NimeCloud/TrackerControlFirmware`
  (public API'de 404; ayrı/private depo).
- Yerel kaynakta build 167'ye kadar üretim yapılmış, ama yayınlanan en son gate sürümü **162**; `release/`
  klonu bu makinede yok.

Sonuç: kapı cihazının varsayılan `updateUrl`'i geçerli bir gate artefaktı tanımlamıyor — kapı OTA'sı bu
haliyle çalışmaz; kanal yanlış ürünün manifestiyle yayınlanırsa panel, hash doğrulamasını *geçen* yanlış
firmware'i cihaza yazabilir.

## Derleme ve yayın hattı

- PlatformIO, `espressif32@~6.11.0`, Arduino framework, `-Os`, LTO yok.
- Env'ler: `gate` (esp32dev, `custom_partitions.csv`), `vehicle` (lolin_c3_mini, `huge_app.csv`),
  `ekranli` (esp32dev, `huge_app.csv`, `-D ENV_EKRANLI`).
- `scripts/version_script.py` **yalnız `gate` env'inde** çalışır: `version_build.txt`'yi artırır,
  `src/version.h` üretir, binary'yi `gate_firmware_<build>` olarak adlandırır, `release/`'e kopyalar,
  SHA-256 hesaplayıp `version.json` yazar. `vehicle`/`ekranli`'de çalıştırılırsa kapının OTA artefaktını ve
  manifestini ezer — bu yüzden bu env'lerde kapalıdır.
- `custom_partitions.csv`: `app0`/`app1` 0x190000 (1,6 MB), `spiffs` 0xCD000 (820 KB). **SPIFFS hiç
  kullanılmıyor**; bölüm ayrılmış ama boş duruyor.

## Ölçülen flash bütçesi (gate, build 167)

Toplam 1 596 573 / 1 638 400 bayt (**%97,4**), RAM %17,4. En büyük kalemler: Bluedroid BT yığını ~700 KB
(`libbt.a` 578 + `libbtdm_app` 93 + `libBLE.a` 31), WiFi yığını ~325 KB (`net80211` 107, `lwip` 94,
`wpa_supplicant` 45, `pp` 37, `phy` 31), `libc` 91 KB, mbedcrypto 40 KB, kendi kodumuz 65 KB, rodata 198 KB.
`libmesh`, `mbedtls_x509` ve `libsmartconfig` linklenmiyor. Ölçüm `.pio/build/gate/*.map` + `nm` ile yapıldı.

## Değişmezler

- Radyo kullanımı ESP-NOW ile sınırlı ve kanal iki tarafta da 6'ya sabit.
- Cihazda HTTP sunucusu yok; panel erişimi yalnız BLE GATT.
- ESP-NOW peer'leri şifresizdir; gizlilik/bütünlük uygulama katmanında (XOR anahtar teslimi + HMAC).
- Komut imzasındaki MAC kaynağı iki tarafta aynı olmak zorunda.
- `version_script.py` yalnız gate env'inde çalışır; OTA manifesti tek kapı artefaktını tanımlar.
- Yayın deposunda kök `version.json` bu ürüne ait değildir; kapı yalnız `gate/` alt klasörünü yazar ve
  ürettiği manifestte `"product": "gate"` bulunur. Panel `product` uyuşmayan manifesti reddeder (fail-closed).
- İki firmware birbirinin kodunu include etmez; tek paylaşım `src/common.cpp`.
