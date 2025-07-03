#include "common.cpp"



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
  #define OPEN_PIN 4  // Motoru açmak için tetik pini
  #define CLOSE_PIN 5 // Motoru kapatmak için tetik pini
  #define AUTH_BUTTON_PIN 21 // Yetkilendirme/Tarama butonu
  #define LED_ON LOW
  #define LED_OFF HIGH
#elif CONFIG_IDF_TARGET_ESP32
  // ESP32-WROOM-32 pinleri
  #define INTERNAL_LED_PIN 2
  #define RED_PIN 14
  #define GREEN_PIN 12
  #define BUZZER_PIN 27
  #define OPEN_PIN 4  // Motoru açmak için tetik pini
  #define CLOSE_PIN 5 // Motoru kapatmak için tetik pini
  #define AUTH_BUTTON_PIN 0 // Yetkilendirme/Tarama butonu
  #define LED_ON HIGH
  #define LED_OFF LOW
#endif

// Sabitler
#define FIXED_WIFI_CHANNEL 6      // WiFi kanalı (1, 6 veya 11 önerilir)
#define MOTOR_COOLDOWN_MS 1000    // 1 saniye motor koruma süresi
#define LIGHT_OFF LOW
#define LIGHT_ON HIGH
#define BUZZER_ON HIGH
#define BUZZER_OFF LOW
#define MOTOR_ACTIVE HIGH
#define MOTOR_INACTIVE LOW
#define DEFAULT_DEVICE_NAME_PREFIX "GATE_" // Varsayılan cihaz adı ön eki
#define DEFAULT_LOG_LEVEL_INFO 0 // 0 = INFO, 1 = VERBOSE

const bool FORCE_AUTO_SCAN_ON_STARTUP = false; // true: otomatik başlar, false: sadece butonla başlar
static bool autoScanTriggered = false; // Bu özelliğin sadece bir kez çalışmasını sağlar


unsigned long lastAuthenticatedCommandTime = 0; // Son geçerli komutun zamanı
unsigned long warningStartTime = 0; // <<< BU SATIRI EKLEYİN


// Preferences (NVRAM) nesnesi
Preferences preferences;

// Cihaz Ayarları Yapısı
struct DeviceSettings {
  char deviceName[32];            // Cihazın adı (BLE reklamında ve seri monitörde kullanılır)
  int closeTimeout;               // Otomatik kapanma süresi (saniye)
  int preCloseWarning;
  int openLimit;                  // Açık kalma süresi sınırı (saniye) - güvenlik için
  int logLevel;                   // 0 = INFO, 1 = VERBOSE
  bool authorizationRequired;     // Yetkilendirme gerekliliği
};

// Durum Makinesi için Enum
enum GateState {
  GATE_CLOSED,
  GATE_CLOSING,
  GATE_OPENING,
  GATE_IDLE_OPEN,
  GATE_WARNING // Kapanmadan önce uyarı durumu
};

// LED Görevi İçin Yapı (Queue Item)
struct LedPattern {
  int duration; // Her flash'ın süresi (ms)
  int count;    // Flash sayısı
};

// Global Değişkenler
DeviceSettings settings;
GateState currentState = GATE_CLOSED;
unsigned long stateChangeTime = 0; // Durum değişikliğinin zaman damgası
unsigned long lastSignalTime = 0;  // Son sinyal alış zaman damgası (otomatik kapanmayı öteleme için)
bool isAutoCloseScheduled = false; // Otomatik kapanmanın planlanıp planlanmadığı
unsigned long autoCloseScheduledTime = 0; // Otomatik kapanmanın zaman damgası
bool preCloseWarnLogged = false; // Kapanmadan önce uyarı mesajının bir kez loglandığını gösterir
bool autoClosePostponedLoggedOnce = false; // Otomatik kapanmanın ertelendiğinin bir kez loglandığını gösterir
bool isSettingsChanged = false; // Ayarların değişip değişmediğini kontrol eden bayrak
unsigned long lastSettingsChangeTime = 0; // Ayarların en son değiştiği zaman
const unsigned long SETTINGS_SAVE_DELAY = 5000; // Ayarları kaydetmek için bekleme süresi (ms)
bool isDeviceNameChanged = false; // Cihaz adının değişip değişmediğini kontrol eden bayrak
bool shouldBeep = false; // Motor hareket halindeyken bip sesini kontrol eder


