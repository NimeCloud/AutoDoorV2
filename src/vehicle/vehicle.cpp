#include "common.cpp"

#define DEFAULT_DEVICE_NAME_PREFIX "VEHICLE_"

// Otomatik board tespiti
#if CONFIG_IDF_TARGET_ESP32C3

#define INTERNAL_LED_PIN 8 // ESP32 C3 Super Mini
#define LED_ON LOW
#define LED_OFF HIGH

#define AUTH_BUTTON_PIN 21

#elif CONFIG_IDF_TARGET_ESP32

#if defined(ENV_EKRANLI)
#define INTERNAL_LED_PIN 17 // 4 Kırmızı
#define LED_ON LOW
#define LED_OFF HIGH

#define AUTH_BUTTON_PIN 0

#else

#define INTERNAL_LED_PIN 2 // ESP32 WROOM-32
#define LED_ON HIGH
#define LED_OFF LOW

#define AUTH_BUTTON_PIN 21

#endif

#endif

#define FIXED_WIFI_CHANNEL 6 // WiFi kanalı (1, 6 veya 11 önerilir)

const char *buildDate = __DATE__;
const char *buildTime = __TIME__;

const unsigned long SCAN_DURATION_MS = 300000;

// vehicle.cpp -> Global Değişkenler bölümüne ekleyin
const bool FORCE_AUTO_SCAN_ON_STARTUP = false; // true: otomatik başlar, false: sadece butonla başlar
static bool autoScanTriggered = false;         // Bu özelliğin sadece bir kez çalışmasını sağlar

unsigned long startTime = 0;
const unsigned long defaultWarningDuration = 10000;
const unsigned long sendInterval = 1000;

unsigned long lastSendTime = 0;

static unsigned long lastSettingsChangeTime = 0;
const int SETTINGS_SAVE_DELAY = 1000;

std::map<String, String> pairedGates; // Key: Gate MAC, Value: Benzersiz Ortak Anahtar
String targetGateMac = "";
String lastNonceFromGate = "";

struct LedPattern
{
  int duration; // Her flash'ın süresi (ms)
  int count;    // Flash sayısı
};

QueueHandle_t ledQueue; // LED görevine komut göndermek için kuyruk


Preferences preferences;

// Cihaz ayarları yapısı
struct DeviceSettings
{
  int warnDuration;
  String deviceName;
  String pinCode; // Yeni: PIN kodu
  int logLevel;   // 0 = INFO, 1 = VERBOSE

  DeviceSettings()
      : deviceName(""),
        warnDuration(defaultWarningDuration),
        pinCode("") {} // Varsayılan boş PIN
};

DeviceSettings settings;

BLECharacteristic *pSettingsChar;
BLECharacteristic *pPinAuthChar;
BLECharacteristic *pStatusChar;
bool isSettingsChanged = false;
bool isDeviceNameChanged = false;
String bleMacAddress = "";

bool isAuthenticated = false; // Kullanıcının kimliği doğrulandı mı?


bool authButtonPressed = false;
unsigned long authButtonPressStartTime = 0;
bool longPressActionTriggered = false;
const unsigned long LONG_PRESS_THRESHOLD_MS = 3000;

bool isAutoSendingActive = false;
String manualCommandToSend = "";

// Yeni: Araç durumu için enum
enum VehicleState
{
  VEHICLE_IDLE,
  VEHICLE_WAITING_FOR_GATE_SCAN,
  VEHICLE_AUTHORIZED_ACTIVE
};

volatile VehicleState currentVehicleState = VEHICLE_IDLE;
uint8_t tempGatePeerMac[6] = {0};


