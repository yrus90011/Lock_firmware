#include <cstdint>
#include <memory>
#define JSON_NOEXCEPTION 1
#include <sodium/crypto_sign.h>
#include <sodium/crypto_box.h>
#include "HAP.h"
#include "hkAuthContext.h"
#include "HomeKey.h"
#include "array"
#include "logging.h"
#include "HomeSpan.h"
#include "PN532_SPI.h"
#include "PN532.h"
#include "chrono"
#include "ESPAsyncWebServer.h"
#include "LittleFS.h"
#include "HK_HomeKit.h"
#include "config.h"
#include "esp_app_desc.h"
#include "pins_arduino.h"
#include "NFC_SERV_CHARS.h"
#include <mbedtls/sha256.h>
#include <esp_mac.h>
#include <esp_now.h>

extern "C" {
#include "mpr121.h"
}

const char* TAG = "MAIN";

AsyncWebServer webServer(80);
PN532_SPI *pn532spi;
PN532 *nfc;

static EventGroupHandle_t g_net_eg;
#define NET_READY_BIT BIT0

typedef struct {
  char api_base[192];
  char device_uuid[64];
  char token[768];
} device_ctx_t;

static device_ctx_t g_ctx = {};

uint8_t receiverMAC[] = {0x9C, 0x9E, 0x6E, 0xC3, 0x79, 0x84};

void onSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

// --- IO pins (change to your real pins) ---
static constexpr int PIN_BUZZER    = 33;
static constexpr int PIN_RED       = 27;
static constexpr int PIN_BACKLIGHT = 26;
static constexpr int PIN_GREEN     = 25;

// Map electrode index -> digit (from your example)
static const int8_t touchPinMap[12] = { 1, 2, 11, 9, 6, 8, 0, 3, 5, 10, 7, 4 };

// Special keys (based on your example logic)
static constexpr int ELE_CANCEL = 10;   // long beep cancel
static constexpr int ELE_ENTER  = 11;  // enter / change pin

static char correctPIN[16] = "927410";
static char masterPIN[16]  = "990011";

// ---------- ESP-NOW TX task ----------
typedef struct {
  char uid[40];     // enough for up to 16-byte UID hex + null (32 chars max) + safety
  uint8_t len;      // strlen(uid) + 1 (include null)
} espnow_msg_t;

QueueHandle_t espnow_queue = nullptr;
TaskHandle_t espnow_task_handle = nullptr;
static bool espnow_initialized = false;

QueueHandle_t gpio_lock_handle = nullptr;
TaskHandle_t gpio_lock_task_handle = nullptr;
TaskHandle_t nfc_reconnect_task = nullptr;
TaskHandle_t nfc_poll_task = nullptr;
TaskHandle_t mpr121_task_handle = nullptr;

nvs_handle savedData;
readerData_t readerData;
uint8_t ecpData[18] = { 0x6A, 0x2, 0xCB, 0x2, 0x6, 0x2, 0x11, 0x0 };
const std::array<std::array<uint8_t, 6>, 4> hk_color_vals = { {{0x01,0x04,0xce,0xd5,0xda,0x00}, {0x01,0x04,0xaa,0xd6,0xec,0x00}, {0x01,0x04,0xe3,0xe3,0xe3,0x00}, {0x01,0x04,0x00,0x00,0x00,0x00}} };
const std::array<const char*, 6> pixelTypeMap = { "RGB", "RBG", "BRG", "BGR", "GBR", "GRB" };
struct gpioLockAction
{
  enum
  {
    HOMEKIT = 1,
    HOMEKEY = 2,
    OTHER = 3
  };
  uint8_t source;
  uint8_t action;
};

std::string platform_create_id_string(void) {
  uint8_t mac[6];
  char id_string[13];
  esp_read_mac(mac, ESP_MAC_BT);
  sprintf(id_string, "ESP32_%02x%02X%02X", mac[3], mac[4], mac[5]);
  return std::string(id_string);
}

namespace espConfig
{
  struct misc_config_t
  {
    enum colorMap
    {
      R,
      G,
      B
    };
    std::string deviceName = DEVICE_NAME;
    std::string otaPasswd = OTA_PWD;
    uint8_t hk_key_color = HOMEKEY_COLOR;
    std::string setupCode = SETUP_CODE;
    bool lockAlwaysUnlock = HOMEKEY_ALWAYS_UNLOCK;
    bool lockAlwaysLock = HOMEKEY_ALWAYS_LOCK;
    uint8_t controlPin = HS_PIN;
    uint8_t hsStatusPin = HS_STATUS_LED;
    uint8_t gpioActionPin = GPIO_ACTION_PIN;
    bool gpioActionLockState = GPIO_ACTION_LOCK_STATE;
    bool gpioActionUnlockState = GPIO_ACTION_UNLOCK_STATE;
    uint8_t gpioActionMomentaryEnabled = GPIO_ACTION_MOMENTARY_STATE;
    bool hkGpioControlledState = true;
    uint16_t gpioActionMomentaryTimeout = GPIO_ACTION_MOMENTARY_TIMEOUT;
    bool webAuthEnabled = WEB_AUTH_ENABLED;
    std::string webUsername = WEB_AUTH_USERNAME;
    std::string webPassword = WEB_AUTH_PASSWORD;
    std::array<uint8_t, 4> nfcGpioPins{SS, SCK, MISO, MOSI};
    uint8_t btrLowStatusThreshold = 10;
    bool proxBatEnabled = false;
    bool hkDumbSwitchMode = false;
    uint8_t hkAltActionInitPin = GPIO_HK_ALT_ACTION_INIT_PIN;
    uint8_t hkAltActionInitLedPin = GPIO_HK_ALT_ACTION_INIT_LED_PIN;
    uint16_t hkAltActionInitTimeout = GPIO_HK_ALT_ACTION_INIT_TIMEOUT;
    uint8_t hkAltActionPin = GPIO_HK_ALT_ACTION_PIN;
    uint16_t hkAltActionTimeout = GPIO_HK_ALT_ACTION_TIMEOUT;
    uint8_t hkAltActionGpioState = GPIO_HK_ALT_ACTION_GPIO_STATE;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
    misc_config_t, deviceName, otaPasswd, hk_key_color, setupCode,
    lockAlwaysUnlock, lockAlwaysLock, controlPin, hsStatusPin,
    gpioActionPin, gpioActionLockState, gpioActionUnlockState,
    gpioActionMomentaryEnabled, gpioActionMomentaryTimeout, webAuthEnabled,
    webUsername, webPassword, nfcGpioPins, btrLowStatusThreshold,
    proxBatEnabled, hkDumbSwitchMode, hkAltActionInitPin,
    hkAltActionInitLedPin, hkAltActionInitTimeout, hkAltActionPin,
    hkAltActionTimeout, hkAltActionGpioState, hkGpioControlledState
  )
  } miscConfig;
}; // namespace espConfig

KeyFlow hkFlow = KeyFlow::kFlowFAST;
bool hkAltActionActive = false;
SpanCharacteristic* lockCurrentState;
SpanCharacteristic* lockTargetState;
SpanCharacteristic* statusLowBtr;
SpanCharacteristic* btrLevel;

bool save_to_nvs() {
  std::vector<uint8_t> serialized = nlohmann::json::to_msgpack(readerData);
  esp_err_t set_nvs = nvs_set_blob(savedData, "READERDATA", serialized.data(), serialized.size());
  esp_err_t commit_nvs = nvs_commit(savedData);
  LOG(D, "NVS SET STATUS: %s", esp_err_to_name(set_nvs));
  LOG(D, "NVS COMMIT STATUS: %s", esp_err_to_name(commit_nvs));
  return !set_nvs && !commit_nvs;
}