// Beacon yayını için zamanlama değişkenleri
unsigned long lastBeaconTime = 0;
const unsigned long BEACON_INTERVAL_MS = 1000; // 1 saniyede bir yayın yapacak şekilde ayarlandı



// FreeRTOS Görev ve Kuyruk Tanımları
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t stateTaskHandle = NULL;
QueueHandle_t ledQueue;

// Yetkilendirme Modu ve Durum Yönetimi
unsigned long authButtonPressStartTime = 0;
bool authButtonPressed = false; // Bu, dahili olarak buton durumunu tutar.
bool longPressActionTriggered = false; // <<< YENİ SATIR: Uzun basış eyleminin tetiklendiğini işaretler
const unsigned long LONG_PRESS_THRESHOLD_MS = 3000; // 3 saniye uzun basış eşiği (tarama modu için)
unsigned long scanModeEndTime = 0;
const unsigned long SCAN_DURATION_MS = 300000;       // 30 saniye tarama süresi
const unsigned long BROADCAST_INTERVAL_MS = 1000;   // Her 1 saniyede bir broadcast
unsigned long lastScanBroadcastTime = 0;

// Yetkilendirilmiş MAC adresleri listesi (Preferences'tan yüklenmeli)
std::map<String, String> registeredDevices; 

// Eşleşme protokolü için yeni durumlar ve değişkenler
enum AuthMode { AUTH_IDLE, AUTH_SCANNING, AUTH_SENDING_KEY };
volatile AuthMode currentAuthMode = AUTH_IDLE;
String pendingVehicleMac = "";      // Teyit beklenen aracın MAC adresi
String pendingEncryptedKey = "";  // Gönderilecek şifreli anahtar
unsigned long keySendStartTime = 0; // Tekrarlı gönderimin başlangıç zamanı
unsigned long lastKeySendTime = 0;  // Son gönderimin zamanı
const unsigned long KEY_SEND_DURATION_MS = 5000;  // 5 saniye boyunca gönder
const unsigned long KEY_SEND_INTERVAL_MS = 500;   // 500ms aralıklarla gönder

// Forward Declarations (ileride tanımlanacak fonksiyonların önden bildirimleri)
void updateLights();
void printSettingsToSerial();
void triggerMotor(bool open);
void stopMotor(bool isManualOverride);
void addMacToRegistereds(const uint8_t *mac);
bool isMacRegistered(const uint8_t *mac);

// ========================= Yardımcı Fonksiyonlar =========================


void printSettingsToSerial() {
  logMessage("--- Device Settings ---", 0, settings.logLevel);
  logMessage("Device Name: " + String(settings.deviceName), 0, settings.logLevel);
  logMessage("Auto-close Timeout: " + String(settings.closeTimeout) + "s", 0, settings.logLevel);
  logMessage("Open Limit: " + String(settings.openLimit) + "s", 0, settings.logLevel);
  logMessage("Log Level: " + String(settings.logLevel == 0 ? "INFO" : "VERBOSE"), 0, settings.logLevel), settings.logLevel;
  logMessage("Authorization Required: " + String(settings.authorizationRequired ? "Yes" : "No"), 0, settings.logLevel);
  logMessage("-----------------------", 0, settings.logLevel);
}

// ========================= NVRAM Yönetimi =========================

void loadSettings() {
  preferences.begin("gate-settings", true);

  preferences.getString("deviceName", settings.deviceName, sizeof(settings.deviceName));
  if (strlen(settings.deviceName) == 0) {
    String defaultName = DEFAULT_DEVICE_NAME_PREFIX + String(ESP.getEfuseMac(), HEX).substring(6);
    defaultName.toUpperCase();
    strncpy(settings.deviceName, defaultName.c_str(), sizeof(settings.deviceName));
  }

  settings.closeTimeout = preferences.getInt("closeTimeout", 30);
    settings.preCloseWarning = preferences.getInt("preCloseWarn", 5);

  settings.openLimit = preferences.getInt("openLimit", 300);
  settings.logLevel = preferences.getInt("logLevel", DEFAULT_LOG_LEVEL_INFO);
  settings.authorizationRequired = preferences.getBool("authReq", true);

  preferences.end();
  logMessage("Settings loaded from NVRAM.", 0, settings.logLevel);
}