// LED'i anlık yakıp söndüren FreeRTOS görevi
void ledBlinkTask(void *parameter) {
  LedPattern currentPattern;
  pinMode(INTERNAL_LED_PIN, OUTPUT);
  digitalWrite(INTERNAL_LED_PIN, LED_OFF);

  while (true) {
    // 1. Öncelik: Cihaz tarama modunda mı?
    if (currentVehicleState == VEHICLE_WAITING_FOR_GATE_SCAN) {
      digitalWrite(INTERNAL_LED_PIN, LED_ON);
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(INTERNAL_LED_PIN, LED_OFF);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue; // Döngüye devam et ki başka bir şey çalışmasın
    }

    // 2. Öncelik: Kuyrukta yeni bir desen komutu var mı?
    if (xQueueReceive(ledQueue, &currentPattern, pdMS_TO_TICKS(20)) == pdPASS) {
      // Evet, yeni bir desen geldi. Bunu uygula.
      for (int i = 0; i < currentPattern.count; i++) {
        digitalWrite(INTERNAL_LED_PIN, LED_ON);
        vTaskDelay(pdMS_TO_TICKS(currentPattern.duration));
        digitalWrite(INTERNAL_LED_PIN, LED_OFF);
        // Eğer son yanıp sönme değilse, arada bir bekleme süresi bırak
        if (i < currentPattern.count - 1) {
          vTaskDelay(pdMS_TO_TICKS(currentPattern.duration));
        }
      }
    } else {
      // Tarama modunda değilsek ve kuyruk boşsa, LED kapalı kalsın.
      digitalWrite(INTERNAL_LED_PIN, LED_OFF);
    }
  }
}

unsigned long parseWarningDuration(const std::string &s)
{
  char *endptr;
  unsigned long val = strtoul(s.c_str(), &endptr, 10);
  return (*endptr == '\0') ? val : 0;
}

unsigned long getWarningDuration()
{
  if (settings.warnDuration < 1000 || settings.warnDuration > SCAN_DURATION_MS)
  {
    return defaultWarningDuration;
  }
  return settings.warnDuration;
}

String getDeviceNameFromMac(String mac)
{
  mac.replace(":", "");
  return String(DEFAULT_DEVICE_NAME_PREFIX) + mac.substring(mac.length() - 4);
}

void printSettingsToSerial()
{
  JsonDocument jsonDoc;
  jsonDoc["warnDuration"] = settings.warnDuration;
  jsonDoc["deviceName"] = settings.deviceName;
  jsonDoc["pinExists"] = !settings.pinCode.isEmpty();
  jsonDoc["buildDate"] = buildDate;
  jsonDoc["buildTime"] = buildTime;

  String jsonString;
  serializeJson(jsonDoc, jsonString);
  Serial.println(jsonString);
}

void loadSettings()
{
  preferences.begin("settings", false);
  settings.warnDuration = preferences.getInt("warnDuration", defaultWarningDuration);
  settings.deviceName = preferences.getString("deviceName", "");
  settings.pinCode = preferences.getString("pinCode", "");
  preferences.end();

  if (settings.deviceName.isEmpty())
  {
    BLEDevice::init("TEMP");
    bleMacAddress = BLEDevice::getAddress().toString().c_str();
    BLEDevice::deinit();
    settings.deviceName = getDeviceNameFromMac(bleMacAddress);
    preferences.begin("settings", false);
    preferences.putString("deviceName", settings.deviceName);
    preferences.end();
    delay(100);
  }

  Serial.print(getTimestamp() + "Ayarlar yüklendi! JSON: ");
  printSettingsToSerial();
}

void saveSettings()
{
  preferences.begin("settings", false);
  preferences.putString("deviceName", settings.deviceName);
  preferences.putInt("warnDuration", settings.warnDuration);
  preferences.putString("pinCode", settings.pinCode);
  preferences.end();
  Serial.print(getTimestamp() + " Ayarlar NVRAM'e kaydedildi. Güncel Ayarlar: ");
  printSettingsToSerial();
  delay(100);
}

void sendStatusNotification(const String &statusMsg)
{
  if (pStatusChar)
  {
    BLEDescriptor *p2902Descriptor = pStatusChar->getDescriptorByUUID(BLEUUID((uint16_t)0x2902));

    if (p2902Descriptor && ((BLE2902 *)p2902Descriptor)->getNotifications())
    {
      pStatusChar->setValue(statusMsg.c_str());
      pStatusChar->notify();
      Serial.print(getTimestamp() + " Durum bildirimi gönderildi: ");
      Serial.println(statusMsg);
    }
    else
    {
      Serial.println(getTimestamp() + " Durum bildirimi gönderilemedi (client abone değil).");
    }
  }
  else
  {
    Serial.println(getTimestamp() + " Durum bildirimi gönderilemedi (pStatusChar null).");
  }
}