struct PhysicalLockBattery : Service::BatteryService
{
  PhysicalLockBattery() {
    LOG(I, "Configuring PhysicalLockBattery");
    statusLowBtr = new Characteristic::StatusLowBattery(0, true);
    btrLevel = new Characteristic::BatteryLevel(100, true);
  }
};

struct LockManagement : Service::LockManagement
{
  SpanCharacteristic* lockControlPoint;
  SpanCharacteristic* version;
  const char* TAG = "LockManagement";

  LockManagement() : Service::LockManagement() {

    LOG(I, "Configuring LockManagement"); // initialization message

    lockControlPoint = new Characteristic::LockControlPoint();
    version = new Characteristic::Version();

  } // end constructor

}; // end LockManagement

struct NFCAccessoryInformation : Service::AccessoryInformation
{
  const char* TAG = "NFCAccessoryInformation";

  NFCAccessoryInformation() : Service::AccessoryInformation() {

    LOG(I, "Configuring NFCAccessoryInformation"); // initialization message

    opt.push_back(&_CUSTOM_HardwareFinish);
    new Characteristic::Identify();
    new Characteristic::Manufacturer("rednblkx");
    new Characteristic::Model("HomeKey-ESP32");
    new Characteristic::Name(DEVICE_NAME);
    const esp_app_desc_t* app_desc = esp_app_get_description();
    std::string app_version = app_desc->version;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char macStr[9] = { 0 };
    sprintf(macStr, "%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
    std::string serialNumber = "HK-";
    serialNumber.append(macStr);
    new Characteristic::SerialNumber(serialNumber.c_str());
    new Characteristic::FirmwareRevision(app_version.c_str());
    std::array<uint8_t, 6> decB64 = hk_color_vals[HK_COLOR(espConfig::miscConfig.hk_key_color)];
    TLV8 hwfinish(NULL, 0);
    hwfinish.unpack(decB64.data(), decB64.size());
    new Characteristic::HardwareFinish(hwfinish);

  } // end constructor
};

// Function to calculate CRC16
void crc16a(unsigned char* data, unsigned int size, unsigned char* result) {
  unsigned short w_crc = 0x6363;

  for (unsigned int i = 0; i < size; ++i) {
    unsigned char byte = data[i];
    byte = (byte ^ (w_crc & 0x00FF));
    byte = ((byte ^ (byte << 4)) & 0xFF);
    w_crc = ((w_crc >> 8) ^ (byte << 8) ^ (byte << 3) ^ (byte >> 4)) & 0xFFFF;
  }

  result[0] = static_cast<unsigned char>(w_crc & 0xFF);
  result[1] = static_cast<unsigned char>((w_crc >> 8) & 0xFF);
}

// Function to append CRC16 to data
void with_crc16(unsigned char* data, unsigned int size, unsigned char* result) {
  crc16a(data, size, result);
}

void gpio_task(void* arg) {
  gpioLockAction status;
  while (1) {
    if (gpio_lock_handle != nullptr) {
      status = {};
      if (uxQueueMessagesWaiting(gpio_lock_handle) > 0) {
        xQueueReceive(gpio_lock_handle, &status, 0);
        LOG(D, "Got something in queue - source = %d action = %d", status.source, status.action);
        if (status.action == 0) {
          LOG(D, "%d - %d - %d -%d", espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionMomentaryEnabled, espConfig::miscConfig.lockAlwaysUnlock, espConfig::miscConfig.lockAlwaysLock);
          if (espConfig::miscConfig.lockAlwaysUnlock && status.source != gpioLockAction::HOMEKIT) {
            lockTargetState->setVal(lockStates::UNLOCKED);
            if(espConfig::miscConfig.gpioActionPin != 255){
              digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionUnlockState);
            }
            lockCurrentState->setVal(lockStates::UNLOCKED);

            if (static_cast<uint8_t>(espConfig::miscConfig.gpioActionMomentaryEnabled) & status.source) {
              delay(espConfig::miscConfig.gpioActionMomentaryTimeout);
              lockTargetState->setVal(lockStates::LOCKED);
              if(espConfig::miscConfig.gpioActionPin != 255){
                digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
              }
              lockCurrentState->setVal(lockStates::LOCKED);
            }
          } else if (espConfig::miscConfig.lockAlwaysLock && status.source != gpioLockAction::HOMEKIT) {
            lockTargetState->setVal(lockStates::LOCKED);
            if(espConfig::miscConfig.gpioActionPin != 255){
              digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
            }
            lockCurrentState->setVal(lockStates::LOCKED);

          } else {
            int currentState = lockCurrentState->getVal();
            if (status.source != gpioLockAction::HOMEKIT) {
              lockTargetState->setVal(!currentState);
            }
            if(espConfig::miscConfig.gpioActionPin != 255){
              digitalWrite(espConfig::miscConfig.gpioActionPin, currentState == lockStates::UNLOCKED ? espConfig::miscConfig.gpioActionLockState : espConfig::miscConfig.gpioActionUnlockState);
            }
            lockCurrentState->setVal(!currentState);

            if ((static_cast<uint8_t>(espConfig::miscConfig.gpioActionMomentaryEnabled) & status.source) && currentState == lockStates::LOCKED) {
              delay(espConfig::miscConfig.gpioActionMomentaryTimeout);
              lockTargetState->setVal(currentState);
              if(espConfig::miscConfig.gpioActionPin != 255){
                digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
              }
              lockCurrentState->setVal(currentState);
            }
          }
        } else if (status.action == 2) {
          vTaskDelete(NULL);
          return;
        }
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

struct LockMechanism : Service::LockMechanism
{
  const char* TAG = "LockMechanism";

  LockMechanism() : Service::LockMechanism() {
    LOG(I, "Configuring LockMechanism"); // initialization message
    lockCurrentState = new Characteristic::LockCurrentState(1, true);
    lockTargetState = new Characteristic::LockTargetState(1, true);
    memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());
    with_crc16(ecpData, 16, ecpData + 16);
    if (espConfig::miscConfig.gpioActionPin != 255) {
      if (lockCurrentState->getVal() == lockStates::LOCKED) {
        digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionLockState);
      } else if (lockCurrentState->getVal() == lockStates::UNLOCKED) {
        digitalWrite(espConfig::miscConfig.gpioActionPin, espConfig::miscConfig.gpioActionUnlockState);
      }
    }
  } // end constructor

  boolean update() {
    int targetState = lockTargetState->getNewVal();
    LOG(I, "New LockState=%d, Current LockState=%d", targetState, lockCurrentState->getVal());
    if (espConfig::miscConfig.gpioActionPin != 255) {
      const gpioLockAction gpioAction{ .source = gpioLockAction::HOMEKIT, .action = 0 };
      xQueueSend(gpio_lock_handle, &gpioAction, 0);
    } else if (espConfig::miscConfig.hkDumbSwitchMode) {
      const gpioLockAction gpioAction{ .source = gpioLockAction::HOMEKIT, .action = 0 };
      xQueueSend(gpio_lock_handle, &gpioAction, 0);
    }
    return (true);
  }
};

struct NFCAccess : Service::NFCAccess
{
  SpanCharacteristic* configurationState;
  SpanCharacteristic* nfcControlPoint;
  SpanCharacteristic* nfcSupportedConfiguration;
  const char* TAG = "NFCAccess";

  NFCAccess() : Service::NFCAccess() {
    LOG(I, "Configuring NFCAccess"); // initialization message
    configurationState = new Characteristic::ConfigurationState();
    nfcControlPoint = new Characteristic::NFCAccessControlPoint();
    TLV8 conf(NULL, 0);
    conf.add(0x01, 0x10);
    conf.add(0x02, 0x10);
    nfcSupportedConfiguration = new Characteristic::NFCAccessSupportedConfiguration(conf);
  }

  boolean update() {
    LOG(D, "PROVISIONED READER KEY: %s", red_log::bufToHexString(readerData.reader_pk.data(), readerData.reader_pk.size()).c_str());
    LOG(D, "READER GROUP IDENTIFIER: %s", red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size()).c_str());
    LOG(D, "READER UNIQUE IDENTIFIER: %s", red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size()).c_str());

    TLV8 ctrlData(NULL, 0);
    nfcControlPoint->getNewTLV(ctrlData);
    std::vector<uint8_t> tlvData(ctrlData.pack_size());
    ctrlData.pack(tlvData.data());
    if (tlvData.size() == 0)
      return false;
    LOG(D, "Decoded data: %s", red_log::bufToHexString(tlvData.data(), tlvData.size()).c_str());
    LOG(D, "Decoded data length: %d", tlvData.size());
    HK_HomeKit hkCtx(readerData, savedData, "READERDATA", tlvData);
    std::vector<uint8_t> result = hkCtx.processResult();
    if (readerData.reader_gid.size() > 0) {
      memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());
      with_crc16(ecpData, 16, ecpData + 16);
    }
    TLV8 res(NULL, 0);
    res.unpack(result.data(), result.size());
    nfcControlPoint->setTLV(res, false);
    return true;
  }

};