void saveSettings() {
  preferences.begin("gate-settings", false);

  preferences.putString("deviceName", settings.deviceName);
  preferences.putInt("closeTimeout", settings.closeTimeout);
    preferences.putInt("preCloseWarn", settings.preCloseWarning);

  preferences.putInt("openLimit", settings.openLimit);
  preferences.putInt("logLevel", settings.logLevel);
  preferences.putBool("authReq", settings.authorizationRequired);

  preferences.end();
  isSettingsChanged = false;
  logMessage("Settings saved to NVRAM.", 0, settings.logLevel);
}


// gate.cpp -> saveRegisteredDevices fonksiyonunu bununla değiştirin
void saveRegisteredDevices() {
  preferences.begin("auth_list", false);
  
  JsonDocument doc;
  // Her zaman bir JSON dizisi oluştur
  JsonArray array = doc.to<JsonArray>(); 
  
  // Map'teki her bir [mac, key] çiftini bu diziye yeni bir nesne olarak ekle
  for (auto const& pair : registeredDevices) {
    JsonObject device = array.add<JsonObject>(); // <<< ÖNEMLİ DEĞİŞİKLİK
    device["mac"] = pair.first;
    device["key"] = pair.second;
  }
  
  String jsonOutput;
  serializeJson(doc, jsonOutput);
  
  preferences.putString("devices_json", jsonOutput);
  
  preferences.end();
  delay(100);
  logMessage("jsonOutput: " + jsonOutput, 0, settings.logLevel);
  logMessage("Saved " + String(registeredDevices.size()) + " registered devices.", 0, settings.logLevel);
}


void loadRegisteredDevices() {
  registeredDevices.clear();
  preferences.begin("auth_list", true);

  // Kayıtlı JSON metnini oku
  String jsonInput = preferences.getString("devices_json", "");
  logMessage("jsonInput: " + jsonInput, 0, settings.logLevel);
  
  if (jsonInput.length() > 0) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonInput);
    
    if (!error) {
      JsonArray array = doc.as<JsonArray>();
      // JSON dizisindeki her bir objeyi map'e geri yükle
      for (JsonObject device : array) {
        String mac = device["mac"];
        String key = device["key"];
        registeredDevices[mac] = key;
      }
    } else {
        logMessage("Failed to parse registered devices JSON.", 0, settings.logLevel);
    }
  }
  
  preferences.end();
  logMessage("Loaded " + String(registeredDevices.size()) + " registered devices.", 0, settings.logLevel);
}

bool isMacRegistered(const String& mac) {
  return registeredDevices.count(mac) > 0;
}

void printRegisteredDevices() {
  logMessage("--- Registered Devices (MAC -> Key) ---", 0, settings.logLevel);
  if (registeredDevices.empty()) {
    logMessage("  No registered devices found.", 0, settings.logLevel);
  } else {
    for (auto const& pair : registeredDevices) { // <-- Döngüyü bu şekilde değiştirin
      String mac = pair.first;  // Anahtarı (key) .first ile alın
      String key = pair.second; // Değeri (value) .second ile alın
      logMessage("  - " + mac + " -> " + key.substring(0, 8) + "...", 0, settings.logLevel);
    }
  }
  logMessage("---------------------------------------", 0, settings.logLevel);
}

// ========================= Motor ve Kapı Durumu Fonksiyonları =========================