class MyCharacteristicCallbacks : public BLECharacteristicCallbacks
{

  void onWrite(BLECharacteristic *pCharacteristic)
  {
    if (!isAuthenticated && !settings.pinCode.isEmpty())
    {
      Serial.println(getTimestamp() + " Ayar yazma reddedildi: Kimlik doğrulanmadı.");
      sendStatusNotification("AUTH_REQUIRED");
      return;
    }

    std::string value = pCharacteristic->getValue();
    Serial.print(getTimestamp() + " Ayar karakteristikine gelen veri: ");
    Serial.println(value.c_str());

    if (!value.empty())
    {
      JsonDocument jsonDoc;
      DeserializationError error = deserializeJson(jsonDoc, value);

      if (!error)
      {
        Serial.println(getTimestamp() + " Ayar JSON başarıyla ayrıştırıldı.");
        bool anyChangeDetected = false;
        String newPinCode = "";

        if (jsonDoc["warnDuration"].is<int>())
        {
          int newWarnDuration = jsonDoc["warnDuration"];
          if (settings.warnDuration != newWarnDuration)
          {
            settings.warnDuration = newWarnDuration;
            anyChangeDetected = true;
            Serial.print(getTimestamp() + " warnDuration güncellendi: ");
            Serial.println(settings.warnDuration);
          }
        }

        if (jsonDoc["deviceName"].is<String>())
        {
          String newDeviceName = jsonDoc["deviceName"].as<String>();
          if (settings.deviceName != newDeviceName)
          {
            settings.deviceName = newDeviceName;
            anyChangeDetected = true;
            isDeviceNameChanged = true;
            Serial.print(getTimestamp() + " deviceName güncellendi: ");
            Serial.println(settings.deviceName);
          }
        }

        if (!jsonDoc["pinCode"].isNull())
        {
          newPinCode = jsonDoc["pinCode"].as<String>();

          if (newPinCode.isEmpty())
          {
            if (!settings.pinCode.isEmpty())
            {
              settings.pinCode = "";
              anyChangeDetected = true;
              Serial.println(getTimestamp() + " PIN sıfırlandı.");
              isAuthenticated = false;
              sendStatusNotification("PIN_RESET");
            }
            else
            {
              Serial.println(getTimestamp() + " PIN zaten boştu, sıfırlama işlemi yapılmadı.");
            }
          }
          else
          {
            if (newPinCode.length() == 4 && newPinCode.toInt() >= 0 && newPinCode.toInt() <= 9999)
            {
              if (settings.pinCode != newPinCode)
              {
                settings.pinCode = newPinCode;
                anyChangeDetected = true;
                Serial.print(getTimestamp() + " pinCode güncellendi: ");
                Serial.println(settings.pinCode);
                isAuthenticated = false;
                sendStatusNotification("PIN_CHANGED");
              }
              else
              {
                Serial.println(getTimestamp() + " Gelen PIN mevcut PIN ile aynı.");
              }
            }
            else
            {
              Serial.println(getTimestamp() + " Geçersiz PIN formatı! PIN güncellenmedi.");
              sendStatusNotification("INVALID_PIN_FORMAT");
            }
          }
        }

        if (anyChangeDetected)
        {
          isSettingsChanged = true;
          lastSettingsChangeTime = millis();
          Serial.println(getTimestamp() + " Ayarlarda değişiklik tespit edildi! Kayıt bekleniyor...");
          if (jsonDoc["pinCode"].isNull() || (newPinCode.length() == 4 || newPinCode.isEmpty()))
          {
            sendStatusNotification("SETTINGS_UPDATED");
          }
        }
        else
        {
          Serial.println(getTimestamp() + " Gelen JSON'da mevcut ayarlarla farklılık yok.");
          sendStatusNotification("NO_CHANGE");
        }
      }
      else
      {
        Serial.print(getTimestamp() + " JSON ayrıştırma hatası: ");
        Serial.println(error.c_str());
        sendStatusNotification("JSON_ERROR");
      }
    }
  }