void deleteReaderData(const char* buf = "") {
  esp_err_t erase_nvs = nvs_erase_key(savedData, "READERDATA");
  esp_err_t commit_nvs = nvs_commit(savedData);
  readerData.issuers.clear();
  readerData.reader_gid.clear();
  readerData.reader_id.clear();
  readerData.reader_pk.clear();
  readerData.reader_pk_x.clear();
  readerData.reader_sk.clear();
  LOG(D, "*** NVS W STATUS");
  LOG(D, "ERASE: %s", esp_err_to_name(erase_nvs));
  LOG(D, "COMMIT: %s", esp_err_to_name(commit_nvs));
  LOG(D, "*** NVS W STATUS");
}

std::vector<uint8_t> getHashIdentifier(const uint8_t* key, size_t len) {
  const char* TAG = "getHashIdentifier";
  LOG(V, "Key: %s, Length: %d", red_log::bufToHexString(key, len).c_str(), len);
  std::vector<unsigned char> hashable;
  std::string string = "key-identifier";
  hashable.insert(hashable.begin(), string.begin(), string.end());
  hashable.insert(hashable.end(), key, key + len);
  LOG(V, "Hashable: %s", red_log::bufToHexString(&hashable.front(), hashable.size()).c_str());
  uint8_t hash[32];
  mbedtls_sha256(&hashable.front(), hashable.size(), hash, 0);
  LOG(V, "HashIdentifier: %s", red_log::bufToHexString(hash, 8).c_str());
  return std::vector<uint8_t>{hash, hash + 8};
}

void pairCallback() {
  if (HAPClient::nAdminControllers() == 0) {
    deleteReaderData(NULL);
    return;
  }
  for (auto it = homeSpan.controllerListBegin(); it != homeSpan.controllerListEnd(); ++it) {
    std::vector<uint8_t> id = getHashIdentifier(it->getLTPK(), 32);
    LOG(D, "Found allocated controller - Hash: %s", red_log::bufToHexString(id.data(), 8).c_str());
    hkIssuer_t* foundIssuer = nullptr;
    for (auto&& issuer : readerData.issuers) {
      if (std::equal(issuer.issuer_id.begin(), issuer.issuer_id.end(), id.begin())) {
        LOG(D, "Issuer %s already added, skipping", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str());
        foundIssuer = &issuer;
        break;
      }
    }
    if (foundIssuer == nullptr) {
      LOG(D, "Adding new issuer - ID: %s", red_log::bufToHexString(id.data(), 8).c_str());
      hkIssuer_t newIssuer;
      newIssuer.issuer_id = std::vector<uint8_t>{ id.begin(), id.begin() + 8 };
      newIssuer.issuer_pk.insert(newIssuer.issuer_pk.begin(), it->getLTPK(), it->getLTPK() + 32);
      readerData.issuers.emplace_back(newIssuer);
    }
  }
  save_to_nvs();
}

void setFlow(const char* buf) {
  switch (buf[1]) {
  case '0':
    hkFlow = KeyFlow::kFlowFAST;
    LOG(I, "FAST Flow");
    break;

  case '1':
    hkFlow = KeyFlow::kFlowSTANDARD;
    LOG(I, "STANDARD Flow");
    break;
  case '2':
    hkFlow = KeyFlow::kFlowATTESTATION;
    LOG(I, "ATTESTATION Flow");
    break;

  default:
    LOG(I, "0 = FAST flow, 1 = STANDARD Flow, 2 = ATTESTATION Flow");
    break;
  }
}

void setLogLevel(const char* buf) {
  esp_log_level_t level = esp_log_level_get("*");
  if (strncmp(buf + 1, "E", 1) == 0) {
    level = ESP_LOG_ERROR;
    LOG(I, "ERROR");
  } else if (strncmp(buf + 1, "W", 1) == 0) {
    level = ESP_LOG_WARN;
    LOG(I, "WARNING");
  } else if (strncmp(buf + 1, "I", 1) == 0) {
    level = ESP_LOG_INFO;
    LOG(I, "INFO");
  } else if (strncmp(buf + 1, "D", 1) == 0) {
    level = ESP_LOG_DEBUG;
    LOG(I, "DEBUG");
  } else if (strncmp(buf + 1, "V", 1) == 0) {
    level = ESP_LOG_VERBOSE;
    LOG(I, "VERBOSE");
  } else if (strncmp(buf + 1, "N", 1) == 0) {
    level = ESP_LOG_NONE;
    LOG(I, "NONE");
  }

  esp_log_level_set(TAG, level);
  esp_log_level_set("HK_HomeKit", level);
  esp_log_level_set("HKAuthCtx", level);
  esp_log_level_set("HKFastAuth", level);
  esp_log_level_set("HKStdAuth", level);
  esp_log_level_set("HKAttestAuth", level);
  esp_log_level_set("PN532", level);
  esp_log_level_set("PN532_SPI", level);
  esp_log_level_set("ISO18013_SC", level);
  esp_log_level_set("LockMechanism", level);
  esp_log_level_set("NFCAccess", level);
  esp_log_level_set("actions-config", level);
  esp_log_level_set("misc-config", level);
}

void print_issuers(const char* buf) {
  for (auto&& issuer : readerData.issuers) {
    LOG(I, "Issuer ID: %s, Public Key: %s", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str(), red_log::bufToHexString(issuer.issuer_pk.data(), issuer.issuer_pk.size()).c_str());
    for (auto&& endpoint : issuer.endpoints) {
      LOG(I, "Endpoint ID: %s, Public Key: %s", red_log::bufToHexString(endpoint.endpoint_id.data(), endpoint.endpoint_id.size()).c_str(), red_log::bufToHexString(endpoint.endpoint_pk.data(), endpoint.endpoint_pk.size()).c_str());
    }
  }
}

void notFound(AsyncWebServerRequest* request) {
  request->send(404, "text/plain", "Not found");
}