void triggerMotor(bool open) {
  if (open) {
    digitalWrite(OPEN_PIN, MOTOR_ACTIVE);
    digitalWrite(CLOSE_PIN, MOTOR_INACTIVE);
    logMessage("Triggering motor to OPEN.", 0, settings.logLevel);
  } else {
    digitalWrite(CLOSE_PIN, MOTOR_ACTIVE);
    digitalWrite(OPEN_PIN, MOTOR_INACTIVE);
    logMessage("Triggering motor to CLOSE.", 0, settings.logLevel);
  }
  stateChangeTime = millis();
  shouldBeep = true;
}

void stopMotor(bool isManualOverride) {
  digitalWrite(OPEN_PIN, MOTOR_INACTIVE);
  digitalWrite(CLOSE_PIN, MOTOR_INACTIVE);
  shouldBeep = false;
  if (isManualOverride) {
    logMessage("Motor stopped by manual override.", 0, settings.logLevel);
  } else {
    logMessage("Motor stopped by limit.", 0, settings.logLevel);
  }
  vTaskDelay(pdMS_TO_TICKS(MOTOR_COOLDOWN_MS));
}

void updateLights() {
  if (currentState == GATE_IDLE_OPEN || currentState == GATE_OPENING) {
    digitalWrite(GREEN_PIN, LIGHT_ON);
    digitalWrite(RED_PIN, LIGHT_OFF);
  } else if (currentState == GATE_CLOSING) {
    digitalWrite(GREEN_PIN, LIGHT_OFF);
    digitalWrite(RED_PIN, LIGHT_ON);
  } else if (currentState == GATE_CLOSED || currentState == GATE_WARNING) {
    digitalWrite(GREEN_PIN, LIGHT_OFF);
    digitalWrite(RED_PIN, LIGHT_OFF);
  }
}

// ========================= Görevler (Tasks) =========================

void ledTask(void *pvParameters) {
  LedPattern currentPattern;
  while (true) {
    // 1. Öncelik: Tarama modunu kontrol et
    if (currentAuthMode == AUTH_SCANNING) {
      digitalWrite(INTERNAL_LED_PIN, LED_ON);
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(INTERNAL_LED_PIN, LED_OFF);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue; // Tarama modunda kalmak için döngüye devam et
    }

    // 2. Öncelik: Kuyruktan gelen özel LED desenlerini kontrol et
    // portMAX_DELAY yerine kısa bir timeout (örn: 20ms) kullan.
    // Bu sayede görev bloklanmaz ve currentAuthMode değişikliğini yakalayabilir.
    if (xQueueReceive(ledQueue, &currentPattern, pdMS_TO_TICKS(20)) == pdPASS) {
      for (int i = 0; i < currentPattern.count; i++) {
        digitalWrite(INTERNAL_LED_PIN, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(currentPattern.duration));
        digitalWrite(INTERNAL_LED_PIN, LED_OFF);
        if (i < currentPattern.count - 1) {
          vTaskDelay(pdMS_TO_TICKS(currentPattern.duration));
        }
      }
    } else {
      // Tarama modunda değilsek ve kuyrukta bir komut yoksa, LED'in kapalı olduğundan emin ol.
      digitalWrite(INTERNAL_LED_PIN, LED_OFF);
    }
  }
}



