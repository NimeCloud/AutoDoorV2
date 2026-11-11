---
id: gate-firmware
type: module-design
title: Panjur/garaj kapısı kontrol firmware'i (gate)
status: active
parent: autodoor-architecture
---

# Panjur/garaj kapısı kontrol firmware'i (gate)

## Sorumluluk

Kapıyı fiziksel olarak süren cihaz: araçtan gelen ESP-NOW komutlarını doğrular, motoru çalıştırır,
otomatik kapanma ve kapanma öncesi uyarıyı yürütür, aracın eşleştirilmesini yönetir, BLE üzerinden panel
ayarlarını, kayıtlı araç listesini, PIN kimlik doğrulamasını ve OTA'yı sunar. Çıkış env'i: `gate`
(esp32dev).

## Sınır

- Paylaştığı tek kod `src/common.cpp`'dir; araç firmware'i ile derleme bağı yoktur, ilişki yalnız
  `autodoor-architecture` içindeki ESP-NOW tel protokolüdür.
- Motor (`OPEN_PIN`/`CLOSE_PIN`), ışık, buzzer ve yetkilendirme butonu pinleri bu modülün malıdır.
- `src/web/gate.html` firmware'e gömülü değildir; ayrı bir istemcidir (`web-panels`) ve flash bütçesine
  katkısı yoktur.

## Kapı durum makinesi

`GATE_CLOSED → GATE_OPENING → GATE_IDLE_OPEN → GATE_WARNING → GATE_CLOSING → GATE_CLOSED`.
Ayrı bir FreeRTOS görevi (`stateMachineTask`, core 1) 100 ms'de bir yürütür; LED ayrı görevde (core 0)
kuyruktan desen tüketir.

- `MODE_NORMAL`: komutlarla çalışır. `GATE_IDLE_OPEN`'tan uyarıya geçiş iki koşuldan biriyle — son geçerli
  komuttan `closeTimeout` sn geçmesi (sinyal kaybı) ya da durum değişiminden `openLimit` sn geçmesi
  (güvenlik sınırı). `GATE_WARNING` boyunca kırmızı ışık 1 Hz flaşlar ve buzzer kısa bipler verir;
  `preCloseWarning` sn sonunda kapı kapanır.
- Araç menzilde kaldıkça her 2 sn'de `WARN`/`OPEN` gönderdiği için `lastAuthenticatedCommandTime` tazelenir
  ve kapı açık kalır; araç gidince sayaç dolup uyarı → kapanma başlar.
- `MODE_FORCE_OPEN`, `MODE_FORCE_CLOSE`, `MODE_DISABLED` komutları yok sayar ve motoru zorlar / durdurur.
- `OPEN` komutu kapı kapanırken gelirse motor durdurulur, `MOTOR_COOLDOWN_MS` (1 sn) beklenir ve tekrar
  açılır. `CLOSE` komutu işlenmez — `handleCommand` yalnız `OPEN` ve `WARN` tanır (bilinen kısıt).

## Eşleştirme ve yetkilendirme

Eşleştirme modu (`AuthMode`): `AUTH_IDLE` / `AUTH_SCANNING` / `AUTH_SENDING_KEY` / `AUTH_DISCOVERY_SCAN`.