void listDir(fs::FS& fs, const char* dirname, uint8_t levels) {
  LOG(I, "Listing directory: %s\r\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    LOG(I, "- failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    LOG(I, " - not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      LOG(I, "%s", file.name());
      if (levels) {
        listDir(fs, file.name(), levels - 1);
      }
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      LOG(I, "%d", file.size());
    }
    file = root.openNextFile();
  }
}

String indexProcess(const String& var) {
  if (var == "VERSION") {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    std::string app_version = app_desc->version;
    return String(app_version.c_str());
  }
  return "";
}

bool headersFix(AsyncWebServerRequest* request) { request->addInterestingHeader("ANY"); return true; };
void setupWeb() {
  using json = nlohmann::json;
  // Static assets
  auto assetsHandle = new AsyncStaticWebHandler("/assets", LittleFS, "/assets/", NULL);
  assetsHandle->setFilter(headersFix);
  webServer.addHandler(assetsHandle);

  auto routesHandle = new AsyncStaticWebHandler("/fragment", LittleFS, "/routes", NULL);
  routesHandle->setFilter(headersFix);
  webServer.addHandler(routesHandle);

  // ----------------------------
  // GET /config?type=misc|hkinfo
  // ----------------------------
  AsyncCallbackWebHandler* dataProvision = new AsyncCallbackWebHandler();
  webServer.addHandler(dataProvision);
  dataProvision->setUri("/config");
  dataProvision->setMethod(HTTP_GET);
  dataProvision->onRequest([](AsyncWebServerRequest* req) {
    if (!req->hasParam("type")) {
      req->send(400, "text/plain", "Missing ?type=");
      return;
    }

    json serializedData;
    AsyncWebParameter* p = req->getParam("type");
    const String type = p->value();

    if (type == "misc") {
      serializedData = espConfig::miscConfig;

    } else if (type == "hkinfo") {
      json inputData = readerData;

      // Provide hex strings for convenience
      if (inputData.contains("group_identifier")) {
        serializedData["group_identifier"] =
          red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size(), true);
      }
      if (inputData.contains("unique_identifier")) {
        serializedData["unique_identifier"] =
          red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size(), true);
      }

      // Issuers/endpoints (IDs only, hex)
      if (inputData.contains("issuers")) {
        serializedData["issuers"] = json::array();
        for (auto it = inputData.at("issuers").begin(); it != inputData.at("issuers").end(); ++it) {
          json issuer;
          if (it.value().contains("issuerId")) {
            std::vector<uint8_t> id = it.value().at("issuerId").get<std::vector<uint8_t>>();
            issuer["issuerId"] = red_log::bufToHexString(id.data(), id.size(), true);
          }
          if (it.value().contains("endpoints") && it.value().at("endpoints").size() > 0) {
            issuer["endpoints"] = json::array();
            for (auto it2 = it.value().at("endpoints").begin(); it2 != it.value().at("endpoints").end(); ++it2) {
              json ep;
              if (it2.value().contains("endpointId")) {
                std::vector<uint8_t> eid = it2.value().at("endpointId").get<std::vector<uint8_t>>();
                ep["endpointId"] = red_log::bufToHexString(eid.data(), eid.size(), true);
              }
              issuer["endpoints"].push_back(ep);
            }
          }
          serializedData["issuers"].push_back(issuer);
        }
      }

    } else {
      req->send(400, "text/plain", "Invalid type (use misc or hkinfo)");
      return;
    }

    req->send(200, "application/json", serializedData.dump().c_str());
  });

  // -------------------------
  // POST /config/clear?type=misc
  // -------------------------
  AsyncCallbackWebHandler* dataClear = new AsyncCallbackWebHandler();
  webServer.addHandler(dataClear);
  dataClear->setUri("/config/clear");
  dataClear->setMethod(HTTP_POST);
  dataClear->onRequest([](AsyncWebServerRequest* req) {
    if (!req->hasParam("type")) {
      req->send(400, "text/plain", "Missing ?type=");
      return;
    }
    AsyncWebParameter* p = req->getParam("type");
    if (p->value() != "misc") {
      req->send(400, "text/plain", "Only type=misc supported");
      return;
    }

    nvs_erase_key(savedData, "MISCDATA");
    nvs_commit(savedData);
    espConfig::miscConfig = {};
    req->send(200, "text/plain", "200 Success");
  });

  // -------------------------
  // POST /config/save?type=misc
  // -------------------------
  AsyncCallbackWebHandler* dataSave = new AsyncCallbackWebHandler();
  webServer.addHandler(dataSave);
  dataSave->setUri("/config/save");
  dataSave->setMethod(HTTP_POST);

  dataSave->onBody([](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    json* dataJson = new json(json::parse(data, data + len));
    if (!dataJson->is_discarded()) {
      request->_tempObject = dataJson;
    } else {
      delete dataJson;
    }
  });

  dataSave->onRequest([](AsyncWebServerRequest* req) {
    json* incoming = static_cast<json*>(req->_tempObject);

    if (!req->hasParam("type") || !incoming) {
      req->send(400, "text/plain", "Missing ?type= or invalid JSON");
      return;
    }

    AsyncWebParameter* p = req->getParam("type");
    if (p->value() != "misc") {
      req->send(400, "text/plain", "Only type=misc supported");
      return;
    }

    json configData = espConfig::miscConfig;

    // Validate & apply only known keys
    uint8_t propertiesProcessed = 0;

    for (auto it = incoming->begin(); it != incoming->end(); ++it) {
      if (!configData.contains(it.key())) {
        req->send(400, "text/plain", ("Unknown key: " + it.key()).c_str());
        return;
      }

      // setupCode validation
      if (it.key() == "setupCode") {
        if (!it.value().is_string()) {
          req->send(400, "text/plain", "setupCode must be string");
          return;
        }
        std::string code = it.value().get<std::string>();
        bool allDigits = !code.empty() && std::find_if(code.begin(), code.end(),
                        [](unsigned char c){ return !std::isdigit(c); }) == code.end();
        if (!allDigits || code.length() != 8) {
          req->send(400, "text/plain", "setupCode must be exactly 8 digits");
          return;
        }
        // Only allow setup code change if no controllers paired
        if (homeSpan.controllerListBegin() != homeSpan.controllerListEnd() &&
            code != configData.at("setupCode").get<std::string>()) {
          req->send(400, "text/plain", "Setup Code can only be changed when no devices are paired");
          return;
        }
      }

      // Any "*Pin" validation (0..255)
      if (it.key().size() >= 3 && it.key().rfind("Pin") == it.key().size() - 3) {
        if (!it.value().is_number()) {
          req->send(400, "text/plain", ("Pin must be number: " + it.key()).c_str());
          return;
        }
        int v = it.value().get<int>();
        if (v < 0 || v > 255) {
          req->send(400, "text/plain", ("Pin out of range: " + it.key()).c_str());
          return;
        }
        if (v != 255 && (!GPIO_IS_VALID_GPIO(v) && !GPIO_IS_VALID_OUTPUT_GPIO(v))) {
          req->send(400, "text/plain", ("Invalid GPIO for: " + it.key()).c_str());
          return;
        }
      }

      // Accept booleans + numbers for booleans (like your old UI)
      if (configData.at(it.key()).is_boolean() && it.value().is_number()) {
        it.value() = static_cast<bool>(it.value().get<uint8_t>());
      } else if (configData.at(it.key()).is_boolean() && !it.value().is_boolean()) {
        req->send(400, "text/plain", ("Expected boolean for: " + it.key()).c_str());
        return;
      }

      // Type compatibility (relaxed for boolean above)
      if (configData.at(it.key()).type() != it.value().type() && !configData.at(it.key()).is_boolean()) {
        req->send(400, "text/plain", ("Type mismatch for: " + it.key()).c_str());
        return;
      }

      propertiesProcessed++;
    }

    if (propertiesProcessed != incoming->size()) {
      req->send(500, "text/plain", "Not all properties validated");
      return;
    }

    // Apply updates
    for (auto it = incoming->begin(); it != incoming->end(); ++it) {
      // setupCode applies to HomeSpan when unpaired
      if (it.key() == "setupCode") {
        std::string code = it.value().get<std::string>();
        if (homeSpan.controllerListBegin() == homeSpan.controllerListEnd()) {
          homeSpan.setPairingCode(code.c_str());
        }
      }

      // gpioActionPin enable/disable task logic remains elsewhere in your code;
      // simplest approach: save + reboot after misc changes.
      configData.at(it.key()) = it.value();
    }

    // Persist MISCDATA
    std::vector<uint8_t> vectorData = json::to_msgpack(configData);
    esp_err_t set_nvs = nvs_set_blob(savedData, "MISCDATA", vectorData.data(), vectorData.size());
    esp_err_t commit_nvs = nvs_commit(savedData);

    LOG(D, "SET_STATUS: %s", esp_err_to_name(set_nvs));
    LOG(D, "COMMIT_STATUS: %s", esp_err_to_name(commit_nvs));

    if (set_nvs == ESP_OK && commit_nvs == ESP_OK) {
      configData.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
      req->send(200, "text/plain", "Saved! Restarting...");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      ESP.restart();
    } else {
      req->send(500, "text/plain", "Failed to save config");
    }
  });

  // Misc endpoints
  auto rebootDeviceHandle = new AsyncCallbackWebHandler();
  rebootDeviceHandle->setUri("/reboot_device");
  rebootDeviceHandle->setMethod(HTTP_GET);
  rebootDeviceHandle->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Rebooting the device...");
    delay(1000);
    ESP.restart();
  });
  webServer.addHandler(rebootDeviceHandle);

  auto startConfigAP = new AsyncCallbackWebHandler();
  startConfigAP->setUri("/start_config_ap");
  startConfigAP->setMethod(HTTP_GET);
  startConfigAP->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Starting the AP...");
    delay(1000);
    webServer.end();
    homeSpan.processSerialCommand("A");
  });
  webServer.addHandler(startConfigAP);

  auto resetHkHandle = new AsyncCallbackWebHandler();
  resetHkHandle->setUri("/reset_hk_pair");
  resetHkHandle->setMethod(HTTP_GET);
  resetHkHandle->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Erasing HomeKit pairings and restarting...");
    delay(1000);
    deleteReaderData();
    homeSpan.processSerialCommand("H");
  });
  webServer.addHandler(resetHkHandle);

  auto resetWifiHandle = new AsyncCallbackWebHandler();
  resetWifiHandle->setUri("/reset_wifi_cred");
  resetWifiHandle->setMethod(HTTP_GET);
  resetWifiHandle->onRequest([](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "Erasing WiFi credentials and restarting...");
    delay(1000);
    homeSpan.processSerialCommand("X");
  });
  webServer.addHandler(resetWifiHandle);

  auto getWifiRssi = new AsyncCallbackWebHandler();
  getWifiRssi->setUri("/get_wifi_rssi");
  getWifiRssi->setMethod(HTTP_GET);
  getWifiRssi->onRequest([](AsyncWebServerRequest* request) {
    std::string rssi_val = std::to_string(WiFi.RSSI());
    request->send(200, "text/plain", rssi_val.c_str());
  });
  webServer.addHandler(getWifiRssi);

  // Root
  AsyncCallbackWebHandler* rootHandle = new AsyncCallbackWebHandler();
  webServer.addHandler(rootHandle);
  rootHandle->setUri("/");
  rootHandle->setMethod(HTTP_GET);
  rootHandle->onRequest([](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html", false, indexProcess);
  });

  AsyncCallbackWebHandler* hashPage = new AsyncCallbackWebHandler();
  webServer.addHandler(hashPage);
  hashPage->setUri("/#*");
  hashPage->setMethod(HTTP_GET);
  hashPage->onRequest([](AsyncWebServerRequest* req) {
    req->send(LittleFS, "/index.html", "text/html", false, indexProcess);
  });

  // Optional basic auth
  if (espConfig::miscConfig.webAuthEnabled) {
    LOG(I, "Web Authentication Enabled");
    routesHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    dataProvision->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    dataSave->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    dataClear->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    rootHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    hashPage->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    resetHkHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    resetWifiHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    getWifiRssi->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    startConfigAP->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
    rebootDeviceHandle->setAuthentication(espConfig::miscConfig.webUsername.c_str(), espConfig::miscConfig.webPassword.c_str());
  }

  webServer.onNotFound(notFound);
  webServer.begin();
}