  void onRead(BLECharacteristic *pCharacteristic)
  {
    if (!isAuthenticated && !settings.pinCode.isEmpty())
    {
      Serial.println(getTimestamp() + " Ayar okuma reddedildi: Kimlik doğrulanmadı.");
      pCharacteristic->setValue("{\"error\": \"Authentication required\"}");
      sendStatusNotification("AUTH_REQUIRED");
      return;
    }

    JsonDocument jsonDoc;
    jsonDoc["warnDuration"] = settings.warnDuration;
    jsonDoc["deviceName"] = settings.deviceName;
    jsonDoc["pinExists"] = !settings.pinCode.isEmpty();
    jsonDoc["buildDate"] = buildDate;
    jsonDoc["buildTime"] = buildTime;

    String jsonString;
    serializeJson(jsonDoc, jsonString);

    Serial.print(getTimestamp() + " Ayarlar okuma talebi alındı. Gönderilen JSON: ");
    Serial.println(jsonString);

    pCharacteristic->setValue(jsonString.c_str());
  }
};

class PinAuthCharacteristicCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string value = pCharacteristic->getValue();
    Serial.print(getTimestamp() + " PIN doğrulama karakteristikine gelen veri: ");
    Serial.println(value.c_str());

    if (!value.empty())
    {
      if (settings.pinCode.isEmpty())
      {
        isAuthenticated = true;
        Serial.println(getTimestamp() + " PIN ayarlı değil, otomatik kimlik doğrulandı.");
        sendStatusNotification("AUTH_SUCCESS");
      }
      else if (value == settings.pinCode.c_str())
      {
        isAuthenticated = true;
        Serial.println(getTimestamp() + " PIN doğrulama başarılı!");
        sendStatusNotification("AUTH_SUCCESS");
      }
      else
      {
        isAuthenticated = false;
        Serial.println(getTimestamp() + " PIN doğrulama başarısız! Yanlış PIN.");
        sendStatusNotification("AUTH_FAILED");
      }
    }
    else
    {
      Serial.println(getTimestamp() + " PIN doğrulama karakteristikine boş veri geldi.");
      sendStatusNotification("INVALID_PIN_FORMAT");
    }
  }

  void onRead(BLECharacteristic *pCharacteristic)
  {
    Serial.println(getTimestamp() + " PIN auth okuma talebi alındı. PIN durumu gönderiliyor.");
    JsonDocument jsonDoc;
    jsonDoc["pinRequired"] = !settings.pinCode.isEmpty();
    String jsonString;
    serializeJson(jsonDoc, jsonString);
    pCharacteristic->setValue(jsonString.c_str());
  }
};

class MyServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer)
  {
    Serial.println(getTimestamp() + " Bir client bağlandı.");
    isAuthenticated = false;
  }

  void onDisconnect(BLEServer *pServer)
  {
    Serial.println(getTimestamp() + " Client bağlantısı kesildi.");
    BLEDevice::startAdvertising(); // Reklamı tekrar başlat
  }
};

bool isGatePaired(const String &mac)
{
  // std::map'in .count() metodu, verilen anahtarın map'te olup olmadığını
  // (0 veya 1) çok hızlı bir şekilde döndürür.
  return pairedGates.count(mac) > 0;
}
// vehicle.cpp -> savePairedGates fonksiyonunu bununla değiştirin
void savePairedGates()
{
  preferences.begin("paired_gates", false);

  JsonDocument doc;
  // Her zaman bir JSON dizisi oluşturarak başla
  JsonArray array = doc.to<JsonArray>();

  // Map'teki her bir [mac, key] çiftini bu diziye yeni bir nesne olarak ekle
  for (auto const &pair : pairedGates)
  {
    JsonObject device = array.add<JsonObject>(); // <<< ÖNEMLİ DEĞİŞİKLİK
    device["mac"] = pair.first;
    device["key"] = pair.second;
  }

  String jsonOutput;
  serializeJson(doc, jsonOutput);

  preferences.putString("gates_json", jsonOutput);

  preferences.end();
  delay(100);
  logMessage("Saved " + String(pairedGates.size()) + " paired Gates.", 0, settings.logLevel);
}