- Butona 3 sn basış → `AUTH_SCANNING` (5 dk zaman aşımı, 1 sn'de bir `SCAN_REQUEST`). Panelden
  `start_discovery_scan` → `AUTH_DISCOVERY_SCAN` (10 sn, 1 sn'de bir `DISCOVERY_PROBE`).
- `AUTH_ACK` gelince 16 baytlık rastgele anahtar üretilir, `registeredDevices`'e yazılır ve
  `AUTH_SENDING_KEY`'e geçilir: 500 ms'de bir, 5 sn boyunca `KEY_DELIVERY`. `KEY_ACK` gelirse eşleştirme
  kalıcılaşır; gelmezse kayıt silinir. Panelden `pair_new_device` de aynı akışı MAC/nickname vererek başlatır.
- `authorizationRequired` kapalıysa kayıtsız araç da komut gönderebilir ve beacon `nonce` taşımaz.
- Açıkken: araç MAC'i kayıtlı olmalı **ve** HMAC doğrulanmalı (`architecture`'daki imza kuralı). Başarısız
  HMAC sessizce reddedilir, yalnız seri loga yazılır.
- PIN kilidi: 3/5/10 hatalı denemede 10 sn / 1 dk / 15 dk kilit. Sayaç ve kilit zamanı NVS'te tutulur ama
  zaman `millis()` tabanlıdır — reboot sonrası anlamsız bir kilit oluşabilir (bilinen kısıt).

## BLE yüzeyi

Kapı servisi (`GATE_SERVICE_UUID`) ve karakteristikleri:

- **settings** — okuma JSON'u: `deviceName`, `closeTimeout`, `preCloseWarning`, `openLimit`, `opMode`,
  `authReq`, `pinExists`, `updateUrl`, `firmwareVersion`, `rebootRequired`. Yazmada aynı anahtarlar;
  ayrıca `pinCode` alanı varsa PIN değişir ve oturum kimlik doğrulaması düşer. Ad değişikliği
  `reboot_required` bildirimi üretir. PIN varsa ve oturum doğrulanmadıysa okuma/yazma reddedilir.
- **pin auth** — yazılan ham değer PIN ile karşılaştırılır; cevap `AUTH_SUCCESS` / `AUTH_FAILED` /
  `AUTH_LOCKED`. Okuma `{"pinRequired":bool}`.
- **status** (notify + `BLE2902`) — olaylar: `ota_config`, `ota_ready`, `ota_progress`, `ota_ack`,
  `ota_nack`, `update_status` (`type`: info/success/error, isteğe bağlı `reboot`), `device_found`,
  `pairing_complete`, `scan_failed`, `reboot_required`.
- **device management** — okuma kayıtlı araç dizisi (`mac`, `nickname`, `vehicleName`); yazma
  `{"action": ...}`: `start_discovery_scan`, `pair_new_device` (`mac`, `nickname`, `vehicleName`),
  `update_nickname`, `delete_device`.
- **OTA control / OTA data** — akış ve parça biçimi `autodoor-architecture`'da.

Ayarlar BLE'den değiştiğinde `SETTINGS_SAVE_DELAY` (2 sn) sonra NVS'e yazılır.

## Kalıcılık (NVS)

- `gate-settings`: `deviceName`, `closeTimeout`, `preCloseWarn`, `openLimit`, `logLevel`, `authReq`,
  `opMode`, `pinCode`, `updateUrl`, `rebootRequired`.
- `auth_list`: `devices_json` = `[{"mac","key","nickname","vehicleName"}]`.
- `security`: `fails`, `lock_end`.
- Varsayılan cihaz adı `GATE_` + eFuse MAC'in son 6 hanesi (büyük harf).

## Donanım

Pin tabloları `#if CONFIG_IDF_TARGET_ESP32C3` / `#elif CONFIG_IDF_TARGET_ESP32` ile seçilir; `gate` env'i
esp32dev kullanır (LED 2, kırmızı 14, yeşil 12, buzzer 27, motor 4/5, buton 0, aktif-HIGH). C3 varyantı
aktif-LOW'dur. Motor `HIGH` ile sürülür; durdurmada her iki röle de pasiflenir.

## Bilinen kısıtlar

- Ölü kod: `challengeNonces`, `authenticatedVehicles`, `stateMachineTask` içindeki hiç çalışmayan
  `if (false)` eşleştirme zaman aşımı bloğu.
- `setup()` içinde `settings.logLevel = 1` zorlaması üretimde ayrıntılı log açar (seri gürültü + rodata).
- ESP-NOW peer'leri `encrypt = false` olarak eklenir; `KEY_DELIVERY` civarındaki "ESP-NOW bunu PMK ile
  çözer" yorumu yanlıştır — koruma yalnız uygulama katmanındaki XOR'dur. *(doğrulanmadı: bu tasarımın
  bilinçli olduğu kullanıcı tarafından onaylanmadı.)*
- Komut nonce'u tek kullanımlık değildir; tekrar oynatma engellenmez.