void wifiCallback(int status) {
  if (status == 1) {
    setupWeb();
  }
}


std::string hex_representation(const std::vector<uint8_t>& v) {
  std::string hex_tmp;
  for (auto x : v) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (unsigned)x;
    hex_tmp += oss.str();
  }
  return hex_tmp;
}

void nfc_retry(void* arg) {
  ESP_LOGI(TAG, "Starting reconnecting PN532");
  while (1) {
    nfc->begin();
    uint32_t versiondata = nfc->getFirmwareVersion();
    if (!versiondata) {
      ESP_LOGE("NFC_SETUP", "Error establishing PN532 connection");
    } else {
      unsigned int model = (versiondata >> 24) & 0xFF;
      ESP_LOGI("NFC_SETUP", "Found chip PN5%x", model);
      int maj = (versiondata >> 16) & 0xFF;
      int min = (versiondata >> 8) & 0xFF;
      ESP_LOGI("NFC_SETUP", "Firmware ver. %d.%d", maj, min);
      nfc->SAMConfig();
      nfc->setRFField(0x02, 0x01);
      nfc->setPassiveActivationRetries(0);
      ESP_LOGI("NFC_SETUP", "Waiting for an ISO14443A card");
      vTaskResume(nfc_poll_task);
      vTaskDelete(NULL);
      return;
    }
    nfc->stop();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// Function to add a UID
void addUID(const std::string& uid) {
  // Initialize NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("my-app", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
      ESP_LOGE("NVS", "Error opening NVS handle!");
      return;
  }

  // Get the current number of UIDs stored
  int32_t numUIDs = 0;  // Use int32_t instead of int
  err = nvs_get_i32(nvs_handle, "numUIDs", &numUIDs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
      numUIDs = 0; // No previous UIDs found, start at 0
  } else if (err != ESP_OK) {
      ESP_LOGE("NVS", "Failed to read numUIDs from NVS");
      nvs_close(nvs_handle);
      return;
  }

  // Save the new UID with a unique key
  std::string key = "uid_" + std::to_string(numUIDs);
  err = nvs_set_str(nvs_handle, key.c_str(), uid.c_str());
  if (err != ESP_OK) {
      ESP_LOGE("NVS", "Failed to write UID to NVS");
      nvs_close(nvs_handle);
      return;
  }

  // Increment and save the total number of UIDs
  numUIDs++;
  err = nvs_set_i32(nvs_handle, "numUIDs", numUIDs);
  if (err != ESP_OK) {
      ESP_LOGE("NVS", "Failed to write numUIDs to NVS");
  }

  // Commit changes and close NVS
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
      ESP_LOGE("NVS", "Failed to commit NVS changes");
  }

  nvs_close(nvs_handle);
}