void loadPairedGates()
{
  pairedGates.clear();                     // Yüklemeden önce mevcut listeyi temizle
  preferences.begin("paired_gates", true); // Sadece okuma modunda aç

  // Kayıtlı JSON metnini oku
  String jsonInput = preferences.getString("gates_json", "");

  if (jsonInput.length() > 0)
  {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonInput);

    if (!error)
    {
      JsonArray array = doc.as<JsonArray>();
      // JSON dizisindeki her bir objeyi map'e geri yükle
      for (JsonObject device : array)
      {
        String mac = device["mac"];
        String key = device["key"];
        if (mac.length() > 0 && key.length() > 0)
        {
          pairedGates[mac] = key;
        }
      }
    }
    else
    {
      logMessage("Failed to parse paired gates JSON.", 0, settings.logLevel);
    }
  }

  preferences.end();
  logMessage("Loaded " + String(pairedGates.size()) + " paired Gates.", 0, settings.logLevel);
}

// Eşleşilen Gate'leri başlangıçta seri monitöre yazdırır
void printPairedGates()
{
  logMessage("--- Paired Gates (MAC -> Unique Key) ---", 0, settings.logLevel);
  if (pairedGates.empty())
  {
    logMessage("  No paired Gates found.", 0, settings.logLevel);
  }
  else
  {
    for (auto const &pair : pairedGates)
    { // <-- Döngüyü bu şekilde değiştirin
      String mac = pair.first;
      String key = pair.second;
      logMessage("  - " + mac + " -> " + key.substring(0, 8) + "...", 0, settings.logLevel);
    }
  }
  logMessage("----------------------------------------", 0, settings.logLevel);
}

// vehicle.cpp -> sendSecureCommand fonksiyonu
void sendSecureCommand(String command)
{
  // Hedef kapı seçilmemişse komut gönderme
  if (targetGateMac.isEmpty())
  {
    logMessage("Cannot send command: No target gate selected.", 0, settings.logLevel);
    return;
  }

  // --- "Kullan ve Sil" Peer Yönetimi: Adım 1 ---
  // Gönderimden hemen önce peer'i ekle
  uint8_t targetMacBytes[6];
  sscanf(targetGateMac.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
         &targetMacBytes[0], &targetMacBytes[1], &targetMacBytes[2], &targetMacBytes[3], &targetMacBytes[4], &targetMacBytes[5]);

  if (!esp_now_is_peer_exist(targetMacBytes))
  {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, targetMacBytes, 6);
    peerInfo.channel = FIXED_WIFI_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK)
    {
      logMessage("Failed to add temporary peer for command.", 0, settings.logLevel);
      return;
    }
  }

  // --- Mesaj Oluşturma ---
  JsonDocument doc;
  doc["msgType"] = "COMMAND";
  doc["command"] = command;

  // --- Güvenlik Modu Kontrolü ---
  // Eğer son beacon'dan bir nonce aldıysak, bu güvenli mod demektir.
  if (!lastNonceFromGate.isEmpty())
  {
    logMessage("Sending SECURE command: " + command, 0, settings.logLevel);

    String sharedKeyHex = pairedGates[targetGateMac];
    if (sharedKeyHex.isEmpty())
    {
      logMessage("Cannot send secure command: No key for gate " + targetGateMac, 0, settings.logLevel);
      return;
    }

    uint8_t sharedKey[16];
    for (int i = 0; i < 16; i++)
    {
      sscanf(sharedKeyHex.c_str() + i * 2, "%2hhx", &sharedKey[i]);
    }

    String vehicleMac = WiFi.macAddress();
    String dataToSign = vehicleMac + command + lastNonceFromGate;
    String hmac = calculateHmac(sharedKey, 16, dataToSign.c_str());

    // Güvenli modda nonce ve hmac'i mesaja ekle
    doc["nonce"] = lastNonceFromGate;
    doc["hmac"] = hmac;
  }
  // Nonce almadıysak, bu güvensiz mod demektir.
  else
  {
    logMessage("Sending INSECURE command: " + command, 0, settings.logLevel);
    // HMAC ve nonce eklemeden direkt gönder.
  }

  char jsonBuffer[300]; // HMAC uzun olabileceği için buffer boyutu güvenli tarafta
  serializeJson(doc, jsonBuffer);

  // Mesajı gönder
  esp_now_send(targetMacBytes, (const uint8_t *)jsonBuffer, strlen(jsonBuffer));

  // OnDataSent callback'i, gönderimden sonra peer'i otomatik olarak silecektir.
}

