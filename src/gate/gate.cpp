#include <esp_rom_crc.h> // CRC hesaplama için gerekli
#include "common.cpp"
#include <Update.h>

// Key: Araç MAC adresi (String), Value: Gönderilen Nonce (String)
std::map<String, String> challengeNonces;
// Key: Araç MAC adresi (String), Value: Yetkinin sona ereceği zaman (millis())
std::map<String, unsigned long> authenticatedVehicles;
const unsigned long AUTH_DURATION_MS = 300000; // 5 dakika yetki süresi

// Otomatik board tespiti
#if CONFIG_IDF_TARGET_ESP32C3
                                               // ESP32-C3 SuperMini pinleri
#define INTERNAL_LED_PIN 8
#define RED_PIN 9
#define GREEN_PIN 10
#define BUZZER_PIN 11
#define OPEN_PIN 4         // Motoru açmak için tetik pini
#define CLOSE_PIN 5        // Motoru kapatmak için tetik pini
#define AUTH_BUTTON_PIN 21 // Yetkilendirme/Tarama butonu
#define LED_ON LOW
#define LED_OFF HIGH
#elif CONFIG_IDF_TARGET_ESP32
                                               // ESP32-WROOM-32 pinleri
#define INTERNAL_LED_PIN 2
#define RED_PIN 14
#define GREEN_PIN 12
#define BUZZER_PIN 27
#define OPEN_PIN 4        // Motoru açmak için tetik pini
#define CLOSE_PIN 5       // Motoru kapatmak için tetik pini
#define AUTH_BUTTON_PIN 0 // Yetkilendirme/Tarama butonu
#define LED_ON HIGH
#define LED_OFF LOW
#endif

// Sabitler
#define FIXED_WIFI_CHANNEL 6   // WiFi kanalı (1, 6 veya 11 önerilir)
#define MOTOR_COOLDOWN_MS 1000 // 1 saniye motor koruma süresi
#define LIGHT_OFF LOW
#define LIGHT_ON HIGH
#define BUZZER_ON HIGH
#define BUZZER_OFF LOW
#define MOTOR_ACTIVE HIGH
#define MOTOR_INACTIVE LOW
#define DEFAULT_DEVICE_NAME_PREFIX "GATE_" // Varsayılan cihaz adı ön eki
#define DEFAULT_LOG_LEVEL_INFO 0           // 0 = INFO, 1 = VERBOSE

const bool FORCE_AUTO_SCAN_ON_STARTUP = false; // true: otomatik başlar, false: sadece butonla başlar
static bool autoScanTriggered = false;         // Bu özelliğin sadece bir kez çalışmasını sağlar

unsigned long lastAuthenticatedCommandTime = 0; // Son geçerli komutun zamanı
unsigned long warningStartTime = 0;             // <<< BU SATIRI EKLEYİN

// Preferences (NVRAM) nesnesi
Preferences preferences;

// Kayıtlı bir aracın tüm bilgilerini bir arada tutan struct
struct RegisteredVehicleInfo
{
  String sharedKey;
  String nickname;
  String vehicleName;
};

std::map<String, RegisteredVehicleInfo> registeredDevices;

// Durum Makinesi için Enum
enum GateState
{
  GATE_CLOSED,
  GATE_CLOSING,
  GATE_OPENING,
  GATE_IDLE_OPEN,
  GATE_WARNING // Kapanmadan önce uyarı durumu
};

// Yeni Çalışma Modları
enum GateOperationMode
{
  MODE_NORMAL,      // Normal otomatik çalışma (araç sinyaline göre aç/kapat)
  MODE_FORCE_OPEN,  // Sürekli Açık Tut (komutları yoksay)
  MODE_FORCE_CLOSE, // Sürekli Kapalı Tut (komutları yoksay)
  MODE_DISABLED     // Motoru Durdur / Servis Modu (komutları yoksay)
};

// LED Görevi İçin Yapı (Queue Item)
struct LedPattern
{
  int duration; // Her flash'ın süresi (ms)
  int count;    // Flash sayısı
};

// Cihaz Ayarları Yapısı
struct DeviceSettings
{
  char deviceName[32]; // Cihazın adı (BLE reklamında ve seri monitörde kullanılır)
  int closeTimeout;    // Otomatik kapanma süresi (saniye)
  int preCloseWarning;
  int openLimit;              // Açık kalma süresi sınırı (saniye) - güvenlik için
  int logLevel;               // 0 = INFO, 1 = VERBOSE
  bool authorizationRequired; // Yetkilendirme gerekliliği
  GateOperationMode operationMode;
  String pinCode;
  String versionMajor;
  String versionMinor;
  String versionBuild;

  char updateUrl[128];
};

// Global Değişkenler
DeviceSettings settings;
GateState currentState = GATE_CLOSED;
unsigned long stateChangeTime = 0;              // Durum değişikliğinin zaman damgası
unsigned long lastSignalTime = 0;               // Son sinyal alış zaman damgası (otomatik kapanmayı öteleme için)
bool isAutoCloseScheduled = false;              // Otomatik kapanmanın planlanıp planlanmadığı
unsigned long autoCloseScheduledTime = 0;       // Otomatik kapanmanın zaman damgası
bool preCloseWarnLogged = false;                // Kapanmadan önce uyarı mesajının bir kez loglandığını gösterir
bool autoClosePostponedLoggedOnce = false;      // Otomatik kapanmanın ertelendiğinin bir kez loglandığını gösterir
bool isSettingsChanged = false;                 // Ayarların değişip değişmediğini kontrol eden bayrak
unsigned long lastSettingsChangeTime = 0;       // Ayarların en son değiştiği zaman
const unsigned long SETTINGS_SAVE_DELAY = 5000; // Ayarları kaydetmek için bekleme süresi (ms)
bool isDeviceNameChanged = false;               // Cihaz adının değişip değişmediğini kontrol eden bayrak
bool shouldBeep = false;                        // Motor hareket halindeyken bip sesini kontrol eder

unsigned long discoveryScanStartTime = 0; // Eşleşme taraması için başlangıç zamanı

// OTA Güncelleme Değişkenleri
const int OTA_PACKET_WINDOW_SIZE = 20; // Her 20 pakette bir ACK gönderilecek
const int OTA_CHUNK_SIZE = 500;        // Paket boyutu (500 byte)
int last_ota_progress_percent = 0;
int ota_packets_received_in_window = 0;

struct SecurityState
{
  int pinFailedAttempts = 0;
  unsigned long pinLockoutEndTime = 0;
};
SecurityState securityState; // Güvenlik durumunu yönetecek global nesne

// BLE Karakteristikleri için global değişkenler
BLECharacteristic *pSettingsChar;
BLECharacteristic *pStatusChar;
BLECharacteristic *pPinAuthChar;
bool isAuthenticated = false;
bool isUpdateInProgress = false;
unsigned long ota_progress = 0;
unsigned long ota_total_size = 0;

// Beacon yayını için zamanlama değişkenleri
unsigned long lastBeaconTime = 0;
const unsigned long BEACON_INTERVAL_MS = 2000; // 2 saniyede bir yayın yapacak şekilde ayarlandı

// FreeRTOS Görev ve Kuyruk Tanımları
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t stateTaskHandle = NULL;
QueueHandle_t ledQueue;

// Yetkilendirme Modu ve Durum Yönetimi
unsigned long authButtonPressStartTime = 0;
bool authButtonPressed = false;                     // Bu, dahili olarak buton durumunu tutar.
bool longPressActionTriggered = false;              // <<< YENİ SATIR: Uzun basış eyleminin tetiklendiğini işaretler
const unsigned long LONG_PRESS_THRESHOLD_MS = 3000; // 3 saniye uzun basış eşiği (tarama modu için)
unsigned long scanModeEndTime = 0;
const unsigned long SCAN_DURATION_MS = 300000;    // 30 saniye tarama süresi
const unsigned long BROADCAST_INTERVAL_MS = 1000; // Her 1 saniyede bir broadcast
unsigned long lastScanBroadcastTime = 0;

// Eşleşme protokolü için yeni durumlar ve değişkenler
enum AuthMode
{
  AUTH_IDLE,
  AUTH_SCANNING,
  AUTH_SENDING_KEY,
  AUTH_DISCOVERY_SCAN
};

String foundVehicleMac = "";
String foundVehicleName = "";

volatile AuthMode currentAuthMode = AUTH_IDLE;
String pendingVehicleMac = "";                   // Teyit beklenen aracın MAC adresi
String pendingEncryptedKey = "";                 // Gönderilecek şifreli anahtar
unsigned long keySendStartTime = 0;              // Tekrarlı gönderimin başlangıç zamanı
unsigned long lastKeySendTime = 0;               // Son gönderimin zamanı
const unsigned long KEY_SEND_DURATION_MS = 5000; // 5 saniye boyunca gönder
const unsigned long KEY_SEND_INTERVAL_MS = 500;  // 500ms aralıklarla gönder

// Forward Declarations (ileride tanımlanacak fonksiyonların önden bildirimleri)
void updateLights();
void printSettingsToSerial();
void triggerMotor(bool open);
void stopMotor(bool isManualOverride);
void addMacToRegistereds(const uint8_t *mac);
bool isMacRegistered(const uint8_t *mac);

