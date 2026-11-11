---
id: autodoor-v2
type: goal-and-requirements
title: AutoDoorV2 — araç kontaklı panjur/garaj kapısı kontrolü
status: active
tags: [esp32, firmware, access-control]
---

# AutoDoorV2 — araç kontaklı panjur/garaj kapısı kontrolü

## Amaç

Araç kontağı açıldığında araçtaki ESP32 devreye girsin ve eşleşmiş panjur/kepenk/garaj kapısına açma sinyali
göndersin; kontak kapanınca sinyal kesilsin ve panjur ayarlanan süre sonunda kendiliğinden kapansın.
Panjuru açan aracın kimliğiyle yetkilendirilsin. Ürün iki ESP32 firmware'inden (araç ünitesi + panjur
kontrol cihazı) ve tarayıcıda çalışan iki panelden oluşur.

## Problem

Kumanda/buton çözümü aracı tanımaz — yetkisiz biri de açabilir — ve sahada WiFi ağı/internet kurulumu
gerektirir. Bu üründe panjur, aracın MAC'ine bağlı imzalı komutla açılır; panel erişimi doğrudan BLE
üzerinden yapılır, sahada ağ kurulmaz.

## Kullanıcılar

- **Araç sahibi** — telefonda tarayıcı paneliyle aracı/kapıyı görür, eşleştirir, elle açma yapar.
- **Kurulumcu/teknisyen** — kapı ayarları, PIN, kayıtlı araç listesi, yeni araç eşleştirme, firmware güncellemesi.

## Yetenekler

- **Panjur kontrolü** (panjur/kepenk/garaj kapısı): araçtan gelen komutlarla motor sürülür; araç sinyali
  kesildiğinde `closeTimeout` sonunda otomatik kapanma, açık kalma için `openLimit` güvenlik sınırı ve
  kapanma öncesi uyarı (kırmızı ışık + buzzer), açık/kapalı LED geri bildirimi.
- **Kontak tetikli çalışma**: araç ünitesi kontakla enerjilenir; araç çalışırken sürekli `WARN`/`OPEN`
  yayınlar, kontak kapanınca yayın kesilir ve panjur kendiliğinden kapanır.
- **Yetkilendirme**: kayıtlı araç MAC'i + HMAC-SHA256 imzalı komut; tek anahtarla "yetkilendirme
  gerekmez" moduna alınabilir.
- **Eşleştirme**: kapı ile araç ESP-NOW el sıkışmasıyla 16 baytlık ortak anahtar üretir; anahtar iki
  tarafta NVS'te saklanır.
- **Araç ünitesi**: kapının beacon'larını izler, `WARN`/`OPEN` komutu gönderir; BLE üzerinden ayarlarını,
  PIN'ini ve eşleşmiş kapılarını yönetir.
- **Paneller**: tarayıcıda Web Bluetooth ile çalışan kapı ve araç panelleri — ayarlar, PIN ile kimlik
  doğrulama, kayıtlı araç yönetimi, kapı panelinde OTA.
- **OTA**: BLE üzerinden parça parça yazma, paket CRC32'si, pencere ACK'leri, bağlantı kopmasında devam
  etme; sürüm/hash bilgisi public depodaki `version.json`'dan.
- **Kalıcılık**: ayarlar, kayıtlı araçlar, eşleşmiş kapılar ve güvenlik sayacı NVS'te; sürüm numarası
  derleme sırasında üretilip seri log ve BLE'de gösterilir.

## Hedef dışı

- **Cihaz üstünde HTTP/web sunucusu yok** — panel erişimi yalnız BLE üzerinden. Gerekçe (kullanıcı
  onaylı): WiFi yığını flash bütçesinde çok yer tutuyor ve sahada WiFi ağı/altyapısı varsayılmıyor.
- Bulut, uzaktan erişim, merkezi çok-cihaz yönetimi.
- Araç ünitesinde motor, ekran, buzzer kontrolü — bunlar panjur kontrol cihazına aittir.

## İlkeler

- **Saha altyapısı bağımsızlığı**: ESP-NOW WiFi radyosunu kullanır ama ağ/altyapı gerektirmez; hiçbir
  çalışma zamanı özelliği sahada WiFi ağı varsaymamalı. Yazılım güncellemesi tarayıcı + public depo
  üzerinden yapılır.
- **Flash kısıtı**: kapı imajı 1 984 KB'lık app bölümünün ~%79'unu dolduruyor (~425 KB boş). Sınır
  artık engelleyici değil; yine de yeni özellik eklerken boyut hesaba katılır.
- **Güvenlik hedefi** *(doğrulanmadı — kullanıcı onaylamadı)*: korunulan tehdit uzaktan saldırgan değil,
  yetkisiz/rastgele bir cihazın kapıyı açmasıdır. PMK firmware'e gömülüdür ve ESP-NOW donanım şifrelemesi
  kullanılmaz; gizlilik/bütünlük uygulama katmanında (anahtar tesliminde XOR, komutlarda HMAC) sağlanır.
