---
id: web-panels
type: module-design
title: Tarayıcı panelleri (Web Bluetooth)
status: active
parent: autodoor-architecture
depends-on: [gate-firmware, vehicle-firmware]
---

# Tarayıcı panelleri (Web Bluetooth)

## Sorumluluk

Operatör arayüzü: tarayıcıda çalışan, cihazlara **Web Bluetooth** (`navigator.bluetooth`) ile bağlanan iki
tek dosyalık panel — `gate.html` (kapı) ve `vehicle.html` (araç). Cihaz üstünde sunucu olmadığı için
paneller cihazın BLE GATT servislerine doğrudan istemci olarak bağlanır.

## Sınır

- Firmware'e gömülmez ve flash bütçesine katkısı yoktur; dağıtımı firmware'den bağımsızdır.
- Cihazla tek temas BLE GATT'tir (UUID'ler `src/common.cpp` ile aynı olmak zorunda) ve OTA için public
  depodaki `version.json` / `.bin` dosyaları.
- Kapı paneli OTA istemcisidir; araç panelinde OTA yoktur.

## Kapı paneli (`gate.html`)

- Kapı servisinin `settings`, `pin auth`, `device management`, `status` karakteristiklerini kullanır; JSON
  anahtarları `gate-firmware` spec'indeki listeyle aynıdır (cihaz ve panel aynı sözleşmeyi paylaşır).
- PIN ile kimlik doğrulama: `AUTH_REQUIRED` / `AUTH_SUCCESS` / `AUTH_FAILED` / `AUTH_LOCKED` bildirimlerini
  yorumlar.
- Eşleştirme: `start_discovery_scan` ile tarama, `device_found` ile bulunan aracı onaylatma,
  `pair_new_device` ile anahtar teslimi, `pairing_complete` ile bitiş.
- **OTA istemcisi**: `version.json` → firmware indirme → SHA-256 doğrulama → `ota_control`
  (`start`/`end`/`abort`/`reboot`), `ota_data`'ya 4 bayt CRC32 + parça, `ota_config`'ten gelen
  `windowSize`/`chunkSize`, pencere ACK'i bekleme ve `ota_ready`'deki `progress`/`windowId` ile kopmada
  devam. `updateUrl` alanının varsayılanı bu ürünün kanalıdır: `…/contents/gate/version.json`.
- **Sürüm kontrolü**: `compareVersions()` sonucuna göre yalnız cihaz sürümü küçükse güncelleme önerilir;
  eşitse "Cihazınız güncel", büyükse "cihazınızda daha yeni sürüm var" bilgisi çıkar.
- **Ürün kimliği kapısı**: manifestteki `product` alanı `gate` değilse (veya alan yoksa) panel yükleme
  teklif etmez, hata gösterir (fail-closed). Paylaşılan yayın deposunda yanlış ürünün manifestinin
  yakalandığı yer burasıdır.
- Manifest ve firmware indirme `Accept: application/vnd.github.v3.raw` başlığıyla yapılır (Contents API
  adresi bu başlıkla ham bayt döner), manifest isteğine önbellek kırıcı `?t=<zaman>` eklenir.
- Ayarlar formu, kayıtlı araç listesi (takma ad güncelleme, silme) ve sürüm/hash gösterimi aynı sayfadadır.

## Araç paneli (`vehicle.html`)

- Araç servisinin `settings`, `pin auth`, `status` karakteristikleri: `warnDuration`, cihaz adı ve PIN
  yönetimi, PIN doğrulama, durum bildirimleri. OTA ve cihaz yönetimi yoktur.
- `NO_AUTH_NEEDED` durumunu bekler ama firmware bu bildirimi hiç göndermez (bilinen uyumsuzluk).
