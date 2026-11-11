---
id: vehicle-firmware
type: module-design
title: Araç firmware'i (vehicle / ekranli)
status: active
parent: autodoor-architecture
---

# Araç firmware'i (vehicle / ekranli)

## Sorumluluk

Araç ünitesi: telefon panelinin bağlandığı BLE peripheral'ı ve kapıya ESP-NOW komutu gönderen düğüm.
Kapıya yaklaşıldığında "buradayım" (`WARN`) ve "aç" (`OPEN`) komutlarını üretir; eşleştirmede kapının
gönderdiği ortak anahtarı saklar. Env'ler: `vehicle` (lolin_c3_mini) ve `ekranli` (esp32dev,
`-D ENV_EKRANLI`).

## Sınır

- Kapıyı sürmez: motor, ışık, buzzer, OTA ve cihaz yönetimi bu modülde yoktur.
- Paylaştığı tek kod `src/common.cpp`'dir; ilişki kapıyla yalnız `autodoor-architecture`'daki tel
  protokolüdür.
- `ENV_EKRANLI` bugün yalnız iki pini değiştirir; ekran/display kodu içermez — ad tarihseldir.

## Davranış

- **Tetikleyici enerji/kontaktır**: ünite araç kontağıyla enerjilenir (kullanıcı bildirimi) — firmware'de
  kontak girişi yoktur, davranış yalnız enerji verilmesiyle başlar. Enerji kesilince ünite susar; panjur
  `closeTimeout` sonunda kapanır. Bu yüzden `closeTimeout` (sinyal kaybı) kapanmanın ana mekanizması,
  `openLimit` ise güvenlik sınırıdır.
- Durum (`VehicleState`): `VEHICLE_IDLE`, `VEHICLE_WAITING_FOR_GATE_SCAN`. (`VEHICLE_AUTHORIZED_ACTIVE`
  tanımlı ama hiçbir yerde atanmıyor.)
- Butona 3 sn basış (ya da `FORCE_AUTO_SCAN_ON_STARTUP` açıksa boot + 3 sn) → `VEHICLE_WAITING_FOR_GATE_SCAN`.
  Kapıdan `SCAN_REQUEST` gelince `AUTH_ACK` ile cevap verir, `KEY_DELIVERY`'yi PMK ile çözer, anahtarı
  `pairedGates`'e yazar ve `VEHICLE_IDLE`'a döner. **Bu moddan çıkışın tek yolu eşleşmedir** — zaman aşımı
  yoktur (bilinen kısıt).
- `DISCOVERY_PROBE` her durumda `{"deviceName": ...}` ile cevaplanır (panel taraması için).
- Beacon davranışı: eşleşmiş bir kapıdan `BEACON` gelirse `targetGateMac` ve (varsa) `nonce` güncellenir.
  Cihazın açılışından `warnDuration` ms geçmediyse `WARN`, sonrasında `OPEN` gönderilir — yani `warnDuration`
  bir kapanma uyarısı değil, açılışta panjurun istemeden açılmaması için tanınan grace süresidir
  *(doğrulanmadı: bu gerekçe kullanıcı tarafından onaylanmadı)*. Kontak açık kaldıkça beacon 2 sn'de bir
  gelir ve sürekli `OPEN` gider — panjurun açık kalmasını sağlayan şey budur.
- Güvenli komut: beacon `nonce` taşıdıysa `HMAC-SHA256(sharedKey, WiFi.macAddress() + command + nonce)` hex
  olarak eklenir; nonce yoksa "güvensiz" gönderilir ve yetkilendirme açık kapı bunu reddeder.
- Butona kısa basış manuel `OPEN` gönderir; seri porttan `CLOSE` de gönderilebilir ama kapı bu komutu
  işlemez.

## BLE yüzeyi

Araç servisi (`VEHICLE_SERVICE_UUID`):

- **settings** — yazma JSON'u `warnDuration`, `deviceName`, `pinCode`; `pinCode` boş string PIN'i sıfırlar,
  yeni PIN 4 hane (0–9999) doğrulanır. Okuma: `warnDuration`, `deviceName`, `pinExists`, `buildDate`,
  `buildTime`. PIN varsa ve oturum doğrulanmadıysa yazma reddedilir.
- **pin auth** — yazılan ham değer PIN ile karşılaştırılır (JSON değil); PIN boşsa her yazma başarılıdır.
- **status** (notify + `BLE2902`) — `AUTH_SUCCESS`, `AUTH_FAILED`, `AUTH_REQUIRED`, `PIN_CHANGED`,
  `PIN_RESET`, `INVALID_PIN_FORMAT`, `SETTINGS_UPDATED`, `NO_CHANGE`, `JSON_ERROR`.
- Cihaz adı `VEHICLE_` + BLE MAC'in son 4 hanesidir; MTU 200'e ayarlanır. Ad değişikliği yeniden başlatma
  gerektirir (reklam yenilenmiyor).

## Kalıcılık (NVS)

- `settings`: `warnDuration` (varsayılan 10 000 ms), `deviceName`, `pinCode`.
- `paired_gates`: `gates_json` = `[{"mac","key"}]` — kapı MAC'i → hex ortak anahtar.

## Donanım

C3 (lolin_c3_mini): dahili LED 8, buton 9, aktif-LOW. ESP32 düz: LED 2, buton 21, aktif-HIGH.
`ekranli`: LED 17, buton 0, aktif-LOW. LED geri bildirimi `ledQueue` + `LedPattern{duration, count}`
üzerinden core 0'daki görevde yürür (paket alımı 50 ms, beacon 200 ms, eşleşme başarısı uzun yanma).

## Bilinen kısıtlar

- `DeviceSettings.logLevel` hiçbir yerde atanmıyor → tüm `logMessage` çağrıları çöp değerle karşılaştırır.
- `getWarningDuration()` clamp'i çağrılmıyor; NVS'teki `warnDuration` doğrudan kullanılıyor, bozuk değer
  `WARN`/`OPEN` davranışını bozar.
- "Kullan ve sil" peer yönetimi yorumu yanlış: `OnDataSent` içindeki `esp_now_del_peer` yorumlu, peer
  tablosu büyüyor.
- Ölü kod: `isGatePaired`, `parseWarningDuration`, `startTime`, `lastSendTime`, `sendInterval`,
  `isAutoSendingActive`, `manualCommandToSend`, `VEHICLE_AUTHORIZED_ACTIVE`.
- Panel `NO_AUTH_NEEDED` bildirimini bekliyor ama firmware bu string'i hiç göndermiyor.
- Tarama modunda LED görevi `continue` ettiği için kuyruktaki desenler yutuluyor (açlık).