// ESP-NOW veri alındığında çağrılır

void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len)
{

  String message = String((char *)incomingData, len);
  Serial.print("Data as String: ");
  Serial.println(message);

  LedPattern p_recv = {50, 1}; // 50ms, 1 kere yanıp sön
  xQueueSend(ledQueue, &p_recv, 0);


  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, incomingData, len);
  if (error)
  {
    logMessage("JSON parsing error!", 1, settings.logLevel);
    return;
  }

  const char *msgType = doc["msgType"];
  if (!msgType)
  {
    logMessage("msgType field missing!", 1, settings.logLevel);
    return;
  }

  String gateMac = macToString(mac_addr);

  // --- 1. EŞLEŞME ADIMI: Tarama İsteğine Cevap ---
  if (strcmp(msgType, "SCAN_REQUEST") == 0 && currentVehicleState == VEHICLE_WAITING_FOR_GATE_SCAN)
  {
    logMessage("SCAN_REQUEST recieved! Replying with AUTH_ACK...", 0, settings.logLevel);

    // Cevap gönderilecek Gate'i geçici olarak peer listesine ekle
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac_addr, 6);
    peerInfo.channel = FIXED_WIFI_CHANNEL;
    peerInfo.encrypt = false;

    if (!esp_now_is_peer_exist(mac_addr))
    {
      if (esp_now_add_peer(&peerInfo) != ESP_OK)
      {
        logMessage("Failed to add Gate as a peer.", 0, settings.logLevel);
        return;
      }
    }

    // AUTH_ACK mesajını oluştur ve gönder
    JsonDocument ackDoc;
    ackDoc["msgType"] = "AUTH_ACK";
    char jsonBuffer[64];
    serializeJson(ackDoc, jsonBuffer);
    esp_now_send(mac_addr, (const uint8_t *)jsonBuffer, strlen(jsonBuffer));
  }
  // --- 2. EŞLEŞME ADIMI: Anahtar Teslimini İşleme ---
  else if (strcmp(msgType, "KEY_DELIVERY") == 0)
  {
    const char *encryptedKey = doc["key"];
    if (encryptedKey)
    {
      // Gelen anahtarı PMK ile çöz
      String decryptedKeyRaw = encryptDecrypt(String(encryptedKey), PMK, sizeof(PMK));

      // Çözülmüş ham byte'ları Hex String'e çevir (saklamak için)
      char decryptedKeyHex[33];
      for (int i = 0; i < decryptedKeyRaw.length(); i++)
      {
        sprintf(decryptedKeyHex + i * 2, "%02x", (uint8_t)decryptedKeyRaw[i]);
      }
      decryptedKeyHex[32] = '\0';

      // Yeni anahtarı Gate'in MAC adresiyle birlikte kaydet
      pairedGates[gateMac] = String(decryptedKeyHex);
      savePairedGates();

      // Gate'e her şeyin yolunda olduğunu bildir (KEY_ACK)
      JsonDocument keyAckDoc;
      keyAckDoc["msgType"] = "KEY_ACK";
      char ackBuffer[32];
      serializeJson(keyAckDoc, ackBuffer);
      esp_now_send(mac_addr, (const uint8_t *)ackBuffer, strlen(ackBuffer));

      logMessage("Stored new key for " + gateMac + ". Sent KEY_ACK.", 0, settings.logLevel);

      // Eşleşme tamamlandı, tarama modundan çık ve başarı LED'ini yak
      currentVehicleState = VEHICLE_IDLE;
      LedPattern p_success = {10000, 1}; // 10 saniye boyunca yanık kal
      xQueueSend(ledQueue, &p_success, 0);

    }
  }
  // --- 3. NORMAL ÇALIŞMA: Beacon'ları Dinleyip Otomatik Komut Gönderme ---
  else if (strcmp(msgType, "BEACON") == 0)
  {

      LedPattern p_success = {200, 1};
      xQueueSend(ledQueue, &p_success, 0);
    

    // Bu beacon, bizim daha önce eşleştiğimiz bir kapıdan mı geliyor?
    if (pairedGates.count(gateMac))
    {
      // Evet, tanıdık bir kapı.

      // Gerekli bilgileri al (nonce ve hedef kapı MAC)
      // --- YENİ VE DOĞRU NONCE KONTROLÜ ---
        // Beacon içinde "nonce" anahtarı var mı diye kontrol et
        if (!doc["nonce"].isNull()) { // <<< DEĞİŞTİRİLMİŞ SATIR
            // Evet var, güvenli moddayız. Nonce'ı kaydet.
            lastNonceFromGate = doc["nonce"].as<String>();
        } else {
            // Hayır yok, güvensiz moddayız. Nonce'ı TEMİZLE.
            // !!! SORUNUN ÇÖZÜMÜ BU SATIR !!!
            lastNonceFromGate = ""; 
        }
      targetGateMac = gateMac;

      String commandToSend;
      // Cihazın çalışma süresi, ayarlardaki uyarı süresinden az mı?
      if (millis() < settings.warnDuration)
      {
        commandToSend = "WARN"; // Evet, o zaman sadece uyar.
      }
      else
      {
        commandToSend = "OPEN"; // Hayır, bir süredir çalışıyor, kapıyı aç.
      }

      // Güvenli komutu gönder
      sendSecureCommand(commandToSend);
    }
  }
}