// ========================= Yardımcı Fonksiyonlar =========================
void saveSecurityState()
{
  preferences.begin("security", false); // "security" adında yeni bir alan aç (yazma)
  preferences.putInt("fails", securityState.pinFailedAttempts);
  preferences.putULong("lock_end", securityState.pinLockoutEndTime);
  preferences.end();
}

void loadSecurityState()
{
  preferences.begin("security", true); // "security" alanını aç (okuma)
  securityState.pinFailedAttempts = preferences.getInt("fails", 0);
  securityState.pinLockoutEndTime = preferences.getULong("lock_end", 0);
  preferences.end();
  logMessage("Loaded security state: " + String(securityState.pinFailedAttempts) + " fails.", 1, settings.logLevel);
}

void printSettingsToSerial()
{
  logMessage("--- Device Settings ---", 0, settings.logLevel);
  logMessage("Device Name: " + String(settings.deviceName), 0, settings.logLevel);
  logMessage("Version: " + getFirmwareVersion(), 0, settings.logLevel);

  // operationMode enum'unu okunabilir bir metne çevir
  String opModeStr;
  switch (settings.operationMode)
  {
  case MODE_NORMAL:
    opModeStr = "Normal (Automatic)";
    break;
  case MODE_FORCE_OPEN:
    opModeStr = "Force Open";
    break;
  case MODE_FORCE_CLOSE:
    opModeStr = "Force Close";
    break;
  case MODE_DISABLED:
    opModeStr = "Disabled";
    break;
  default:
    opModeStr = "Unknown";
    break;
  }
  logMessage("Operation Mode: " + opModeStr, 0, settings.logLevel);

  // Güvenlik ayarları
  logMessage("Authorization Required: " + String(settings.authorizationRequired ? "Yes" : "No"), 0, settings.logLevel);
  // Güvenlik için PIN'in kendisi yerine sadece var olup olmadığını yazdır
  logMessage("PIN Set: " + String(settings.pinCode.isEmpty() ? "No" : "Yes"), 0, settings.logLevel);

  // Zamanlama ayarları
  logMessage("Auto-close Timeout (Signal Loss): " + String(settings.closeTimeout) + "s", 0, settings.logLevel);
  logMessage("Pre-close Warning: " + String(settings.preCloseWarning) + "s", 0, settings.logLevel);
  logMessage("Motor Run Limit: " + String(settings.openLimit) + "s", 0, settings.logLevel);

  // Diğer ayarlar
  logMessage("Log Level: " + String(settings.logLevel == 0 ? "INFO" : "VERBOSE"), 0, settings.logLevel);

  logMessage("-----------------------", 0, settings.logLevel);
}

// ========================= NVRAM Yönetimi =========================

void loadSettings()
{
  preferences.begin("gate-settings", true);

  preferences.getString("deviceName", settings.deviceName, sizeof(settings.deviceName));
  if (strlen(settings.deviceName) == 0)
  {
    String defaultName = DEFAULT_DEVICE_NAME_PREFIX + String(ESP.getEfuseMac(), HEX).substring(6);
    defaultName.toUpperCase();
    strncpy(settings.deviceName, defaultName.c_str(), sizeof(settings.deviceName));
  }

  settings.closeTimeout = preferences.getInt("closeTimeout", 30);
  settings.preCloseWarning = preferences.getInt("preCloseWarn", 5);

  settings.openLimit = preferences.getInt("openLimit", 300);
  settings.logLevel = preferences.getInt("logLevel", DEFAULT_LOG_LEVEL_INFO);
  settings.authorizationRequired = preferences.getBool("authReq", true);

  settings.operationMode = (GateOperationMode)preferences.getInt("opMode", MODE_NORMAL);
  settings.pinCode = preferences.getString("pinCode", "");

  preferences.getString("updateUrl", settings.updateUrl, sizeof(settings.updateUrl));

  preferences.end();
  logMessage("Settings loaded from NVRAM.", 0, settings.logLevel);
}

void saveSettings()
{
  preferences.begin("gate-settings", false);

  preferences.putString("deviceName", settings.deviceName);
  preferences.putInt("closeTimeout", settings.closeTimeout);
  preferences.putInt("preCloseWarn", settings.preCloseWarning);

  preferences.putInt("openLimit", settings.openLimit);
  preferences.putInt("logLevel", settings.logLevel);
  preferences.putBool("authReq", settings.authorizationRequired);

  preferences.putInt("opMode", settings.operationMode);
  preferences.putString("pinCode", settings.pinCode);

  preferences.putString("updateUrl", settings.updateUrl);

  preferences.end();
  isSettingsChanged = false;
  logMessage("Settings saved to NVRAM.", 0, settings.logLevel);
}

void saveRegisteredDevices()
{
  preferences.begin("auth_list", false);

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();

  for (auto const &pair : registeredDevices)
  {
    JsonObject device = array.add<JsonObject>();
    device["mac"] = pair.first;                      // MAC adresi
    device["key"] = pair.second.sharedKey;           // Anahtar
    device["nickname"] = pair.second.nickname;       // Takma isim
    device["vehicleName"] = pair.second.vehicleName; // <<< yeni
  }

  String jsonOutput;
  serializeJson(doc, jsonOutput);
  preferences.putString("devices_json", jsonOutput);

  preferences.end();
  logMessage("Saved " + String(registeredDevices.size()) + " registered devices.", 0, settings.logLevel);
}

void loadRegisteredDevices()
{
  registeredDevices.clear();
  preferences.begin("auth_list", true);

  String jsonInput = preferences.getString("devices_json", "[]"); // Varsayılan olarak boş dizi

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonInput);

  if (!error)
  {
    JsonArray array = doc.as<JsonArray>();
    for (JsonObject device : array)
    {
      String mac = device["mac"];

      RegisteredVehicleInfo info;
      info.sharedKey = device["key"].as<String>();
      info.nickname = device["nickname"].as<String>();
      info.vehicleName = device["vehicleName"].as<String>(); // <<< yeni

      registeredDevices[mac] = info;
    }
  }

  preferences.end();
  logMessage("Loaded " + String(registeredDevices.size()) + " registered devices.", 0, settings.logLevel);
}

bool isMacRegistered(const String &mac)
{
  return registeredDevices.count(mac) > 0;
}

void printRegisteredDevices()
{
  logMessage("--- Registered Devices (MAC -> Vehicle name | Nickname | Key) ---", 0, settings.logLevel);
  if (registeredDevices.empty())
  {
    logMessage("  No registered devices found.", 0, settings.logLevel);
  }
  else
  {
    for (auto const &pair : registeredDevices)
    {
      String mac = pair.first;
      RegisteredVehicleInfo info = pair.second;
      logMessage("  - " + mac + " -> " + info.vehicleName + " | " + info.nickname + " | " + info.sharedKey.substring(0, 8) + "...", 0, settings.logLevel);
    }
  }
  logMessage("-------------------------------------------------", 0, settings.logLevel);
}

// ========================= OTA & BLE Karakteristikleri ve Callback'ler =========================

void sendUpdateStatus(const String &message, const String &type, bool reboot)
{
  if (pStatusChar)
  {
    JsonDocument doc;
    doc["event"] = "update_status";
    doc["message"] = message;
    doc["type"] = type; // "info", "success", "error"
    if (reboot)
    {
      doc["reboot"] = true;
    }

    String output;
    serializeJson(doc, output);
    pStatusChar->setValue(output.c_str());
    pStatusChar->notify();
  }
}

// gate.cpp -> Bu sınıfı mevcut olanla tamamen değiştirin
class OtaControlCallbacks : public BLECharacteristicCallbacks
{

  // gate.cpp -> OtaControlCallbacks::onWrite fonksiyonunu bununla tamamen değiştirin
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    JsonDocument doc;
    deserializeJson(doc, value);

    String command = doc["command"];
    logMessage("OTA Control Command: " + command, 0, settings.logLevel);