// Function to check if a UID is already stored
bool isUIDStored(const std::string& uid) {
  // Initialize NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("my-app", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
      ESP_LOGE("NVS", "Error opening NVS handle!");
      return false;
  }

  // Get the total number of UIDs stored
  int32_t numUIDs = 0;  // Use int32_t instead of int
  err = nvs_get_i32(nvs_handle, "numUIDs", &numUIDs);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
      numUIDs = 0; // No UIDs stored
  } else if (err != ESP_OK) {
      ESP_LOGE("NVS", "Failed to read numUIDs from NVS");
      nvs_close(nvs_handle);
      return false;
  }

  // Iterate through all stored UIDs to check for a match
  for (int32_t i = 0; i < numUIDs; i++) {
      std::string key = "uid_" + std::to_string(i);
      size_t len = 0;

      // Get the length of the stored UID
      err = nvs_get_str(nvs_handle, key.c_str(), nullptr, &len);
      if (err != ESP_OK) {
          ESP_LOGE("NVS", "Failed to get length of stored UID");
          continue;
      }

      // Retrieve the stored UID
      char* storedUID = new char[len + 1];
      err = nvs_get_str(nvs_handle, key.c_str(), storedUID, &len);
      if (err != ESP_OK) {
          ESP_LOGE("NVS", "Failed to read stored UID");
          delete[] storedUID;
          continue;
      }

      // Compare the stored UID with the provided one
      if (uid == std::string(storedUID)) {
          delete[] storedUID;
          nvs_close(nvs_handle);
          return true; // UID found
      }

      delete[] storedUID;
  }

  nvs_close(nvs_handle);
  return false; // UID not found
}

static bool espnow_init_once() {
  if (espnow_initialized) return true;

  // IMPORTANT: do NOT WiFi.disconnect() here.
  // ESP-NOW can coexist with STA WiFi; HomeSpan uses WiFi.
  WiFi.mode(WIFI_STA);
  delay(50);

  if (esp_now_init() != ESP_OK) {
    ESP_LOGE("ESPNOW", "esp_now_init failed");
    return false;
  }

  esp_now_register_send_cb([](const uint8_t *mac_addr, esp_now_send_status_t status) {
    ESP_LOGI("ESPNOW", "Send Status: %s", status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
  });

  // Register peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, ESP_NOW_ETH_ALEN);
  peerInfo.channel = 0;     // 0 = use current channel (works best when WiFi STA connected)
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(receiverMAC)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      ESP_LOGE("ESPNOW", "Failed to add peer");
      return false;
    }
  }

  espnow_initialized = true;
  ESP_LOGI("ESPNOW", "Initialized");
  return true;
}

static void espnow_task_entry(void *arg) {
  ESP_LOGI("ESPNOW", "Task started");

  // init with retries
  while (!espnow_init_once()) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  espnow_msg_t msg{};
  while (true) {
    if (xQueueReceive(espnow_queue, &msg, portMAX_DELAY) == pdTRUE) {
      esp_err_t r = esp_now_send(
        receiverMAC,
        reinterpret_cast<const uint8_t*>(msg.uid),
        msg.len
      );

      if (r != ESP_OK) {
        ESP_LOGW("ESPNOW", "esp_now_send failed: %s (%d)", esp_err_to_name(r), (int)r);
      } else {
        ESP_LOGI("ESPNOW", "Queued UID sent: %s", msg.uid);
      }
    }
  }
}

void nfc_thread_entry(void* arg) {

  uint32_t versiondata = nfc->getFirmwareVersion();
  if (!versiondata) {
    ESP_LOGE("NFC_SETUP", "Error establishing PN532 connection");
    nfc->stop();
    xTaskCreatePinnedToCore(nfc_retry, "nfc_reconnect_task", 8192, NULL, 1, &nfc_reconnect_task, 0);
    vTaskSuspend(NULL);
  } else {
    unsigned int model = (versiondata >> 24) & 0xFF;
    ESP_LOGI("NFC_SETUP", "Found chip PN5%x", model);
    int maj = (versiondata >> 16) & 0xFF;
    int min = (versiondata >> 8) & 0xFF;
    ESP_LOGI("NFC_SETUP", "Firmware ver. %d.%d", maj, min);
    nfc->SAMConfig();
    nfc->setRFField(0x02, 0x01);
    nfc->setPassiveActivationRetries(0);
    ESP_LOGI("NFC_SETUP", "Waiting for an ISO14443A card");
  }

  // Refresh ECP with GID + CRC
  if (readerData.reader_gid.size() > 0) {
    memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());
    with_crc16(ecpData, 16, ecpData + 16);
  }

  while (1) {

    // Keep PN532 stable
    uint8_t res[4];
    uint16_t resLen = 4;

    bool writeStatus = nfc->writeRegister(0x633d, 0, true);
    if (!writeStatus) {
      LOG(W, "writeRegister failed, reconnecting PN532");
      nfc->stop();
      xTaskCreatePinnedToCore(nfc_retry, "nfc_reconnect_task", 8192, NULL, 1, &nfc_reconnect_task, 0);
      vTaskSuspend(NULL);
    }

    nfc->inCommunicateThru(ecpData, sizeof(ecpData), res, &resLen, 100, true);

    uint8_t uid[16];
    uint8_t uidLen = 0;
    uint8_t atqa[2];
    uint8_t sak[1];

    bool passiveTarget = nfc->readPassiveTargetID(
      PN532_MIFARE_ISO14443A, uid, &uidLen, atqa, sak, 500, true, true
    );

    if (passiveTarget) {

      nfc->setPassiveActivationRetries(5);

      std::string detectedUID = red_log::bufToHexString(uid, uidLen);

      // ESP-NOW broadcast of actual UID (fixes earlier bug)
      if (espnow_queue) {
        espnow_msg_t m{};
        // copy UID safely
        size_t n = detectedUID.size();
        if (n >= sizeof(m.uid)) n = sizeof(m.uid) - 1;
        memcpy(m.uid, detectedUID.c_str(), n);
        m.uid[n] = '\0';
        m.len = (uint8_t)(strlen(m.uid) + 1);

        // non-blocking enqueue (drop if full)
        if (xQueueSend(espnow_queue, &m, 0) != pdTRUE) {
          ESP_LOGW("ESPNOW", "Queue full, dropping UID: %s", m.uid);
        }
      }

      ESP_LOGI("NFC_TAG", "Detected UID: %s", detectedUID.c_str());

      // Optional UID whitelist unlock (non-HomeKey cards)
      if (isUIDStored(detectedUID)) {
        ESP_LOGI("NFC", "UID found (whitelist) -> toggling lock");
        const gpioLockAction action{ .source = gpioLockAction::HOMEKEY, .action = 0 };
        xQueueSend(gpio_lock_handle, &action, 0);
        delay(1000);
        xQueueSend(gpio_lock_handle, &action, 0);
      }

      LOG(I, "*** PASSIVE TARGET DETECTED ***");
      auto startTime = std::chrono::high_resolution_clock::now();

      // SELECT HomeKey Applet
      uint8_t selectHomeKey[] = {
        0x00, 0xA4, 0x04, 0x00, 0x07,
        0xA0, 0x00, 0x00, 0x08, 0x58, 0x01, 0x01, 0x00
      };

      uint8_t selectCmdRes[32];
      uint16_t selectCmdResLength = sizeof(selectCmdRes);

      LOG(I, "Selecting HomeKey applet...");
      bool status = nfc->inDataExchange(selectHomeKey, sizeof(selectHomeKey), selectCmdRes, &selectCmdResLength);

      if (status && selectCmdResLength >= 2 &&
          selectCmdRes[selectCmdResLength - 2] == 0x90 &&
          selectCmdRes[selectCmdResLength - 1] == 0x00) {

        LOG(I, "*** HOMEKEY APPLET SELECT OK ***");

        HKAuthenticationContext authCtx(
          [](uint8_t* s, uint8_t l, uint8_t* r, uint16_t* rl, bool il) -> bool {
            return nfc->inDataExchange(s, l, r, rl, il);
          },
          readerData, savedData
        );

        auto authResult = authCtx.authenticate(hkFlow);

        if (std::get<2>(authResult) != kFlowFailed) {

          // HomeKey success -> trigger lock (local only)
          if ((espConfig::miscConfig.gpioActionPin != 255 && espConfig::miscConfig.hkGpioControlledState) ||
              espConfig::miscConfig.hkDumbSwitchMode) {
            const gpioLockAction action{ .source = gpioLockAction::HOMEKEY, .action = 0 };
            xQueueSend(gpio_lock_handle, &action, 0);
          }

          if (espConfig::miscConfig.lockAlwaysUnlock) {
            if (espConfig::miscConfig.gpioActionPin == 255 || !espConfig::miscConfig.hkGpioControlledState) {
              lockCurrentState->setVal(lockStates::UNLOCKED);
              lockTargetState->setVal(lockStates::UNLOCKED);
            }
          } else if (espConfig::miscConfig.lockAlwaysLock) {
            if (espConfig::miscConfig.gpioActionPin == 255 || espConfig::miscConfig.hkGpioControlledState) {
              lockCurrentState->setVal(lockStates::LOCKED);
              lockTargetState->setVal(lockStates::LOCKED);
            }
          }

          auto stopTime = std::chrono::high_resolution_clock::now();
          LOG(I, "Total Time (detection->auth->gpio): %lli ms",
              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(stopTime - startTime).count());

        } else {
          LOG(W, "HomeKey auth failed (FlowFailed)");
        }

        nfc->setRFField(0x02, 0x01);

      } else {
        LOG(W, "Not a HomeKey tag (SELECT failed) UID=%s", detectedUID.c_str());
      }

      vTaskDelay(50 / portTICK_PERIOD_MS);
      nfc->inRelease();

      // Wait until tag leaves field (anti-repeat)
      int counter = 50;
      bool deviceStillInField = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
      while (deviceStillInField && counter-- > 0) {
        vTaskDelay(50 / portTICK_PERIOD_MS);
        nfc->inRelease();
        deviceStillInField = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen);
      }

      nfc->inRelease();
      nfc->setPassiveActivationRetries(0);
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }

  vTaskDelete(NULL);
}