// ESP-NOW veri gönderildiğinde çağrılır
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  logMessage("Last Packet to " + macToString(mac_addr) + " Send Status: " + (status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail"), 1, settings.logLevel);

  // Gönderim bittikten sonra, geçici olarak eklediğimiz peer'i sil.
  // Not: broadcast (FF:FF:..) adresini silmemeye dikkat et!
  bool isBroadcast = true;
  for (int i = 0; i < 6; ++i)
    if (mac_addr[i] != 0xFF)
      isBroadcast = false;

  if (!isBroadcast)
  {
    esp_now_del_peer(mac_addr);
  }
}

void setup()
{
  Serial.begin(115200);
  Serial.println(getTimestamp() + " Vehicle Control System Starting...");

  pinMode(INTERNAL_LED_PIN, OUTPUT);
  digitalWrite(INTERNAL_LED_PIN, LED_OFF);
  pinMode(AUTH_BUTTON_PIN, INPUT_PULLUP);

  // Ayarları ve kayıtlı mac id'leri yükle
  loadSettings();
  printSettingsToSerial();

  loadPairedGates();
  printPairedGates();

  // WiFi'yi istasyon moduna ayarla
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.println(getTimestamp() + " WiFi set to Station Mode.");

  // ESP-NOW'ı başlat
  if (esp_now_init() != ESP_OK)
  {
    Serial.println(getTimestamp() + " Error initializing ESP-NOW");
    return;
  }
  Serial.println(getTimestamp() + " ESP-NOW initialized.");

  // ESP-NOW callback fonksiyonlarını kaydet
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);
  Serial.println(getTimestamp() + " ESP-NOW receiver ready");

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
    Serial.println(getTimestamp() + " Failed to add broadcast peer");
    return;
  }
  Serial.println(getTimestamp() + " Broadcast peer added.");

  // BLE başlatma
  BLEDevice::init(settings.deviceName.c_str());
  BLEDevice::setMTU(200); // MTU boyutunu artır
  bleMacAddress = BLEDevice::getAddress().toString().c_str();
  Serial.println(String(getTimestamp()) + " BLE MAC Address: " + bleMacAddress);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pSettingsChar = pService->createCharacteristic(
      CHAR_SETTINGS_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE);
  pSettingsChar->setCallbacks(new MyCharacteristicCallbacks());

  pPinAuthChar = pService->createCharacteristic(
      CHAR_PIN_AUTH_UUID,
      BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_READ);
  pPinAuthChar->setCallbacks(new PinAuthCharacteristicCallbacks());

  pStatusChar = pService->createCharacteristic(
      CHAR_STATUS_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  pStatusChar->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println(getTimestamp() + " BLE server başladı, ayarlar bekleniyor...");

  digitalWrite(INTERNAL_LED_PIN, LED_ON);
  delay(1000);
  digitalWrite(INTERNAL_LED_PIN, LED_OFF);
  delay(1000);

   // LED kuyruğunu oluştur
  ledQueue = xQueueCreate(10, sizeof(LedPattern));
  if (ledQueue == NULL) {
    logMessage("Failed to create LED queue", 0, settings.logLevel);
  }

  // LED görevini başlat
  xTaskCreatePinnedToCore(
      ledBlinkTask,
      "LED Blink Task",
      1024, // Stack size
      NULL,
      1,    // Priority
      NULL, // Task handle
      0     // Core
  );

  Serial.println(getTimestamp() + " Setup tamamlandı...");
}