    if (command == "start")
    {
      if (isUpdateInProgress)
      {
        logMessage("OTA Error: Update already in progress.", 0, settings.logLevel);
        return;
      }

      // NVS'i kontrol et, ama sadece loglamak için.
      preferences.begin("ota_state", true);
      unsigned long previous_progress = preferences.getULong("ota_prog", 0);
      preferences.end();
      if (previous_progress > 0)
      {
        logMessage("Incomplete update found from a previous session. Starting fresh for safety.", 0, settings.logLevel);
      }

      // --- ÖNEMLİ DÜZELTME ---
      // Her yeni 'start' komutunda, ilerlemeyi her zaman sıfırla.
      // Bu, hard reset sonrası yaşanacak "Update.write" hatasını engeller.
      ota_progress = 0;
      last_ota_progress_percent = 0;
      ota_packets_received_in_window = 0;

      // NVS'teki kaydı da temizle
      preferences.begin("ota_state", false);
      preferences.putULong("ota_prog", 0);
      preferences.end();

      ota_total_size = doc["size"];
      if (Update.begin(ota_total_size))
      {
        isUpdateInProgress = true;
        if (pStatusChar)
        {
          JsonDocument readyDoc;
          readyDoc["event"] = "ota_ready";
          readyDoc["progress"] = 0; // Her zaman 0 gönder
          String output;
          serializeJson(readyDoc, output);
          pStatusChar->setValue(output.c_str());
          pStatusChar->notify();
        }
        logMessage("OTA Ready. Starting new update from scratch. Total size: " + String(ota_total_size), 0, settings.logLevel);
      }
      else
      {
        logMessage("OTA Error: Not enough space. Error: " + String(Update.errorString()), 0, settings.logLevel);
        sendUpdateStatus("Yetersiz alan", "error", false);
      }
    }
    else if (command == "end")
    {
      if (!isUpdateInProgress)
        return;

      if (Update.end(true))
      {
        isUpdateInProgress = false;
        preferences.begin("ota_state", false);
        preferences.putULong("ota_prog", 0); // Başarılı, ilerlemeyi sıfırla
        preferences.end();

        logMessage("OTA Success! Rebooting...", 0, settings.logLevel);
        sendUpdateStatus("Güncelleme başarılı! Cihaz yeniden başlatılıyor...", "success", true);
        delay(100);
        ESP.restart();
      }
      else
      {
        isUpdateInProgress = false; // Hata durumunda da ilerlemeyi durdur
        logMessage("OTA Error on Update.end(). Error: " + String(Update.errorString()), 0, settings.logLevel);
        sendUpdateStatus("Doğrulama hatası: " + String(Update.errorString()), "error", false);
      }
    }
    else if (command == "abort")
    {
      isUpdateInProgress = false;
      Update.abort();
      preferences.begin("ota_state", false);
      // TODO: İptal edildiğinde ilerlemeyi sıfırlama
      preferences.putULong("ota_prog", 0); // İptal edildi, ilerlemeyi sıfırla
      preferences.end();
      logMessage("OTA Aborted by client.", 0, settings.logLevel);
    }
  }

}; // OtaControlCallbacks

class OtaDataCallbacks : public BLECharacteristicCallbacks
{

  // Bu fonksiyonu C++ dosyanızdaki mevcut olanla değiştirin
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    if (!isUpdateInProgress)
      return;

    std::string value = pCharacteristic->getValue();
    const uint8_t *pData = (const uint8_t *)value.data();
    size_t len = value.length();

    if (len <= 4)
    {
      logMessage("OTA Error: Invalid packet received (too small).", 0, settings.logLevel);
      return;
    }

    uint32_t received_crc;
    memcpy(&received_crc, pData, 4);

    const uint8_t *firmware_chunk = pData + 4;
    size_t firmware_len = len - 4;
    uint32_t calculated_crc = esp_rom_crc32_le(0, firmware_chunk, firmware_len);

    if (received_crc != calculated_crc)
    {
      logMessage("OTA CRC Mismatch! Requesting resend.", 0, settings.logLevel);
      if (pStatusChar)
      {
        JsonDocument nackDoc;
        nackDoc["event"] = "ota_nack";
        String output;
        serializeJson(nackDoc, output);
        pStatusChar->setValue(output.c_str());
        pStatusChar->notify();
      }
      return;
    }

    if (Update.write((uint8_t *)firmware_chunk, firmware_len) != firmware_len)
    {
      Update.abort();
      isUpdateInProgress = false;
      logMessage("OTA Error on write.", 0, settings.logLevel);
      sendUpdateStatus("Yazma sırasında hata oluştu: " + String(Update.errorString()), "error", false);
      return;
    }

    ota_progress += firmware_len;
    ota_packets_received_in_window++;

    int current_percent = ((long long)ota_progress * 100) / ota_total_size;

    if (current_percent >= last_ota_progress_percent + 1 || ota_progress == ota_total_size)
    {
      Serial.printf("OTA Progress: %d%%\n", current_percent);

      if (pStatusChar)
      {
        JsonDocument doc;
        doc["event"] = "ota_progress";
        doc["progress"] = current_percent;
        String output;
        serializeJson(doc, output);
        pStatusChar->setValue(output.c_str());
        pStatusChar->notify();
      }
      last_ota_progress_percent = current_percent;
    }

    if (ota_packets_received_in_window >= OTA_PACKET_WINDOW_SIZE || ota_progress == ota_total_size)
    {
      if (pStatusChar)
      {
        JsonDocument ackDoc;
        ackDoc["event"] = "ota_ack";
        String output;
        serializeJson(ackDoc, output);
        pStatusChar->setValue(output.c_str());
        pStatusChar->notify();
      }
      ota_packets_received_in_window = 0;

      preferences.begin("ota_state", false);
      preferences.putULong("ota_prog", ota_progress);
      preferences.end();

      // <<< YENİ LOG (Seviye 1: Verbose) >>>
      logMessage("Progress " + String(ota_progress) + " saved to NVS.", 1, settings.logLevel);
    }
  }
};

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onRead(BLECharacteristic *pCharacteristic)
  {
    JsonDocument jsonDoc;
    jsonDoc["deviceName"] = settings.deviceName;
    jsonDoc["closeTimeout"] = settings.closeTimeout;
    jsonDoc["preCloseWarning"] = settings.preCloseWarning;
    jsonDoc["openLimit"] = settings.openLimit;
    jsonDoc["opMode"] = settings.operationMode;
    jsonDoc["authReq"] = settings.authorizationRequired;

    String jsonString;
    serializeJson(jsonDoc, jsonString);
    pCharacteristic->setValue(jsonString.c_str());
  }

  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    if (!value.empty())
    {
      JsonDocument jsonDoc;
      deserializeJson(jsonDoc, value);

      // Gelen JSON'dan ayarları oku ve settings nesnesini güncelle
      if (jsonDoc["deviceName"].is<const char *>())
      {
        strlcpy(settings.deviceName, jsonDoc["deviceName"], sizeof(settings.deviceName));
      }
      if (jsonDoc["closeTimeout"].is<int>())
      {
        settings.closeTimeout = jsonDoc["closeTimeout"];
      }
      if (jsonDoc["preCloseWarning"].is<int>())
      {
        settings.preCloseWarning = jsonDoc["preCloseWarning"];
      }
      if (jsonDoc["opMode"].is<int>())
      {
        settings.operationMode = (GateOperationMode)jsonDoc["opMode"].as<int>();
      }
      if (jsonDoc["authReq"].is<bool>())
      {
        settings.authorizationRequired = jsonDoc["authReq"];
      }

      isSettingsChanged = true; // Değişiklik olduğunu işaretle
      lastSettingsChangeTime = millis();
    }
  }
};

class DeviceManagementCallbacks : public BLECharacteristicCallbacks
{
  void onRead(BLECharacteristic *pCharacteristic)
  {
    // Bu karakteristiğe okuma isteği geldiğinde, kayıtlı cihaz listesini JSON olarak gönder.
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    for (auto const &pair : registeredDevices)
    {
      JsonObject device = array.add<JsonObject>();
      device["mac"] = pair.first;
      device["nickname"] = pair.second.nickname;
      device["vehicleName"] = pair.second.vehicleName; // <<< YENİ SATIR
    }
    String jsonString;
    serializeJson(doc, jsonString);
    pCharacteristic->setValue(jsonString.c_str());
    logMessage("Sent device list over BLE.", 1, settings.logLevel);
  }

  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0)
    {
      JsonDocument doc;
      deserializeJson(doc, value);

      const char *action = doc["action"];
      if (!action)
        return;

      if (strcmp(action, "start_discovery_scan") == 0)
      {
        // Panelden "Tara" komutu geldi
        if (currentAuthMode == AUTH_IDLE)
        {
          currentAuthMode = AUTH_DISCOVERY_SCAN;
          lastScanBroadcastTime = 0; // Taramayı hemen başlatmak için
          discoveryScanStartTime = millis();
          logMessage("Discovery scan started from panel.", 0, settings.logLevel);
        }
      }
      else if (strcmp(action, "pair_new_device") == 0)
      {
        // Panelden gelen MAC adresi ve takma isimle tam bir eşleşme süreci başlat
        String mac = doc["mac"];
        String nickname = doc["nickname"];
        String vehicleName = doc["vehicleName"];

        if (!mac.isEmpty())
        {
          uint8_t macBytes[6];
          sscanf(mac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &macBytes[0], &macBytes[1], &macBytes[2], &macBytes[3], &macBytes[4], &macBytes[5]);

          // Aracı geçici peer olarak ekle
          esp_now_peer_info_t peerInfo = {};
          memcpy(peerInfo.peer_addr, macBytes, 6);
          peerInfo.channel = FIXED_WIFI_CHANNEL;
          peerInfo.encrypt = false;
          memcpy(peerInfo.lmk, PMK, 16); // Peer'e anahtar bilgisini ekle
          // Peer
          if (!esp_now_is_peer_exist(macBytes))
          {
            esp_now_add_peer(&peerInfo);
          }

          // Yeni anahtar oluştur ve kaydet
          uint8_t newSharedKey[16];
          for (int i = 0; i < 16; i++)
          {
            newSharedKey[i] = esp_random() % 256;
          }
          char newSharedKeyHex[33];
          for (int i = 0; i < 16; i++)
          {
            sprintf(newSharedKeyHex + i * 2, "%02x", newSharedKey[i]);
          }

          RegisteredVehicleInfo newVehicle;
          newVehicle.sharedKey = String(newSharedKeyHex);
          newVehicle.nickname = nickname;
          newVehicle.vehicleName = vehicleName;
          registeredDevices[mac] = newVehicle;
          saveRegisteredDevices();

          // Anahtar teslim sürecini başlat
          String keyToSend = String((char *)newSharedKey, 16);
          pendingEncryptedKey = encryptDecrypt(keyToSend, PMK, sizeof(PMK));
          pendingVehicleMac = mac;
          currentAuthMode = AUTH_SENDING_KEY;
          keySendStartTime = millis();
          lastKeySendTime = 0;
          logMessage("Pairing new device from panel: " + mac, 0, settings.logLevel);
        }
      }
      else if (strcmp(action, "update_nickname") == 0)
      {
        String mac = doc["mac"];
        String nickname = doc["nickname"];
        if (registeredDevices.count(mac))
        {
          registeredDevices[mac].nickname = nickname;
          saveRegisteredDevices();
          logMessage("Updated nickname for " + mac, 0, settings.logLevel);
        }
      }
      else if (strcmp(action, "delete_device") == 0)
      {
        String mac = doc["mac"];
        if (registeredDevices.count(mac))
        {
          registeredDevices.erase(mac);
          saveRegisteredDevices();
          logMessage("Deleted device " + mac, 0, settings.logLevel);
        }
      }
    }
  }
};