static inline void io_init() {
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BACKLIGHT, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);

  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_BACKLIGHT, LOW);
  digitalWrite(PIN_GREEN, LOW);
}

static inline void beep_short() {
  digitalWrite(PIN_BUZZER, HIGH);
  vTaskDelay(pdMS_TO_TICKS(50));
  digitalWrite(PIN_BUZZER, LOW);
}

static inline void beep_long() {
  digitalWrite(PIN_BUZZER, HIGH);
  vTaskDelay(pdMS_TO_TICKS(500));
  digitalWrite(PIN_BUZZER, LOW);
}


static bool read_pin_code(MPR121_t *dev, char *out6, TickType_t timeout_ticks) {
  int count = 0;
  TickType_t start = xTaskGetTickCount();

  while (count < 6) {
    if ((xTaskGetTickCount() - start) > timeout_ticks) {
      out6[0] = '\0';
      return false; // timeout
    }

    MPR121_updateAll(dev);

    for (int ele = 0; ele < 12; ele++) {
      if (MPR121_isNewTouch(dev, ele)) {
        // ignore special keys while entering digits
        if (touchPinMap[ele] == ELE_CANCEL || touchPinMap[ele] == ELE_ENTER) {
          continue;
        }

        beep_short();

        int digit = touchPinMap[ele];
        ESP_LOGI("KEYPAD", "Pin %d touched", digit);
        if (digit >= 0 && digit <= 9) {
          out6[count++] = char('0' + digit);
        }

        // small debounce
        vTaskDelay(pdMS_TO_TICKS(200));
        break;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }

  out6[6] = '\0';
  return true;
}

static bool check_pin_code(const char *pin, int mode) {
  if (!pin || pin[0] == '\0') return false;

  if (strcmp(pin, correctPIN) == 0 || strcmp(pin, masterPIN) == 0) {
    // success
    if (mode == 0) {
      digitalWrite(PIN_GREEN, HIGH);
      for (int i = 0; i < 2; i++) { beep_short(); vTaskDelay(pdMS_TO_TICKS(50)); }
      digitalWrite(PIN_GREEN, LOW);
    } else {
      beep_short();
    }
    return true;
  }

  // fail
  digitalWrite(PIN_RED, HIGH);
  beep_long();
  digitalWrite(PIN_RED, LOW);
  return false;
}

static void touch_keypad(MPR121_t *dev) {

  // Always refresh touch state
  MPR121_updateAll(dev);

  for (int ele = 0; ele < 12; ele++) {

    if (!MPR121_isNewTouch(dev, ele)) continue;

    // CANCEL key
    if (touchPinMap[ele] == ELE_CANCEL) {
      beep_long();
      return;
    }

    // ENTER key: change PIN flow (requires existing PIN first)
    if (touchPinMap[ele] == ELE_ENTER) {
      digitalWrite(PIN_RED, HIGH);
      digitalWrite(PIN_GREEN, HIGH);

      char pin[7];
      if (!read_pin_code(dev, pin, pdMS_TO_TICKS(15000))) {
        digitalWrite(PIN_RED, LOW);
        digitalWrite(PIN_GREEN, LOW);
        return;
      }

      if (check_pin_code(pin, 1)) {
        // read new pin twice
        char new_pin[7], re_pin[7];
        if (read_pin_code(dev, new_pin, pdMS_TO_TICKS(15000)) &&
            read_pin_code(dev, re_pin,  pdMS_TO_TICKS(15000)) &&
            strcmp(new_pin, re_pin) == 0) {

          strncpy(correctPIN, new_pin, sizeof(correctPIN));
          correctPIN[sizeof(correctPIN) - 1] = '\0';
          beep_short();
          beep_short();
        } else {
          beep_long();
        }
      }

      digitalWrite(PIN_RED, LOW);
      digitalWrite(PIN_GREEN, LOW);
      return;
    }

    // Any normal key: turn on backlight, ask for PIN, validate
    digitalWrite(PIN_BACKLIGHT, HIGH);
    beep_short();

    char pin[7];
    if (read_pin_code(dev, pin, pdMS_TO_TICKS(15000))) {
      if (check_pin_code(pin, 0)) {
        // TODO: call your ESP-NOW send / unlock action here
        // sendUIDViaESPNow(...);
      }
    } else {
      beep_long(); // timeout
    }

    digitalWrite(PIN_BACKLIGHT, LOW);
    return;
  }
}

static void mpr121_task_entry(void *arg)
{
    const char* TAG = "MPR121";

    ESP_LOGI(TAG, "CONFIG_I2C_ADDRESS=0x%X", CONFIG_I2C_ADDRESS);
    ESP_LOGI(TAG, "CONFIG_SCL_GPIO=%d", CONFIG_SCL_GPIO);
    ESP_LOGI(TAG, "CONFIG_SDA_GPIO=%d", CONFIG_SDA_GPIO);
    ESP_LOGI(TAG, "CONFIG_IRQ_GPIO=%d", CONFIG_IRQ_GPIO);
    ESP_LOGI(TAG, "MPR121 Task Started");

    MPR121_t dev;

    uint16_t touchThreshold =  9;
    uint16_t releaseThreshold = 4;

    MPR121_type(&dev);

    bool ret = MPR121_begin(&dev, CONFIG_I2C_ADDRESS,
                            touchThreshold, releaseThreshold,
                            CONFIG_IRQ_GPIO, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO);

    ESP_LOGI(TAG, "MPR121_begin=%d", ret);
    if (!ret) {
        ESP_LOGE(TAG, "MPR121 init failed, err=%d", (int)MPR121_getError(&dev));
        vTaskDelete(NULL);
    }

    MPR121_setFFI(&dev, FFI_10);
    MPR121_setSFI(&dev, SFI_10);
    MPR121_setGlobalCDT(&dev, CDT_4US);
    MPR121_autoSetElectrodesDefault(&dev, true);
    MPR121_updateTouchData(&dev);

    while (1) {
        touch_keypad(&dev);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  const esp_app_desc_t* app_desc = esp_app_get_description();
  std::string app_version = app_desc->version;
  gpio_lock_handle = xQueueCreate(2, sizeof(gpioLockAction));
  espnow_queue = xQueueCreate(10, sizeof(espnow_msg_t));

  size_t len;
  const char* TAG = "SETUP";
  nvs_open("SAVED_DATA", NVS_READWRITE, &savedData);
  if (!nvs_get_blob(savedData, "READERDATA", NULL, &len)) {
    std::vector<uint8_t> savedBuf(len);
    nvs_get_blob(savedData, "READERDATA", savedBuf.data(), &len);
    LOG(D, "NVS READERDATA LENGTH: %d", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, savedBuf.data(), savedBuf.size(), ESP_LOG_VERBOSE);
    nlohmann::json data = nlohmann::json::from_msgpack(savedBuf);
    if (!data.is_discarded()) {
      data.get_to<readerData_t>(readerData);
      LOG(I, "Reader Data loaded from NVS");
    }
  }
  if (!nvs_get_blob(savedData, "MISCDATA", NULL, &len)) {
    std::vector<uint8_t> dataBuf(len);
    nvs_get_blob(savedData, "MISCDATA", dataBuf.data(), &len);
    std::string str(dataBuf.begin(), dataBuf.end());
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, dataBuf.data(), dataBuf.size(), ESP_LOG_VERBOSE);
    auto isValidJson = nlohmann::json::accept(dataBuf);
    if (isValidJson) {
      nlohmann::json data = nlohmann::json::parse(str);
      if (!data.is_discarded()) {
        data.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
        LOG(I, "Misc Config loaded from NVS");
      }
    } else {
      nlohmann::json data = nlohmann::json::from_msgpack(dataBuf);
      if (!data.is_discarded()) {
        data.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
        LOG(I, "Misc Config loaded from NVS");
      }
    }
  }
  pn532spi = new PN532_SPI(espConfig::miscConfig.nfcGpioPins[0], espConfig::miscConfig.nfcGpioPins[1], espConfig::miscConfig.nfcGpioPins[2], espConfig::miscConfig.nfcGpioPins[3]);
  nfc = new PN532(*pn532spi);
  nfc->begin();
  io_init();

  if (espConfig::miscConfig.gpioActionPin && espConfig::miscConfig.gpioActionPin != 255) {
    pinMode(espConfig::miscConfig.gpioActionPin, OUTPUT);
  }
  if (!LittleFS.begin(true)) {
    LOG(I, "An Error has occurred while mounting LITTLEFS");
    return;
  }
  listDir(LittleFS, "/", 0);
  LOG(I, "LittleFS used space: %d / %d", LittleFS.usedBytes(), LittleFS.totalBytes());
  if (espConfig::miscConfig.controlPin != 255) {
    homeSpan.setControlPin(espConfig::miscConfig.controlPin);
  }
  if (espConfig::miscConfig.hsStatusPin != 255) {
    homeSpan.setStatusPin(espConfig::miscConfig.hsStatusPin);
  }

  homeSpan.setStatusAutoOff(15);
  homeSpan.setLogLevel(0);
  homeSpan.setSketchVersion(app_version.c_str());

  LOG(I, "READER GROUP ID (%d): %s", readerData.reader_gid.size(), red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size()).c_str());
  LOG(I, "READER UNIQUE ID (%d): %s", readerData.reader_id.size(), red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size()).c_str());

  LOG(I, "HOMEKEY ISSUERS: %d", readerData.issuers.size());
  for (auto&& issuer : readerData.issuers) {
    LOG(D, "Issuer ID: %s, Public Key: %s", red_log::bufToHexString(issuer.issuer_id.data(), issuer.issuer_id.size()).c_str(), red_log::bufToHexString(issuer.issuer_pk.data(), issuer.issuer_pk.size()).c_str());
  }
  homeSpan.enableAutoStartAP();
  //homeSpan.enableOTA(espConfig::miscConfig.otaPasswd.c_str());
  homeSpan.setPortNum(1201);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  char macStr[9] = { 0 };
  sprintf(macStr, "%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3]);
  homeSpan.setHostNameSuffix(macStr);
  homeSpan.begin(Category::Locks, espConfig::miscConfig.deviceName.c_str(), "HK-", "HomeKey-ESP32");

  new SpanUserCommand('D', "Delete Home Key Data", deleteReaderData);
  new SpanUserCommand('L', "Set Log Level", setLogLevel);
  new SpanUserCommand('F', "Set HomeKey Flow", setFlow);
  new SpanUserCommand('P', "Print Issuers", print_issuers);
  new SpanUserCommand('R', "Remove Endpoints", [](const char*) {
    for (auto&& issuer : readerData.issuers) {
      issuer.endpoints.clear();
    }
    save_to_nvs();
    });
  new SpanUserCommand('N', "Btr status low", [](const char* arg) {
    const char* TAG = "BTR_LOW";
    if (strncmp(arg + 1, "0", 1) == 0) {
      statusLowBtr->setVal(0);
      LOG(I, "Low status set to NORMAL");
    } else if (strncmp(arg + 1, "1", 1) == 0) {
      statusLowBtr->setVal(1);
      LOG(I, "Low status set to LOW");
    }
  });
  new SpanUserCommand('B', "Btr level", [](const char* arg) {
    uint8_t level = atoi(static_cast<const char *>(arg + 1));
    btrLevel->setVal(level);
  });
  new SpanAccessory();
  new NFCAccessoryInformation();
  new Service::HAPProtocolInformation();
  new Characteristic::Version();
  new LockManagement();
  new LockMechanism();
  new NFCAccess();
  if (espConfig::miscConfig.proxBatEnabled) {
    new PhysicalLockBattery();
  }
  homeSpan.setControllerCallback(pairCallback);
  homeSpan.setConnectionCallback(wifiCallback);
  
  if (espConfig::miscConfig.gpioActionPin != 255 || espConfig::miscConfig.hkDumbSwitchMode) {
    xTaskCreatePinnedToCore(gpio_task, "gpio_task", 4096, NULL, 2, &gpio_lock_task_handle, 0);
  }
  //xTaskCreatePinnedToCore(espnow_task_entry, "espnow_task", 4096, NULL, 4, &espnow_task_handle, 0);
  xTaskCreatePinnedToCore(nfc_thread_entry, "nfc_task", 8192, NULL, 1, &nfc_poll_task, 0);

  xTaskCreatePinnedToCore(mpr121_task_entry, "mpr121_task", 4096, NULL, 5, &mpr121_task_handle, 1);
 }

//////////////////////////////////////

void loop() {
  homeSpan.poll();
  vTaskDelay(5);
}