void loop()
{
  unsigned long currentTime = millis();

  // --- Başlangıçta Otomatik Tarama Tetikleyicisi ---
  // Cihaz açıldıktan 3 saniye sonra, eğer ayarlanmışsa, otomatik olarak eşleşme moduna girer.
  // Not: Bu blok, sizin isteğiniz üzerine, cihazda kayıtlı kapı olsa bile çalışır.
  if (FORCE_AUTO_SCAN_ON_STARTUP && !autoScanTriggered && currentTime >= 3000)
  {
    if (currentVehicleState == VEHICLE_IDLE)
    {
      currentVehicleState = VEHICLE_WAITING_FOR_GATE_SCAN;
      logMessage("FORCED AUTO-SCAN: Entering WAITING_FOR_GATE_SCAN mode...", 0, settings.logLevel);
    }
    autoScanTriggered = true; // Bu bloğun tekrar çalışmasını engelle
  }

  // --- Ayarların Gerekirse Kaydedilmesi ---
  if (isSettingsChanged && (currentTime - lastSettingsChangeTime >= SETTINGS_SAVE_DELAY))
  {
    saveSettings();
    isSettingsChanged = false;
  }

  // --- Yeni Buton Yönetim Mantığı ---
  bool isButtonPressed = (digitalRead(AUTH_BUTTON_PIN) == LOW);

  // Buton basılı tutuluyorsa...
  if (isButtonPressed)
  {
    // Eğer butona YENİ basıldıysa (ilk an)
    if (!authButtonPressed)
    {
      authButtonPressed = true;
      longPressActionTriggered = false; // Her yeni basışta eylem bayrağını sıfırla
      authButtonPressStartTime = currentTime;
      logMessage("Auth button pressed.", 0, settings.logLevel);
    }

    // Buton 3 saniyedir basılı tutuluyorsa VE uzun basış eylemi daha önce tetiklenmediyse...
    if (!longPressActionTriggered && (currentTime - authButtonPressStartTime >= LONG_PRESS_THRESHOLD_MS))
    {
      if (currentVehicleState == VEHICLE_IDLE)
      {
        currentVehicleState = VEHICLE_WAITING_FOR_GATE_SCAN;
        logMessage("3-second hold detected: Entering WAITING_FOR_GATE_SCAN mode...", 0, settings.logLevel);
      }
      longPressActionTriggered = true; // Eylemin tetiklendiğini işaretle ki tekrar çalışmasın
    }
  }
  // Buton basılı değilse (bırakılmışsa)...
  else
  {
    // Eğer buton AZ ÖNCE bırakıldıysa
    if (authButtonPressed)
    {
      unsigned long pressDuration = currentTime - authButtonPressStartTime;
      logMessage("Auth button released. Duration: " + String(pressDuration) + "ms", 0, settings.logLevel);

      // Eğer buton bırakıldığında uzun basış eylemi HİÇ tetiklenmediyse, bu bir kısa basıştır.
      if (!longPressActionTriggered)
      {
        logMessage("SHORT PRESS: Attempting to send COMMAND 'OPEN'", 0, settings.logLevel);
        sendSecureCommand("OPEN");
      }

      // Buton durumunu bir sonraki basış için sıfırla
      authButtonPressed = false;
    }
  }

  // --- Seri Monitörden Gelen Manuel Komutlar ---
  if (Serial.available())
  {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input == "open")
    {
      logMessage("Manual command: OPEN", 0, settings.logLevel);
      sendSecureCommand("OPEN");
    }
    else if (input == "close")
    {
      logMessage("Manual command: CLOSE", 0, settings.logLevel);
      sendSecureCommand("CLOSE");
    }
    // Buraya başka manuel komutlar ekleyebilirsiniz...
  }

  vTaskDelay(pdMS_TO_TICKS(20)); // CPU'ya nefes aldır.
}