// ========================= Motor ve Kapı Durumu Fonksiyonları =========================

void triggerMotor(bool open)
{
  if (open)
  {
    digitalWrite(OPEN_PIN, MOTOR_ACTIVE);
    digitalWrite(CLOSE_PIN, MOTOR_INACTIVE);
    logMessage("Triggering motor to OPEN.", 0, settings.logLevel);
  }
  else
  {
    digitalWrite(CLOSE_PIN, MOTOR_ACTIVE);
    digitalWrite(OPEN_PIN, MOTOR_INACTIVE);
    logMessage("Triggering motor to CLOSE.", 0, settings.logLevel);
  }
  stateChangeTime = millis();
  shouldBeep = true;
}

void stopMotor(bool isManualOverride)
{
  digitalWrite(OPEN_PIN, MOTOR_INACTIVE);
  digitalWrite(CLOSE_PIN, MOTOR_INACTIVE);
  shouldBeep = false;
  if (isManualOverride)
  {
    logMessage("Motor stopped by manual override.", 0, settings.logLevel);
  }
  else
  {
    logMessage("Motor stopped by limit.", 0, settings.logLevel);
  }
  vTaskDelay(pdMS_TO_TICKS(MOTOR_COOLDOWN_MS));
}

void updateLights()
{
  if (currentState == GATE_IDLE_OPEN || currentState == GATE_OPENING)
  {
    digitalWrite(GREEN_PIN, LIGHT_ON);
    digitalWrite(RED_PIN, LIGHT_OFF);
  }
  else if (currentState == GATE_CLOSING)
  {
    digitalWrite(GREEN_PIN, LIGHT_OFF);
    digitalWrite(RED_PIN, LIGHT_ON);
  }
  else if (currentState == GATE_CLOSED || currentState == GATE_WARNING)
  {
    digitalWrite(GREEN_PIN, LIGHT_OFF);
    digitalWrite(RED_PIN, LIGHT_OFF);
  }
}

// ========================= Görevler (Tasks) =========================

// gate.cpp

