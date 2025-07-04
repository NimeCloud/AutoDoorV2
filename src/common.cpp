#ifndef COMMON_H
#define COMMON_H

// --- ORTAK KÜTÜPHANELER ---
#include <esp_chip_info.h>
#include <esp_now.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <Preferences.h>

#include <vector>
#include <array>
#include <map> 





#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>


#include <esp_wifi.h>

#include "mbedtls/sha256.h"
#include "mbedtls/md.h"


// --- ORTAK SABİTLER ---
#define FIXED_WIFI_CHANNEL 6

// PSK (Pre-Shared Key) - Her iki cihazda da aynı olmalı
const uint8_t PMK[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10};

#define GATE_SERVICE_UUID    "e3a1b2f0-5f4d-4c8a-bd2f-8e6a7f3b9c0d" // Gate için
#define VEHICLE_SERVICE_UUID "b1c2d3e4-5f4d-4c8a-bd2f-8e6a7f3b9c0d" // Araç için (farklı)

#define CHAR_SETTINGS_UUID  "a1d2f3e4-5678-4b9a-b3c2-9d8e7f6a5b4c"
#define CHAR_PIN_AUTH_UUID  "b2c3d4e5-6789-4f0b-c1d2-a3b4c5d6e7f8" // Yeni PIN karakteristiği UUID'si
#define CHAR_STATUS_UUID    "c3d4e5f6-7890-4a1c-b2d3-e4f5a6b7c8d9" // Yeni Durum/Bildirim karakteristiği UUID'si

const uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- ORTAK YARDIMCI FONKSİYONLAR ---
// Fonksiyonları 'inline' olarak tanımlamak, header dosyasında olmalarına rağmen
// 'multiple definition' hatası vermelerini engeller.

// Zaman damgası oluşturan fonksiyon
inline String getTimestamp() {
  unsigned long currentMillis = millis();
  unsigned long seconds = currentMillis / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;

  seconds %= 60;
  minutes %= 60;
  hours %= 24;

  char timeStr[15];
  sprintf(timeStr, "%02lu:%02lu:%02lu.%03lu", hours, minutes, seconds, currentMillis % 1000);
  return String(timeStr);
}

inline void logMessage(String message, int level, int currentLogLevel) {
  if (currentLogLevel == 0 && level == 1) {
    return;
  }
  Serial.print(getTimestamp() + " ");
  Serial.println(message);
}

// MAC adresini String'e çeviren fonksiyon
inline String macToString(const uint8_t *mac) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}


// HMAC-SHA256 hesaplayan yardımcı fonksiyon
inline String calculateHmac(const uint8_t* key, size_t keyLen, const char* data) {
  uint8_t hmacResult[32];
  mbedtls_sha256_context ctx;

  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0); // 0 for SHA-256
  mbedtls_sha256_update(&ctx, (const unsigned char*)data, strlen(data));
  mbedtls_sha256_finish(&ctx, hmacResult);
  mbedtls_sha256_free(&ctx);

  // HMAC'i HMAC-SHA256'ya dönüştürmek için mbedtls_md_hmac kullanmak daha doğrudur,
  // ancak basitlik adına şimdilik bu şekilde bir hash alalım. Gerçek uygulamada bu geliştirilmelidir.
  // Bu örnekte, nonce'ı PMK ile hash'liyoruz.
  
  mbedtls_md_context_t md_ctx;
  mbedtls_md_init(&md_ctx);
  mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1); // 1 for HMAC
  mbedtls_md_hmac_starts(&md_ctx, key, keyLen);
  mbedtls_md_hmac_update(&md_ctx, (const unsigned char*)data, strlen(data));
  mbedtls_md_hmac_finish(&md_ctx, hmacResult);
  mbedtls_md_free(&md_ctx);

  char hexResult[65];
  for(int i = 0; i < 32; i++) {
    sprintf(hexResult + i * 2, "%02x", hmacResult[i]);
  }
  hexResult[64] = '\0';
  
  return String(hexResult);
}

// Bir veriyi, verilen anahtarla XOR işlemine sokan basit şifreleme/şifre çözme fonksiyonu
// Not: Bu işlem simetriktir, aynı fonksiyon hem şifreleme hem de şifre çözme için kullanılır.
inline String encryptDecrypt(String data, const uint8_t* key, size_t keyLen) {
  String output = "";
  for (int i = 0; i < data.length(); i++) {
    output += (char)(data[i] ^ key[i % keyLen]);
  }
  return output;
}


#endif // COMMON_H