void stateMachineTask(void *pvParameters) {
  while (true) {
    unsigned long currentTime = millis();

    // Ana durum makinesi
    switch (currentState) {
      
      case GATE_IDLE_OPEN:
        // --- Kapı açık ve boşta bekleme durumu ---
        shouldBeep = false;
        
        // 1. Sinyal Kaybı Kontrolü: Araçtan gelen sinyal kesildi mi?
        if (lastAuthenticatedCommandTime > 0 && (currentTime - lastAuthenticatedCommandTime > (settings.closeTimeout * 1000UL))) {
          logMessage("Loss of signal detected. Starting pre-close warning.", 0, settings.logLevel);
          currentState = GATE_WARNING;    // Durumu "Uyarı" yap
          warningStartTime = currentTime; // Uyarı başlangıç zamanını kaydet
        }
        // 2. Güvenlik Limiti Kontrolü: Kapı çok uzun süredir mi açık?
        else if (currentTime - stateChangeTime >= (settings.openLimit * 1000UL)) {
            logMessage("Open limit reached. Starting pre-close warning.", 0, settings.logLevel);
            currentState = GATE_WARNING;    // Durumu "Uyarı" yap
            warningStartTime = currentTime; // Uyarı başlangıç zamanını kaydet
        }
        break;

      case GATE_WARNING:
        // --- Kapanma Öncesi Uyarı Durumu ---
        
        // Uyarı süresi boyunca ışığı ve buzzer'ı çalıştır
        digitalWrite(RED_PIN, (currentTime % 1000 < 500) ? LIGHT_ON : LIGHT_OFF);
        digitalWrite(BUZZER_PIN, (currentTime % 1000 < 100) ? BUZZER_ON : BUZZER_OFF);

        // Uyarı süresi doldu mu?
        if (currentTime - warningStartTime > (settings.preCloseWarning * 1000UL)) {
          logMessage("Pre-close warning finished. Closing gate.", 0, settings.logLevel);
          currentState = GATE_CLOSING;      // Artık kapıyı kapat
          triggerMotor(false);              // Motoru tetikle
          updateLights();                   // Sabit kırmızı ışığı yak
          lastAuthenticatedCommandTime = 0; // Zamanlayıcıyı sıfırla
        }
        break;

      case GATE_OPENING:
      case GATE_CLOSING:
        // --- Kapı Hareket Halindeyken ---
        shouldBeep = true; // Motor çalışırken sürekli bip sesi çal
        
        // Güvenlik: Motorun çok uzun süre çalışıp sıkışmasını önle
        if (currentTime - stateChangeTime >= (settings.openLimit * 1000UL)){
            logMessage("Motor run-time limit exceeded. Stopping motor.", 0, settings.logLevel);
            stopMotor(false);
            currentState = GATE_IDLE_OPEN; // Hata durumunda kapıyı açık bırak
            updateLights();
        }
        break;

      case GATE_CLOSED:
        // --- Kapı Kapalı Durumu ---
        shouldBeep = false;
        // Bir şey yapma
        break;
    }

    // Sürekli bip sesi gerektiren durumlar için (örn: motor hareketi)
    if (shouldBeep) {
      digitalWrite(BUZZER_PIN, BUZZER_ON);
    } else if (currentState != GATE_WARNING) { // Uyarı durumunun kendi buzzer mantığı var
      digitalWrite(BUZZER_PIN, BUZZER_OFF);
    }

    // Görevin diğer görevlere zaman tanıması için kısa bir bekleme
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ========================= ESP-NOW Callback Fonksiyonları =========================


// ESP-NOW Broadcast Gönderme Fonksiyonu
void sendScanRequestBroadcast() {
  // Basit bir JSON mesajı oluşturalım
  JsonDocument doc;
  doc["msgType"] = "SCAN_REQUEST";
  
  String output;
  serializeJson(doc, output);

  esp_err_t result = esp_now_send(broadcastAddress, (const uint8_t*)output.c_str(), output.length());
  
  if (result == ESP_OK) {
    //logMessage("SCAN_REQUEST broadcast succesfully sent.", 0, settings.logLevel);
    // Başarılı gönderimde LED'i bir kez yakıp söndür
    LedPattern p = {100, 1};
    xQueueSend(ledQueue, &p, 0);
  } else {
    logMessage("Error sending SCAN_REQUEST broadcast. Code: " + String(result), 0, settings.logLevel);
  }
}

// ESP-NOW veri alındığında çağrılır
// gate.cpp -> Mevcut OnDataRecv fonksiyonunu silip bunu yapıştırın

void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {


  String message = String((char*)incomingData, len);
  Serial.print("Data as String: ");
  Serial.println(message);


  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, incomingData, len);
  if (error) {
    logMessage("JSON parsing failed.", 1, settings.logLevel);
    return;
  }
  
  const char* msgType = doc["msgType"];
  if (!msgType) {
    logMessage("msgType field missing.", 1, settings.logLevel);
    return;
  }
  
  String vehicleMac = macToString(mac_addr);
  logMessage("Received '" + String(msgType) + "' from " + vehicleMac, 1, settings.logLevel);

  // AUTH_ACK: Eşleşme talebi onayı
  if (strcmp(msgType, "AUTH_ACK") == 0) {
    if (currentAuthMode == AUTH_SCANNING) {

      logMessage("AUTH_ACK received. Adding Vehicle as peer...", 0, settings.logLevel);

      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, mac_addr, 6);
      peerInfo.channel = FIXED_WIFI_CHANNEL;
      peerInfo.encrypt = false;

      if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        logMessage("Fatal: Failed to add Vehicle as peer.", 0, settings.logLevel);
        currentAuthMode = AUTH_IDLE; // Hata durumunda başa dön
        return;
      }

      uint8_t newSharedKey[16];
      for (int i = 0; i < 16; i++) { newSharedKey[i] = esp_random() % 256; }
      
      char newSharedKeyHex[33];
      for(int i = 0; i < 16; i++) { sprintf(newSharedKeyHex + i * 2, "%02x", newSharedKey[i]); }
      
      registeredDevices[vehicleMac] = String(newSharedKeyHex);
      saveRegisteredDevices();
      
      String keyToSend = String((char*)newSharedKey, 16);
      pendingEncryptedKey = encryptDecrypt(keyToSend, PMK, sizeof(PMK));
      pendingVehicleMac = vehicleMac;
      
      currentAuthMode = AUTH_SENDING_KEY;
      keySendStartTime = millis();
      lastKeySendTime = 0; // Hemen göndermeye başlamak için
      logMessage("AUTH_ACK received. Starting repeated KEY_DELIVERY to " + vehicleMac, 0, settings.logLevel);
    }
  // KEY_ACK: Anahtarın alındığına dair son teyit
  } else if (strcmp(msgType, "KEY_ACK") == 0) {
    if (currentAuthMode == AUTH_SENDING_KEY && vehicleMac.equals(pendingVehicleMac)) {
        logMessage("KEY_ACK received. Pairing is now fully complete with " + vehicleMac, 0, settings.logLevel);

        esp_now_del_peer(mac_addr);
        logMessage("Temporary peer " + vehicleMac + " deleted.", 1, settings.logLevel);
      
        //g_pairingComplete = true;
        currentAuthMode = AUTH_IDLE;
        pendingVehicleMac = "";
        pendingEncryptedKey = "";
        // Başarı bildirimi
        LedPattern p = {5000, 1}; xQueueSend(ledQueue, &p, 0);
    }
  // SECURE_COMMAND: Güvenli komut
  } else if (strcmp(msgType, "SECURE_COMMAND") == 0) {
    if (!isMacRegistered(vehicleMac)) {
      logMessage("CMD from unregistered device: " + vehicleMac, 0, settings.logLevel);
      return;
    }

    const char* command = doc["command"];
    const char* nonce = doc["nonce"];
    const char* receivedHmac = doc["hmac"];
    if (!command || !nonce || !receivedHmac) return;
    
    String sharedKeyHex = registeredDevices[vehicleMac];
    uint8_t sharedKey[16];
    for(int i=0; i<16; i++){ sscanf(sharedKeyHex.c_str() + i*2, "%2hhx", &sharedKey[i]); }

    String dataToSign = vehicleMac + String(command) + String(nonce);
    String expectedHmac = calculateHmac(sharedKey, 16, dataToSign.c_str());

    if (expectedHmac.equals(String(receivedHmac))) {
        logMessage("HMAC SUCCESS! Secure command: " + String(command), 0, settings.logLevel);
                lastAuthenticatedCommandTime = millis(); // <<< YENİ SATIR: Zamanlayıcıyı sıfırla

        // BURAYA MEVCUT KOMUT İŞLEME MANTIĞINIZI KOYUN (Kapı açma, kapama vs.)
        // Örnek: handleCommand(command);
    } else {
        logMessage("HMAC FAILED! Command rejected from " + vehicleMac, 0, settings.logLevel);
    }
  }
}