void ledTask(void *pvParameters)
{
  LedPattern currentPattern;
  static unsigned long lastHeartbeatTime = 0;

  while (true)
  {
    unsigned long currentTime = millis();

    // 1. Öncelik: Tarama modunu kontrol et
    if (currentAuthMode == AUTH_SCANNING)
    {
      digitalWrite(INTERNAL_LED_PIN, LED_ON);
      vTaskDelay(pdMS_TO_TICKS(1000));
      digitalWrite(INTERNAL_LED_PIN, LED_OFF);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue; // Tarama modunda kalmak için döngüye devam et
    }

    // 2. Öncelik: Kuyruktan gelen özel LED desenlerini kontrol et
    if (xQueueReceive(ledQueue, &currentPattern, pdMS_TO_TICKS(20)) == pdPASS)
    {
      for (int i = 0; i < currentPattern.count; i++)
      {
        digitalWrite(INTERNAL_LED_PIN, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(currentPattern.duration));

        // Eğer bu tekli bir "uzun yanma" deseni DEĞİLSE veya son flaş ise LED'i kapat
        // currentPattern.count > 1 ise normal flaş dizisidir, her flaş sonrası kapat.
        // currentPattern.count == 1 ise tek bir "uzun yanma"dır, bu durumda döngünün sonunda kapatılır.
        if (currentPattern.count > 1 || i == currentPattern.count - 1)
        {
          digitalWrite(INTERNAL_LED_PIN, LED_OFF);
        }

        if (i < currentPattern.count - 1)
        {
          vTaskDelay(pdMS_TO_TICKS(currentPattern.duration));
        }
      }
      // Özellikle tekli uzun yanmalarda, for döngüsünden çıktıktan sonra LED'in kapandığından emin olun.
      // Yukarıdaki if bloğu bunu zaten sağlamalı, ancak bir güvenlik katmanı olarak düşünülebilir.
      // digitalWrite(INTERNAL_LED_PIN, LED_OFF); // Bu satır genellikle gerekmez, çünkü for döngüsü yönetmeli.
    }
    else
    {
      // 3. Idle durumunda LED kapalı tut
      digitalWrite(INTERNAL_LED_PIN, LED_OFF);

      // 4. Heartbeat: 10 saniyede bir 50ms yan
      if (currentTime - lastHeartbeatTime > 10000)
      {
        digitalWrite(INTERNAL_LED_PIN, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite(INTERNAL_LED_PIN, LED_OFF);
        lastHeartbeatTime = currentTime;
      }
    }
  }
}

void stateMachineTask(void *pvParameters)
{
  while (true)
  {
    unsigned long currentTime = millis();

    // Ana kontrol: Cihazın genel çalışma moduna göre ne yapacağına karar ver.
    switch (settings.operationMode)
    {

    case MODE_FORCE_OPEN:
      // ZORUNLU AÇIK MODU: Kapı kapalıysa aç, açıksa açık bırak.
      if (currentState != GATE_IDLE_OPEN && currentState != GATE_OPENING)
      {
        logMessage("FORCE_OPEN mode active. Opening gate.", 0, settings.logLevel);
        currentState = GATE_OPENING;
        triggerMotor(true);
        updateLights();
      }
      break;

    case MODE_FORCE_CLOSE:
      // ZORUNLU KAPALI MODU: Kapı açıksa kapat, kapalıysa kapalı bırak.
      if (currentState != GATE_CLOSED && currentState != GATE_CLOSING)
      {
        logMessage("FORCE_CLOSE mode active. Closing gate.", 0, settings.logLevel);
        currentState = GATE_CLOSING;
        triggerMotor(false);
        updateLights();
      }
      break;

    case MODE_DISABLED:
      // SERVİS DIŞI MODU: Motoru hemen durdur.
      if (currentState == GATE_OPENING || currentState == GATE_CLOSING)
      {
        logMessage("DISABLED mode active. Stopping motor.", 0, settings.logLevel);
        stopMotor(true);
        currentState = GATE_IDLE_OPEN; // Durduğu pozisyonu "açık" kabul edelim
        updateLights();
      }
      break;

    case MODE_NORMAL:
      // NORMAL OTOMATİK MOD: Sadece bu moddayken sinyal kesintisini ve limitleri kontrol et.
      switch (currentState)
      {
      case GATE_IDLE_OPEN:
        // Sinyal kaybı veya openLimit zaman aşımlarını burada kontrol et
        if ((lastAuthenticatedCommandTime > 0 && (currentTime - lastAuthenticatedCommandTime > (settings.closeTimeout * 1000UL))) ||
            (currentTime - stateChangeTime >= (settings.openLimit * 1000UL)))
        {

          if (currentTime - stateChangeTime >= (settings.openLimit * 1000UL))
            logMessage("Open limit reached. Starting pre-close warning.", 0, settings.logLevel);
          else
            logMessage("Loss of signal detected. Starting pre-close warning.", 0, settings.logLevel);

          currentState = GATE_WARNING;
          warningStartTime = currentTime;
        }
        break;

      case GATE_WARNING:
        // Kapanma öncesi uyarı verme mantığı
        digitalWrite(RED_PIN, (currentTime % 1000 < 500) ? LIGHT_ON : LIGHT_OFF);
        digitalWrite(BUZZER_PIN, (currentTime % 1000 < 100) ? BUZZER_ON : BUZZER_OFF);
        if (currentTime - warningStartTime > (settings.preCloseWarning * 1000UL))
        {
          logMessage("Pre-close warning finished. Closing gate.", 0, settings.logLevel);
          currentState = GATE_CLOSING;
          triggerMotor(false);
          updateLights();
          lastAuthenticatedCommandTime = 0;
        }
        break;

        // Diğer durumlar (OPENING, CLOSING, CLOSED) kendi içinde yönetiliyor veya bir şey yapmıyor.
      }
      break;
    }

    // Motor hareket halindeyken sürekli bip sesi (bu genel kontrol kalabilir)
    shouldBeep = (currentState == GATE_OPENING || currentState == GATE_CLOSING);
    if (shouldBeep)
    {
      digitalWrite(BUZZER_PIN, BUZZER_ON);
    }
    else if (currentState != GATE_WARNING)
    {
      digitalWrite(BUZZER_PIN, BUZZER_OFF);
    }

    if (false)
    {
      // Tarama sonunda eşleşme onayı sonrası zamanlayıcı sıfırlanınca işletilecek kod
      if (currentAuthMode == AUTH_SENDING_KEY && keySendStartTime != 0 &&
          currentTime - keySendStartTime > KEY_SEND_DURATION_MS)
      {
        logMessage("KEY_DELIVERY timeout for " + pendingVehicleMac + ". Pairing failed.", 0, settings.logLevel);
        uint8_t targetMac[6];
        sscanf(pendingVehicleMac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
               &targetMac[0], &targetMac[1], &targetMac[2],
               &targetMac[3], &targetMac[4], &targetMac[5]);
        esp_now_del_peer(targetMac);
        registeredDevices.erase(pendingVehicleMac);
        saveRegisteredDevices();
        currentAuthMode = AUTH_IDLE;
        keySendStartTime = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ========================= ESP-NOW Callback Fonksiyonları =========================

// ESP-NOW Broadcast Gönderme Fonksiyonu
void sendScanRequestBroadcast()
{
  // Basit bir JSON mesajı oluşturalım
  JsonDocument doc;
  doc["msgType"] = "SCAN_REQUEST";

  String output;
  serializeJson(doc, output);

  esp_err_t result = esp_now_send(broadcastAddress, (const uint8_t *)output.c_str(), output.length());

  if (result == ESP_OK)
  {
    // logMessage("SCAN_REQUEST broadcast succesfully sent.", 0, settings.logLevel);
    //  Başarılı gönderimde LED'i bir kez yakıp söndür
    LedPattern p = {100, 1};
    xQueueSend(ledQueue, &p, 0);
  }
  else
  {
    logMessage("Error sending SCAN_REQUEST broadcast. Code: " + String(result), 0, settings.logLevel);
  }
}

// Gelen komutları işleyen merkezi fonksiyon
// Gelen komutları işleyen merkezi fonksiyon
void handleCommand(const char *command)
{
  // --- OPEN Komutu Mantığı ---
  if (strcmp(command, "OPEN") == 0)
  {
    // Durum 1: Kapı kapalı veya kapanmak üzere uyarı veriyorsa, kapıyı aç.
    if (currentState == GATE_CLOSED || currentState == GATE_WARNING)
    {
      currentState = GATE_OPENING;
      triggerMotor(true);
      updateLights();
      logMessage("Command: OPEN. Gate is opening...", 0, settings.logLevel);
    }
    // Durum 2: Kapı tam kapanırken araç geri geldiyse, durdur ve tekrar aç.
    else if (currentState == GATE_CLOSING)
    {
      logMessage("Command: OPEN received while closing. Re-opening gate.", 0, settings.logLevel);
      stopMotor(true);                              // Motoru durdur
      vTaskDelay(pdMS_TO_TICKS(MOTOR_COOLDOWN_MS)); // Kısa bir bekleme
      currentState = GATE_OPENING;
      triggerMotor(true); // Tekrar aç
      updateLights();
    }
    // Durum 3: Kapı zaten açık veya açılıyorsa, hiçbir şey yapma.
    // Sadece log basarak sinyalin alındığını ve kapının açık tutulduğunu belirt.
    else if (currentState == GATE_IDLE_OPEN || currentState == GATE_OPENING)
    {
      logMessage("Command: OPEN received. Keeping gate open.", 1, settings.logLevel); // Seviye 1 log
    }
  }
  // --- WARN Komutu Mantığı ---
  else if (strcmp(command, "WARN") == 0)
  {
    // WARN komutunun ana görevi "ben buradayım" sinyali vermektir.
    // Zamanlayıcı zaten OnDataRecv'de sıfırlandığı için burada ek bir motor işlemi yapmayız.
    // Sadece kapı açıkken bu sinyalin bir anlamı olur.
    if (currentState == GATE_IDLE_OPEN)
    {
      logMessage("Command: WARN received. Auto-close postponed.", 1, settings.logLevel);
    }
  }
  // Buraya 'CLOSE', 'STOP' gibi başka komutlar için 'else if' blokları eklenebilir.
}

// ESP-NOW veri alındığında çağrılır
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len)
{

  String message = String((char *)incomingData, len);
  Serial.print("Data as String: ");
  Serial.println(message);

  LedPattern p_ack = {50, 1}; // 50ms, 1 kere
  xQueueSend(ledQueue, &p_ack, 0);

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, incomingData, len);
  if (error)
  {
    logMessage("JSON parsing failed.", 1, settings.logLevel);
    return;
  }

  const char *msgType = doc["msgType"];
  if (!msgType)
  {
    logMessage("msgType field missing.", 1, settings.logLevel);
    return;
  }

  String vehicleMac = macToString(mac_addr);
  logMessage("Received '" + String(msgType) + "' from " + vehicleMac, 1, settings.logLevel);

  // === EŞLEŞME MESAJLARI ===

  if (strcmp(msgType, "DISCOVERY_RESPONSE") == 0)
  {
    // Eğer keşif modundaysak ve henüz bir cihaz bulmadıysak
    if (currentAuthMode == AUTH_DISCOVERY_SCAN)
    {
      currentAuthMode = AUTH_IDLE; // Taramayı hemen durdur

      foundVehicleMac = macToString(mac_addr);
      foundVehicleName = doc["deviceName"].as<String>();

      if (isMacRegistered(foundVehicleMac))
      {
        logMessage("Found vehicle is already registered: " + foundVehicleMac + " (" + foundVehicleName + ")", 0, settings.logLevel);
        // return; // Zaten kayıtlıysa, başka bir şey yapma
      }

      logMessage("First vehicle found: " + foundVehicleMac + " (" + foundVehicleName + ")", 0, settings.logLevel);

      // Panele bulduğumuz cihazın bilgilerini gönder
      JsonDocument foundDoc;
      foundDoc["event"] = "device_found";
      foundDoc["mac"] = foundVehicleMac;
      foundDoc["deviceName"] = foundVehicleName;

      String output;
      serializeJson(foundDoc, output);
      pStatusChar->setValue(output.c_str());
      pStatusChar->notify();
    }
  }

  if (strcmp(msgType, "AUTH_ACK") == 0)
  {
    if (currentAuthMode == AUTH_SCANNING)
    {
      logMessage("AUTH_ACK received. Adding Vehicle as peer...", 0, settings.logLevel);

      // Cevap verilecek Aracı geçici olarak peer listesine ekle
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, mac_addr, 6);
      peerInfo.channel = FIXED_WIFI_CHANNEL;
      peerInfo.encrypt = false;
      memcpy(peerInfo.lmk, PMK, 16); // Peer'e anahtar bilgisini ekle

      if (esp_now_add_peer(&peerInfo) != ESP_OK)
      {
        logMessage("Fatal: Failed to add Vehicle as peer.", 0, settings.logLevel);
        currentAuthMode = AUTH_IDLE;
        return;
      }

      // Yeni, benzersiz ortak anahtar oluştur ve kaydet
      uint8_t newSharedKey[16];
      for (int i = 0; i < 16; i++)
      {
        newSharedKey[i] = esp_random() % 256;
      }

      char newSharedKeyHex[33];
      for (int i = 0; i < 16; i++)
      {
        sprintf(newSharedKeyHex + i * 2, "%02x", newSharedKey[i]);
      }

      const char *incomingVehicleName = doc["deviceName"];
      String vehicleNameStr = incomingVehicleName ? String(incomingVehicleName) : "Unknown";

      RegisteredVehicleInfo newVehicle;
      newVehicle.sharedKey = String(newSharedKeyHex);
      newVehicle.nickname = vehicleNameStr;    // ilk başta nickname'i de araç ismi yap
      newVehicle.vehicleName = vehicleNameStr; // orijinal araç ismi sakla

      registeredDevices[vehicleMac] = newVehicle;

      saveRegisteredDevices();

      // Anahtarı şifreleyip gönderme moduna geç
      String keyToSend = String((char *)newSharedKey, 16);
      pendingEncryptedKey = encryptDecrypt(keyToSend, PMK, sizeof(PMK));
      pendingVehicleMac = vehicleMac;

      currentAuthMode = AUTH_SENDING_KEY;
      keySendStartTime = millis();
      lastKeySendTime = 0;
      logMessage("Starting repeated KEY_DELIVERY to " + vehicleMac, 0, settings.logLevel);
    }
  }
  // 2. Adım: Araçtan anahtarı aldığına dair son teyit geldi
  else if (strcmp(msgType, "KEY_ACK") == 0)
  {
    if (currentAuthMode == AUTH_SENDING_KEY && vehicleMac.equals(pendingVehicleMac))
    {
      logMessage("KEY_ACK received. Pairing is now fully complete with " + vehicleMac, 0, settings.logLevel);

      esp_now_del_peer(mac_addr); // Geçici peer kaydını sil

      currentAuthMode = AUTH_IDLE;
      pendingVehicleMac = "";
      pendingEncryptedKey = "";

      // Pairing zamanlayıcısını sıfırla
      keySendStartTime = 0;

      // BLE paneline pairing tamamlandı mesajı gönder
      JsonDocument doc;
      doc["event"] = "pairing_complete";
      String output;
      serializeJson(doc, output);
      pStatusChar->setValue(output.c_str());
      pStatusChar->notify();

      LedPattern p = {10000, 1};
      xQueueSend(ledQueue, &p, 0); // Başarı sinyali
    }
  }

  // === NORMAL OPERASYON MESAJLARI ===

  // Güvenli komut geldi
  else if (strcmp(msgType, "COMMAND") == 0)
  {
    if (settings.operationMode != MODE_NORMAL)
    {
      logMessage("Ignoring COMMAND: Gate is not in NORMAL mode.", 1, settings.logLevel);
      return;
    }

    const char *command = doc["command"];
    if (!command)
    {
      logMessage("Command field missing in COMMAND.", 0, settings.logLevel);
      return;
    }

    // 1. Güvenlik Gerekli Değilse (Herkese Açık Mod)
    if (!settings.authorizationRequired)
    {
      logMessage("Authorization not required. Processing command directly.", 1, settings.logLevel);
      lastAuthenticatedCommandTime = millis(); // Sinyal var olarak kabul et
      handleCommand(command);
    }
    // 2. Güvenlik Gerekliyse
    else
    {
      if (!isMacRegistered(vehicleMac))
      {
        logMessage("CMD from unregistered device: " + vehicleMac, 0, settings.logLevel);
        return;
      }

      const char *nonce = doc["nonce"];
      const char *receivedHmac = doc["hmac"];
      if (!nonce || !receivedHmac)
      {
        logMessage("Secure command missing nonce or hmac.", 0, settings.logLevel);
        return;
      }

      String sharedKeyHex = registeredDevices[vehicleMac].sharedKey;
      uint8_t sharedKey[16];
      for (int i = 0; i < 16; i++)
      {
        sscanf(sharedKeyHex.c_str() + i * 2, "%2hhx", &sharedKey[i]);
      }

      String dataToSign = vehicleMac + String(command) + String(nonce);
      String expectedHmac = calculateHmac(sharedKey, 16, dataToSign.c_str());

      if (expectedHmac.equals(String(receivedHmac)))
      {
        logMessage("HMAC SUCCESS! Secure command verified: " + String(command), 0, settings.logLevel);
        lastAuthenticatedCommandTime = millis(); // Otomatik kapanma zamanlayıcısını sıfırla

        LedPattern p_cmd = {100, 2}; // 100ms, 2 kere
        xQueueSend(ledQueue, &p_cmd, 0);

        handleCommand(command); // Komutu işle
      }
      else
      {
        logMessage("HMAC FAILED! Command rejected from " + vehicleMac, 0, settings.logLevel);
      }
    }
  }
}

// ESP-NOW veri gönderildiğinde çağrılır
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  if (status != ESP_NOW_SEND_SUCCESS)
  {
    logMessage("Last Packet Send Status: " + String(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail"), 0, settings.logLevel);
  }
}

// gate.cpp -> setup()'dan önce bir yere ekleyin

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    isAuthenticated = false;
    logMessage("BLE Client Connected.", 0, settings.logLevel);

    // Bağlantı kurulur kurulmaz, OTA yapılandırmasını web paneline gönder.
    // vTaskDelay, BLE yığınının bağlantıyı tamamen kurmasına zaman tanır.
    vTaskDelay(pdMS_TO_TICKS(100)); // 100ms bekle

    if (pStatusChar && pServer->getConnectedCount() > 0)
    {

      // Cihaz ile istemci arasında anlaşılan MTU değerini al

      JsonDocument doc;
      doc["event"] = "ota_config";
      doc["windowSize"] = OTA_PACKET_WINDOW_SIZE; // C++'taki sabiti gönder
      doc["chunkSize"] = OTA_CHUNK_SIZE;
      String output;
      serializeJson(doc, output);
      pStatusChar->setValue(output.c_str());
      pStatusChar->notify();

      char logBuffer[100];
      snprintf(logBuffer, sizeof(logBuffer), "Sent OTA config to client. (ChunkSize: %d, WindowSize: %d)", OTA_CHUNK_SIZE, OTA_PACKET_WINDOW_SIZE);
      logMessage(logBuffer, 0, settings.logLevel);
    }
  }
  void onDisconnect(BLEServer *pServer)
  {
    logMessage("BLE Client Disconnected.", 0, settings.logLevel);

    // Eğer bağlantı, bir OTA güncellemesinin ortasında koptuysa...
    if (isUpdateInProgress)
    {
      logMessage("OTA session interrupted by disconnect. Progress is SAVED for resume.", 0, settings.logLevel);

      isUpdateInProgress = false;
    }

    // Cihazın yeni bağlantılar için tekrar reklam yapmasını sağla.
    BLEDevice::startAdvertising();
  }
};

class SettingsCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onRead(BLECharacteristic *pCharacteristic)
  {
    // 1. Yetki Kontrolü
    if (!isAuthenticated && !settings.pinCode.isEmpty())
    {
      pCharacteristic->setValue("{\"error\":\"Authentication required\"}");
      return;
    }

    // 2. Tüm Ayarları İçeren JSON'u Oluştur
    JsonDocument jsonDoc;
    jsonDoc["deviceName"] = settings.deviceName;
    jsonDoc["closeTimeout"] = settings.closeTimeout;
    jsonDoc["preCloseWarning"] = settings.preCloseWarning;
    jsonDoc["openLimit"] = settings.openLimit;
    jsonDoc["opMode"] = settings.operationMode;
    jsonDoc["authReq"] = settings.authorizationRequired;
    jsonDoc["pinExists"] = !settings.pinCode.isEmpty();
    jsonDoc["updateUrl"] = settings.updateUrl;
    jsonDoc["firmwareVersion"] = getFirmwareVersion();

    // 3. JSON'u Panele Gönder
    String jsonString;
    serializeJson(jsonDoc, jsonString);
    pCharacteristic->setValue(jsonString.c_str());
    logMessage("Sent full settings over BLE.", 1, settings.logLevel);
  }

  void onWrite(BLECharacteristic *pCharacteristic)
  {
    // 1. Yetki Kontrolü
    // Eğer PIN ayarlıysa ve kullanıcı kimliğini doğrulamadıysa, işlemi hemen reddet.
    if (!isAuthenticated && !settings.pinCode.isEmpty())
    {
      logMessage("Settings write rejected: Not authenticated", 0, settings.logLevel);
      pStatusChar->setValue("AUTH_REQUIRED");
      pStatusChar->notify();
      return;
    }

    // 2. Gelen Veriyi Al ve JSON Olarak Ayrıştır
    std::string value = pCharacteristic->getValue();
    if (value.empty())
      return;

    JsonDocument jsonDoc;
    DeserializationError error = deserializeJson(jsonDoc, value);
    if (error)
    {
      logMessage("JSON parse error on write: " + String(error.c_str()), 0, settings.logLevel);
      return;
    }

    logMessage("Received new settings via BLE. Processing...", 1, settings.logLevel);
    bool anyChangeDetected = false;

    // 3. Ayarları Tek Tek Kontrol Et ve Güncelle

    // Cihaz Adı
    if (jsonDoc["deviceName"].is<const char *>())
    {
      String newDeviceName = jsonDoc["deviceName"].as<String>();
      // Karşılaştırma yaparken char dizisini String'e çevir
      if (String(settings.deviceName) != newDeviceName)
      {
        strlcpy(settings.deviceName, newDeviceName.c_str(), sizeof(settings.deviceName));
        logMessage("Setting updated: deviceName -> " + newDeviceName, 0, settings.logLevel);
        anyChangeDetected = true;
      }
    }

    // Otomatik Kapanma Süresi
    if (jsonDoc["closeTimeout"].is<int>() && settings.closeTimeout != jsonDoc["closeTimeout"].as<int>())
    {
      settings.closeTimeout = jsonDoc["closeTimeout"];
      logMessage("Setting updated: closeTimeout -> " + String(settings.closeTimeout), 0, settings.logLevel);
      anyChangeDetected = true;
    }

    // Kapanma Öncesi Uyarı
    if (jsonDoc["preCloseWarning"].is<int>() && settings.preCloseWarning != jsonDoc["preCloseWarning"].as<int>())
    {
      settings.preCloseWarning = jsonDoc["preCloseWarning"];
      logMessage("Setting updated: preCloseWarning -> " + String(settings.preCloseWarning), 0, settings.logLevel);
      anyChangeDetected = true;
    }

    // Çalışma Modu
    if (jsonDoc["opMode"].is<int>() && settings.operationMode != jsonDoc["opMode"].as<int>())
    {
      settings.operationMode = (GateOperationMode)jsonDoc["opMode"].as<int>();
      logMessage("Setting updated: operationMode -> " + String(settings.operationMode), 0, settings.logLevel);
      anyChangeDetected = true;
    }

    // Güvenlik Modu
    if (jsonDoc["authReq"].is<bool>() && settings.authorizationRequired != jsonDoc["authReq"].as<bool>())
    {
      settings.authorizationRequired = jsonDoc["authReq"];
      logMessage("Setting updated: authorizationRequired -> " + String(settings.authorizationRequired), 0, settings.logLevel);
      anyChangeDetected = true;
    }

    // PIN Kodu Değişikliği
    // Web paneli, sadece PIN değiştirilmek istendiğinde bu alanı gönderir.
    // Check if the "pinCode" key exists and is not null in the incoming JSON
    if (!jsonDoc["pinCode"].isNull())
    { // <-- This is the corrected line
      String newPin = jsonDoc["pinCode"].as<String>();
      if (settings.pinCode != newPin)
      {
        settings.pinCode = newPin;
        isAuthenticated = false; // PIN changed, require re-authentication for security
        logMessage("Setting updated: PIN has been changed. Re-authentication required.", 0, settings.logLevel);
        anyChangeDetected = true;
      }
    }

    // 4. Eğer Herhangi Bir Değişiklik Varsa Kaydetmeyi Tetikle
    if (anyChangeDetected)
    {
      isSettingsChanged = true;
      lastSettingsChangeTime = millis();
      logMessage("Settings changed. Will be saved to NVRAM shortly.", 0, settings.logLevel);
      // İsteğe bağlı: Web paneline ayarların güncellendiğine dair bir bildirim gönder
      pStatusChar->setValue("SETTINGS_UPDATED");
      pStatusChar->notify();
    }
    else
    {
      logMessage("Received settings are same as current. No changes made.", 1, settings.logLevel);
    }
  }
};

class PinAuthCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    // Kilitli olup olmadığımızı kontrol etmeden önce en güncel durumu oku
    loadSecurityState();

    // 1. ADIM: Cihaz kilitli mi?
    if (millis() < securityState.pinLockoutEndTime)
    {
      long remainingSeconds = (securityState.pinLockoutEndTime - millis()) / 1000;
      logMessage("Device is locked. Try again in " + String(remainingSeconds) + " seconds.", 0, settings.logLevel);
      pStatusChar->setValue("AUTH_LOCKED");
      pStatusChar->notify();
      return;
    }

    std::string value = pCharacteristic->getValue();
    if (value.empty())
      return;

    // 2. ADIM: PIN doğru mu?
    if (!settings.pinCode.isEmpty() && value == settings.pinCode.c_str())
    {
      // PIN DOĞRU
      logMessage("PIN auth successful!", 0, settings.logLevel);
      isAuthenticated = true;

      // Hata sayacını ve kilidi sıfırla, sonra durumu KALICI OLARAK KAYDET
      securityState.pinFailedAttempts = 0;
      securityState.pinLockoutEndTime = 0;
      saveSecurityState();

      pStatusChar->setValue("AUTH_SUCCESS");
      pStatusChar->notify();
    }
    else
    {
      // PIN YANLIŞ
      isAuthenticated = false;
      securityState.pinFailedAttempts++;
      logMessage("PIN auth FAILED! Attempt " + String(securityState.pinFailedAttempts), 0, settings.logLevel);

      // 3. ADIM: Kademeli kilitlemeyi uygula
      if (securityState.pinFailedAttempts >= 10)
      {
        securityState.pinLockoutEndTime = millis() + 900000; // 15 dakika
      }
      else if (securityState.pinFailedAttempts >= 5)
      {
        securityState.pinLockoutEndTime = millis() + 60000; // 1 dakika
      }
      else if (securityState.pinFailedAttempts >= 3)
      {
        securityState.pinLockoutEndTime = millis() + 10000; // 10 saniye
      }

      // Yeni hata durumunu KALICI OLARAK KAYDET
      saveSecurityState();

      pStatusChar->setValue("AUTH_FAILED");
      pStatusChar->notify();
    }
  }

  void onRead(BLECharacteristic *pCharacteristic)
  {
    // PIN gerekip gerekmediği bilgisini JSON olarak gönder
    JsonDocument jsonDoc;
    jsonDoc["pinRequired"] = !settings.pinCode.isEmpty();
    String jsonString;
    serializeJson(jsonDoc, jsonString);
    pCharacteristic->setValue(jsonString.c_str());
  }
};