// ESP-NOW veri gönderildiğinde çağrılır
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if(status != ESP_NOW_SEND_SUCCESS)  {
    logMessage("Last Packet Send Status: " + String(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail"), 0, settings.logLevel);
  }
}

// ========================= Kurulum ve Döngü =========================

void setup() {
  Serial.begin(115200);
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

  // Ayarları ve kayıtlı mac id'leri yükle
  loadSettings();  
  printSettingsToSerial();

  loadRegisteredDevices();
  printRegisteredDevices(); 




  // WiFi'yi istasyon moduna ayarla
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  logMessage("WiFi set to Station Mode.", 0, settings.logLevel);

  // ESP-NOW'ı başlat
  if (esp_now_init() != ESP_OK) {
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
  peerInfo.ifidx = WIFI_IF_STA; // Station arayüzünü kullan

  esp_wifi_set_channel(FIXED_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);


  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    logMessage("Failed to add broadcast peer", 0, settings.logLevel);
    return;
  }
  logMessage("Broadcast peer added.", 0, settings.logLevel);


  digitalWrite(INTERNAL_LED_PIN, LED_ON);
  delay(1000);
  digitalWrite(INTERNAL_LED_PIN, LED_OFF);
  delay(1000);

  // LED kuyruğunu oluştur
  ledQueue = xQueueCreate(10, sizeof(LedPattern));
  if (ledQueue == NULL) {
    logMessage("Failed to create LED queue", 0, settings.logLevel);
  }


  // FreeRTOS görevlerini oluştur
  xTaskCreatePinnedToCore(
    ledTask,
    "LED Task",
    2048, // Stack size
    NULL,
    1,    // Priority
    &ledTaskHandle,
    0     // Core
  );

  logMessage("LED Task added.", 0, settings.logLevel);

  xTaskCreatePinnedToCore(
    stateMachineTask,
    "State Machine Task",
    2048, // Stack size
    NULL,
    1,    // Priority
    &stateTaskHandle,
    1     // Core
  );

  logMessage("State Machine Task added.", 0, settings.logLevel);

  
  updateLights(); // Başlangıç ışık durumunu ayarla

  Serial.println(getTimestamp() + " Setup tamamlandı...");

}

void sendBeaconBroadcast() {
  JsonDocument doc;
  doc["msgType"] = "BEACON";
  doc["nonce"] = String(esp_random());
  String output;
  serializeJson(doc, output);
  esp_now_send(broadcastAddress, (const uint8_t*)output.c_str(), output.length());
}


void loop() {
  unsigned long currentTime = millis();

  // --- Başlangıçta Otomatik Tarama Tetikleyicisi ---
  // Cihaz açıldıktan 3 saniye sonra, eğer ayarlanmışsa, otomatik olarak eşleşme moduna girer.
  if (FORCE_AUTO_SCAN_ON_STARTUP && !autoScanTriggered && currentTime >= 3000) {
    if (currentAuthMode == AUTH_IDLE) {
      currentAuthMode = AUTH_SCANNING;
      scanModeEndTime = currentTime + SCAN_DURATION_MS;
      //g_pairingComplete = false;
      lastScanBroadcastTime = 0; 
      logMessage("FORCED AUTO-SCAN: Entering SCANNING mode...", 0, settings.logLevel);
    }
    autoScanTriggered = true; // Bu bloğun tekrar çalışmasını engelle
  }

  // --- Ayarların Gerekirse Kaydedilmesi ---
  if (isSettingsChanged && (currentTime - lastSettingsChangeTime >= SETTINGS_SAVE_DELAY)) {
    saveSettings();
  }

  // --- Yeni Buton Yönetim Mantığı ---
  bool isButtonPressed = (digitalRead(AUTH_BUTTON_PIN) == LOW);

  // Buton basılı tutuluyorsa...
  if (isButtonPressed) {
    if (!authButtonPressed) {
      // Butona YENİ basıldı (ilk an)
      authButtonPressed = true;
      longPressActionTriggered = false; // Her yeni basışta eylem bayrağını sıfırla
      authButtonPressStartTime = currentTime;
      logMessage("Auth button pressed.", 0, settings.logLevel);
    }

    // Buton 3 saniyedir basılı tutuluyorsa VE eylem daha önce tetiklenmediyse...
    if (!longPressActionTriggered && (currentTime - authButtonPressStartTime >= LONG_PRESS_THRESHOLD_MS)) {
      if (currentAuthMode == AUTH_IDLE) {
        currentAuthMode = AUTH_SCANNING;
        scanModeEndTime = currentTime + SCAN_DURATION_MS;
        //g_pairingComplete = false;
        lastScanBroadcastTime = 0;
        logMessage("3-second hold detected: Entering SCANNING mode...", 0, settings.logLevel);
      }
      longPressActionTriggered = true; // Eylemin tetiklendiğini işaretle
    }
  } 
  // Buton basılı değilse (bırakılmışsa)...
  else {
    if (authButtonPressed) {
      // Buton AZ ÖNCE bırakıldı
      unsigned long pressDuration = currentTime - authButtonPressStartTime;
      logMessage("Auth button released. Duration: " + String(pressDuration) + "ms", 0, settings.logLevel);

      // Eğer bırakıldığında uzun basış eylemi tetiklenmediyse, bu bir kısa basıştır.
      // Gate için kısa basışa bir eylem atamadık, o yüzden bir şey yapmıyoruz.
      if (!longPressActionTriggered) {
        logMessage("SHORT PRESS: No action defined for Gate.", 0, settings.logLevel);
      }
      
      // Buton durumunu bir sonraki basış için sıfırla
      authButtonPressed = false;
    }
  }

  // --- Ana Durum Makinesi (State Machine) ---
  switch (currentAuthMode) {
    
    case AUTH_SCANNING:
      // Tarama modundaysak...
      if (currentTime >= scanModeEndTime) {
        logMessage("SCANNING mode timed out.", 0, settings.logLevel);
        currentAuthMode = AUTH_IDLE;
      } else if (currentTime - lastScanBroadcastTime >= BROADCAST_INTERVAL_MS) {
        // Belirtilen aralıklarla SCAN_REQUEST yayını yap.
        sendScanRequestBroadcast();
        lastScanBroadcastTime = currentTime;
      }
      break;

    case AUTH_SENDING_KEY:
      // Anahtar teslim modundaysak...
      if (currentTime - keySendStartTime > KEY_SEND_DURATION_MS) {
        logMessage("KEY_DELIVERY timeout for " + pendingVehicleMac + ". Pairing failed.", 0, settings.logLevel);
        uint8_t targetMac[6];
        sscanf(pendingVehicleMac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &targetMac[0], &targetMac[1], &targetMac[2], &targetMac[3], &targetMac[4], &targetMac[5]);
        esp_now_del_peer(targetMac);
        registeredDevices.erase(pendingVehicleMac);
        saveRegisteredDevices();
        currentAuthMode = AUTH_IDLE;
      } else if (currentTime - lastKeySendTime > KEY_SEND_INTERVAL_MS) {
        // Belirtilen aralıklarla KEY_DELIVERY mesajı gönder.
        JsonDocument keyDoc;
        keyDoc["msgType"] = "KEY_DELIVERY";
        keyDoc["key"] = pendingEncryptedKey;
        String output;
        serializeJson(keyDoc, output);
        
        uint8_t targetMac[6];
        sscanf(pendingVehicleMac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &targetMac[0], &targetMac[1], &targetMac[2], &targetMac[3], &targetMac[4], &targetMac[5]);
        esp_now_send(targetMac, (const uint8_t*)output.c_str(), output.length());
        lastKeySendTime = currentTime;
      }
      break;

    case AUTH_IDLE:
      // Boşta modundaysak...
      
      if (!registeredDevices.empty()) { 
        if (currentTime - lastBeaconTime > BEACON_INTERVAL_MS) {
            sendBeaconBroadcast();
            lastBeaconTime = currentTime;
        }
      }
      

      
      // Eşleşme bekleniyorsa hiçbir şey yapma.
      break;
  }
  
  vTaskDelay(pdMS_TO_TICKS(50)); // CPU'ya nefes aldır ve diğer görevlere zaman tanı.
}