// ========================= Kurulum ve Döngü =========================
void setupSimple()
{
  Serial.begin(115200);
  Serial.println("Setup basladi...");

  Serial.println("Setup tamamlandi.");
}

void setup()
{
  Serial.begin(115200);
  Serial.println();

  logMessage("Gate Control System Starting...", 0, settings.logLevel);

  // Pinleri ayarla
  pinMode(INTERNAL_LED_PIN, OUTPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(OPEN_PIN, OUTPUT);
  pinMode(CLOSE_PIN, OUTPUT);
  pinMode(AUTH_BUTTON_PIN, INPUT_PULLUP); // Buton için INPUT_PULLUP

  // Başlangıçta tüm çıkışları kapat
  digitalWrite(INTERNAL_LED_PIN, LED_OFF);
  digitalWrite(RED_PIN, LIGHT_OFF);
  digitalWrite(GREEN_PIN, LIGHT_OFF);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
  digitalWrite(OPEN_PIN, MOTOR_INACTIVE);
  digitalWrite(CLOSE_PIN, MOTOR_INACTIVE);

  digitalWrite(INTERNAL_LED_PIN, LED_ON);
  delay(2000);
  digitalWrite(INTERNAL_LED_PIN, LED_OFF);
  delay(1000);

  // Cihaz her açıldığında, önce yarım kalmış bir OTA olup olmadığını kontrol et.
  preferences.begin("ota_state", true); // Kalıcı hafızayı okuma modunda aç
  bool hasIncompleteUpdate = preferences.isKey("ota_prog") && preferences.getULong("ota_prog", 0) > 0;
  preferences.end();

  if (hasIncompleteUpdate)
  {
    logMessage("Incomplete OTA update detected from previous session. Cleaning up...", 0, settings.logLevel);

    // 1. Update kütüphanesinin durumunu temizle ve OTA bölümünü geçersiz kıl.
    // Bu, bootloader'ın kafasının karışmasını önler.
    Update.abort();

    // 2. Kalıcı hafızadaki ilerleme kaydını sıfırla.
    preferences.begin("ota_state", false); // Yazma modunda aç
    preferences.putULong("ota_prog", 0);
    preferences.end();
    delay(100); // Hafızanın güncellenmesi için kısa bir bekleme

    logMessage("OTA cleanup complete. System is ready for a fresh update.", 0, settings.logLevel);
  }

  // Ayarları ve kayıtlı mac id'leri yükle
  loadSettings();
  printSettingsToSerial();

  // Override
  // settings.authorizationRequired = false;
  settings.logLevel = 1;
  // settings.operationMode = MODE_DISABLED;

  loadRegisteredDevices();
  printRegisteredDevices();

  loadSecurityState();

  // LED kuyruğunu oluştur
  ledQueue = xQueueCreate(10, sizeof(LedPattern));
  if (ledQueue == NULL)
  {
    logMessage("Failed to create LED queue", 0, settings.logLevel);
  }

  // FreeRTOS görevlerini oluştur
  xTaskCreatePinnedToCore(
      ledTask,
      "LED Task",
      2048, // Stack size
      NULL,
      1, // Priority
      &ledTaskHandle,
      0 // Core
  );

  logMessage("LED Task added.", 0, settings.logLevel);

  xTaskCreatePinnedToCore(
      stateMachineTask,
      "State Machine Task",
      2048, // Stack size
      NULL,
      1, // Priority
      &stateTaskHandle,
      1 // Core
  );

  logMessage("State Machine Task added.", 0, settings.logLevel);

  // WiFi'yi istasyon moduna ayarla
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  logMessage("WiFi set to Station Mode.", 0, settings.logLevel);

  // ESP-NOW'ı başlat
  if (esp_now_init() != ESP_OK)
  {
    logMessage("Error initializing ESP-NOW", 0, settings.logLevel);
    return;
  }
  logMessage("ESP-NOW initialized.", 0, settings.logLevel);

  // ESP-NOW callback fonksiyonlarını kaydet
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);
  logMessage("ESP-NOW receiver ready", 0, settings.logLevel);

  // ESP-NOW peer ekle (broadcast için)
  esp_now_peer_info_t peerInfo;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = FIXED_WIFI_CHANNEL;
  peerInfo.encrypt = false;
  memcpy(peerInfo.lmk, PMK, 16); // PSK'yı ayarla
  peerInfo.ifidx = WIFI_IF_STA;  // Station arayüzünü kullan

  esp_wifi_set_channel(FIXED_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_add_peer(&peerInfo) != ESP_OK)
  {
    logMessage("Failed to add broadcast peer", 0, settings.logLevel);
    return;
  }
  logMessage("Broadcast peer added.", 0, settings.logLevel);

  // --- BLE BAŞLATMA BLOĞU ---
  logMessage("Initializing BLE...", 0, settings.logLevel);
  BLEDevice::init(settings.deviceName);
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(GATE_SERVICE_UUID);

  // Ayarlar karakteristiği (bu zaten vardı)
  pSettingsChar = pService->createCharacteristic(
      CHAR_SETTINGS_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE);
  pSettingsChar->setCallbacks(new SettingsCharacteristicCallbacks());

  // --- EKSİK OLAN VE EKLENMESİ GEREKEN BLOK ---
  // PIN Doğrulama karakteristiğini oluştur ve servise ekle
  pPinAuthChar = pService->createCharacteristic(
      CHAR_PIN_AUTH_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE);
  pPinAuthChar->setCallbacks(new PinAuthCharacteristicCallbacks());

  BLECharacteristic *pDeviceMgmtChar = pService->createCharacteristic(
      CHAR_DEVICE_MGMT_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pDeviceMgmtChar->setCallbacks(new DeviceManagementCallbacks());

  // ---------------------------------------------

  // Durum karakteristiği (bu zaten vardı)
  pStatusChar = pService->createCharacteristic(
      CHAR_STATUS_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  pStatusChar->addDescriptor(new BLE2902());

  // --- YENİ OTA KARAKTERİSTİKLERİ ---
  BLECharacteristic *pOtaControlChar = pService->createCharacteristic(
      CHAR_OTA_CONTROL_UUID,
      BLECharacteristic::PROPERTY_WRITE);
  pOtaControlChar->setCallbacks(new OtaControlCallbacks());

  BLECharacteristic *pOtaDataChar = pService->createCharacteristic(
      CHAR_OTA_DATA_UUID,
      BLECharacteristic::PROPERTY_WRITE_NR // WRITE NO RESPONSE for speed
  );
  pOtaDataChar->setCallbacks(new OtaDataCallbacks());

  // --- BLE Servisini Başlatma ---
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(GATE_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); // iOS bağlantı sorunları için yardımcı olur
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  logMessage("BLE Service started. Advertising...", 0, settings.logLevel);

  updateLights(); // Başlangıç ışık durumunu ayarla

  Serial.println(getTimestamp() + " Setup tamamlandı...");
}

void sendBeaconBroadcast()
{
  JsonDocument doc;
  doc["msgType"] = "BEACON";

  // Sadece yetkilendirme gerekiyorsa BEACON'a nonce ekle
  if (settings.authorizationRequired)
  {
    doc["nonce"] = String(esp_random());
  }

  String output;
  serializeJson(doc, output);
  esp_now_send(broadcastAddress, (const uint8_t *)output.c_str(), output.length());
}

void sendDiscoveryProbe()
{
  JsonDocument doc;
  doc["msgType"] = "DISCOVERY_PROBE";

  String output;
  serializeJson(doc, output);

  // Mesajı tüm cihazların duyabilmesi için broadcast adresiyle gönder
  esp_err_t result = esp_now_send(broadcastAddress, (const uint8_t *)output.c_str(), output.length());

  if (result != ESP_OK)
  {
    logMessage("Error sending DISCOVERY_PROBE broadcast. Code: " + String(result), 0, settings.logLevel);
  }
  else
  {
    logMessage("Sent DISCOVERY_PROBE.", 1, settings.logLevel); // Sadece detaylı loglamada göster
  }
}

void loop()
{
  unsigned long currentTime = millis();

  // --- Başlangıçta Otomatik Tarama Tetikleyicisi ---
  // Cihaz açıldıktan 3 saniye sonra, eğer ayarlanmışsa, otomatik olarak eşleşme moduna girer.
  if (FORCE_AUTO_SCAN_ON_STARTUP && !autoScanTriggered && currentTime >= 3000)
  {
    if (currentAuthMode == AUTH_IDLE)
    {
      currentAuthMode = AUTH_SCANNING;
      scanModeEndTime = currentTime + SCAN_DURATION_MS;
      // g_pairingComplete = false;
      lastScanBroadcastTime = 0;
      logMessage("FORCED AUTO-SCAN: Entering SCANNING mode...", 0, settings.logLevel);
    }
    autoScanTriggered = true; // Bu bloğun tekrar çalışmasını engelle
  }

  // --- Ayarların Gerekirse Kaydedilmesi ---
  if (isSettingsChanged && (currentTime - lastSettingsChangeTime >= SETTINGS_SAVE_DELAY))
  {
    saveSettings();
  }

  // --- Yeni Buton Yönetim Mantığı ---
  bool isButtonPressed = (digitalRead(AUTH_BUTTON_PIN) == LOW);

  // Buton basılı tutuluyorsa...
  if (isButtonPressed)
  {
    if (!authButtonPressed)
    {
      // Butona YENİ basıldı (ilk an)
      authButtonPressed = true;
      longPressActionTriggered = false; // Her yeni basışta eylem bayrağını sıfırla
      authButtonPressStartTime = currentTime;
      logMessage("Auth button pressed.", 0, settings.logLevel);
    }

    // Buton 3 saniyedir basılı tutuluyorsa VE eylem daha önce tetiklenmediyse...
    if (!longPressActionTriggered && (currentTime - authButtonPressStartTime >= LONG_PRESS_THRESHOLD_MS))
    {
      if (currentAuthMode == AUTH_IDLE)
      {
        currentAuthMode = AUTH_SCANNING;
        scanModeEndTime = currentTime + SCAN_DURATION_MS;
        // g_pairingComplete = false;
        lastScanBroadcastTime = 0;
        logMessage("3-second hold detected: Entering SCANNING mode...", 0, settings.logLevel);
      }
      longPressActionTriggered = true; // Eylemin tetiklendiğini işaretle
    }
  }
  // Buton basılı değilse (bırakılmışsa)...
  else
  {
    if (authButtonPressed)
    {
      // Buton AZ ÖNCE bırakıldı
      unsigned long pressDuration = currentTime - authButtonPressStartTime;
      logMessage("Auth button released. Duration: " + String(pressDuration) + "ms", 0, settings.logLevel);

      // Eğer bırakıldığında uzun basış eylemi tetiklenmediyse, bu bir kısa basıştır.
      // Gate için kısa basışa bir eylem atamadık, o yüzden bir şey yapmıyoruz.
      if (!longPressActionTriggered)
      {
        logMessage("SHORT PRESS: No action defined for Gate.", 0, settings.logLevel);
      }

      // Buton durumunu bir sonraki basış için sıfırla
      authButtonPressed = false;
    }
  }

  // --- Ana Durum Makinesi (State Machine) ---
  switch (currentAuthMode)
  {

  case AUTH_SCANNING:
    // Tarama modundaysak...
    if (currentTime >= scanModeEndTime)
    {
      logMessage("SCANNING mode timed out.", 0, settings.logLevel);
      currentAuthMode = AUTH_IDLE;
    }
    else if (currentTime - lastScanBroadcastTime >= BROADCAST_INTERVAL_MS)
    {
      // Belirtilen aralıklarla SCAN_REQUEST yayını yap.
      sendScanRequestBroadcast();
      lastScanBroadcastTime = currentTime;
    }
    break;

  case AUTH_SENDING_KEY:
    // Anahtar teslim modundaysak...
    if (currentTime - keySendStartTime > KEY_SEND_DURATION_MS)
    {
      logMessage("KEY_DELIVERY timeout for " + pendingVehicleMac + ". Pairing failed.", 0, settings.logLevel);
      uint8_t targetMac[6];
      sscanf(pendingVehicleMac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &targetMac[0], &targetMac[1], &targetMac[2], &targetMac[3], &targetMac[4], &targetMac[5]);
      esp_now_del_peer(targetMac);
      registeredDevices.erase(pendingVehicleMac);
      saveRegisteredDevices();
      currentAuthMode = AUTH_IDLE;
    }
    else if (currentTime - lastKeySendTime > KEY_SEND_INTERVAL_MS)
    {
      // Belirtilen aralıklarla KEY_DELIVERY mesajı gönder.
      JsonDocument keyDoc;
      keyDoc["msgType"] = "KEY_DELIVERY";
      keyDoc["key"] = pendingEncryptedKey;
      String output;
      serializeJson(keyDoc, output);

      uint8_t targetMac[6];
      sscanf(pendingVehicleMac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &targetMac[0], &targetMac[1], &targetMac[2], &targetMac[3], &targetMac[4], &targetMac[5]);
      esp_now_send(targetMac, (const uint8_t *)output.c_str(), output.length());
      lastKeySendTime = currentTime;
    }
    break;

  case AUTH_DISCOVERY_SCAN:
    // Keşif modundaysak ve 10 saniye geçtiyse zaman aşımına uğrat
    if (millis() - discoveryScanStartTime > 10000)
    {
      logMessage("Discovery scan timed out. No devices found.", 0, settings.logLevel);
      currentAuthMode = AUTH_IDLE;
      // Opsiyonel: Panele "cihaz bulunamadı" bildirimi gönderilebilir.
      pStatusChar->setValue("{\"event\":\"scan_failed\"}");
      pStatusChar->notify();
    }
    // Her saniye keşif yayınını tekrarla
    else if (millis() - lastScanBroadcastTime >= 1000)
    {
      sendDiscoveryProbe();
      lastScanBroadcastTime = millis();
    }
    break;

  case AUTH_IDLE:
    // Boşta modundaysak...
    // VE ÖNEMLİSİ: BİR OTA GÜNCELLEMESİ YAPILMIYORKEN...
    if (!registeredDevices.empty() && settings.operationMode != MODE_DISABLED)
    {
      if (currentTime - lastBeaconTime > BEACON_INTERVAL_MS)
      {
        sendBeaconBroadcast();
        lastBeaconTime = currentTime;
      }
    }
    // Eşleşme bekleniyorsa hiçbir şey yapma.
    break;
  }

  vTaskDelay(pdMS_TO_TICKS(50)); // CPU'ya nefes aldır ve diğer görevlere zaman tanı.
}