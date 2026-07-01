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
#include "HK_HomeKit.h"
#include "config.h"
#include "esp_app_desc.h"
#include "pins_arduino.h"
#include "NFC_SERV_CHARS.h"
#include <mbedtls/sha256.h>
#include <esp_mac.h>
#include <esp_now.h>

#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <vector>
#include <string>
#include <cstdlib>

#include "cJSON.h"
#include <mbedtls/base64.h>
#include "mbedtls/sha256.h"

#include <sys/stat.h>
#include <dirent.h>

#include "esp_spiffs.h"
#include "esp_vfs.h"

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include "esp_timer.h"
#include <time.h>

#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"

extern "C" {
#include "mpr121.h"
#include "nvs_config.h"
#include "api_client.h"
#include "ws_client.h"
#include "ota_client.h"
#include "task_control.h"
#include "monitor.h"
#include "ws_log_upload.h"
#include "evtlog.h"
}

#define BACKEND_START_BIT   (1 << 0)
#define BACKEND_STOP_BIT    (1 << 1)

EventGroupHandle_t g_backend_ev = NULL;
static bool g_backend_running = false;

static TaskHandle_t g_ws_keepalive_task = NULL;
static volatile int g_live_miss_count = 0;
static const int LIVE_MISS_LIMIT = 3;
static volatile bool g_waiting_for_live = false;

volatile bool store_new_card = false;

std::vector<std::string> listUIDs();
bool deleteUID(const std::string& uid);
bool cmd_get_kv(const char *cmd, const char *key, char *out, size_t out_len);
bool deactivateUID(const std::string& uid);
bool activateUID(const std::string& uid);
bool isUIDDeactivated(const std::string& uid);

static bool s_live_serial_enabled = false;
static esp_timer_handle_t s_live_serial_timer = NULL;

const char* TAG = "MAIN";

static const char *TAG_MAIN = "BACKEND";
static const char *TAG_NET  = "NET";
static const char *TAG_WS   = "WS";
static const char *TAG_NVS = "NVS_UID";
static const char *TAG_DEACT = "NVS_DEACT";

static bool g_backend_started = false;
static bool g_login_inflight = false;

static void request_relogin(void);

void printWifiChannel(const char *tag) {
  wifi_second_chan_t second;
  uint8_t primary = 0;

  esp_err_t err = esp_wifi_get_channel(&primary, &second);
  if (err == ESP_OK) {
    Serial.printf("[%s] WiFi channel: %u\n", tag, primary);
  } else {
    Serial.printf("[%s] esp_wifi_get_channel failed: %s\n", tag, esp_err_to_name(err));
  }
}

static bool homespan_has_wifi_data_in_nvs() {
  nvs_handle_t h;
  if (nvs_open("WIFI", NVS_READONLY, &h) != ESP_OK) return false;

  size_t len = 0;
  esp_err_t err = nvs_get_blob(h, "WIFIDATA", nullptr, &len);
  nvs_close(h);

  // blob exists and is at least the struct size-ish (len>0 is usually enough)
  return (err == ESP_OK && len > 0);
}

static void print_homespan_wifidata_from_nvs() {
  nvs_handle_t h;
  esp_err_t err = nvs_open("WIFI", NVS_READONLY, &h);
  if (err != ESP_OK) {
    Serial.println("WIFI NVS namespace not found");
    return;
  }

  size_t len = 0;
  err = nvs_get_blob(h, "WIFIDATA", nullptr, &len);
  if (err != ESP_OK || len == 0) {
    Serial.println("WIFIDATA not present in NVS");
    nvs_close(h);
    return;
  }

  uint8_t buf[len];
  err = nvs_get_blob(h, "WIFIDATA", buf, &len);
  nvs_close(h);

  if (err != ESP_OK) {
    Serial.println("Failed to read WIFIDATA");
    return;
  }

  struct {
    char ssid[33];
    char pwd[65];
  } wifi;

  memcpy(&wifi, buf, sizeof(wifi));

  Serial.println("---- HomeSpan WIFIDATA (from NVS) ----");
  Serial.printf("SSID: '%s'\n", wifi.ssid);
  Serial.printf("PWD : '%s'\n", wifi.pwd[0] ? "********" : "(empty)");
  Serial.println("-------------------------------------");
}

static void wifi_idf_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA_DISCONNECTED reason=%d", (int)e->reason);
    }
}

// Forward declarations (needed because on_ws_event is above these definitions)
void deleteReaderData(const char* buf = "");
static nlohmann::json build_hkinfo_json();
static void ws_clear_misc();
static bool ws_apply_and_save_misc(const nlohmann::json &incoming, std::string &err);

// ---------------- Device Context ----------------
typedef struct {
  char api_base[192];
  char device_uuid[64];
  char token[768];
} device_ctx_t;

static device_ctx_t g_ctx{};

PN532_SPI *pn532spi;
PN532 *nfc;

static const char *SHARED_UUID = "8cfaa2ec-2ed2-493b-8825-53d90a35e913";
static constexpr std::array<uint8_t, 4> kDefaultNfcGpioPins{
  NFC_PN532_SS, NFC_PN532_SCK, NFC_PN532_MISO, NFC_PN532_MOSI
};

static bool nfc_pin_sets_equal(const std::array<uint8_t, 4> &a, const std::array<uint8_t, 4> &b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void create_nfc_driver(const std::array<uint8_t, 4> &pins)
{
  ESP_LOGI("NFC_SETUP", "PN532 SPI pins: SS=%u SCK=%u MISO=%u MOSI=%u",
           pins[0], pins[1], pins[2], pins[3]);
  pn532spi = new PN532_SPI(pins[0], pins[1], pins[2], pins[3]);
  nfc = new PN532(*pn532spi);
  nfc->begin();
}

static void destroy_nfc_driver()
{
  if (nfc) {
    nfc->stop();
    delete nfc;
    nfc = nullptr;
  }
  if (pn532spi) {
    delete pn532spi;
    pn532spi = nullptr;
  }
}

//uint8_t receiverMAC[] = {0x9C, 0x9E, 0x6E, 0xC3, 0x33, 0x00}; //JT-002 office
//uint8_t receiverMAC[] = {0x9C, 0x9E, 0x6E, 0xC2, 0xFB, 0x10};  //JT-001  test

uint8_t receiverMAC[] = {0xF0, 0xF5, 0xBD, 0xFB, 0xA9, 0x70}; //JT-003 : C0:5D:89:DE:14:F4

static volatile bool g_espnow_busy = false;
static SemaphoreHandle_t g_espnow_send_done = nullptr;
static volatile esp_now_send_status_t g_espnow_last_status = ESP_NOW_SEND_FAIL;

uint8_t espnow_pmk[16];
uint8_t espnow_lmk[16];
const uint8_t ESPNOW_CHANNEL = 6;

static void espnow_blast_task(void *arg);

void onSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  g_espnow_last_status = status;

  ESP_LOGI("ESPNOW", "Send Status: %s",
           status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");

  if (g_espnow_send_done) {
    xSemaphoreGive(g_espnow_send_done);
  }
}

// --- IO pins (change to your real pins) ---
static constexpr int PIN_BUZZER    = 33;
static constexpr int PIN_RED       = 27;
static constexpr int PIN_BACKLIGHT = 25;
static constexpr int PIN_GREEN     = 26;

// Map electrode index -> digit (from your example)
static const int8_t touchPinMap[12] = { 1, 2, 11, 9, 6, 8, 0, 3, 5, 10, 7, 4 };

// Special keys (based on your example logic)
static constexpr int ELE_CANCEL = 10;   // long beep cancel
static constexpr int ELE_ENTER  = 11;  // enter / change pin

static char correctPIN[16] = {0};
static char masterPIN[16]  = "990011";

typedef enum {
  UI_IDLE = 0,
  UI_SPECIAL_MENU_ARMED,
} ui_state_t;

typedef enum {
  MENU_INPUT_TIMEOUT = 0,
  MENU_INPUT_CANCEL,
  MENU_INPUT_ENTER,
  MENU_INPUT_DIGITS
} menu_input_result_t;

static ui_state_t ui_state = UI_IDLE;

static const char MENU_UNLOCK_CODE[] = "990011"; // 6 digits
static const char MENU_ADDCARD_CODE[] = "123456"; // 6 digits
static const char MENU2_CODE_CHANGE_PIN[] = "000";
static const char MENU2_CODE_RESET_WIFI[] = "211";
static const char MENU2_CODE_PLAY[] = "999";
static const char MENU2_CODE_RESTART[] = "888";

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

static inline void beep_correct(bool corr) {
  if (corr){
    digitalWrite(PIN_GREEN, HIGH);
    for (int i = 0; i < 2; i++) { beep_short(); vTaskDelay(pdMS_TO_TICKS(50)); }
    digitalWrite(PIN_GREEN, LOW);
  } else if(!corr){
    digitalWrite(PIN_RED, HIGH);
    beep_long();
    digitalWrite(PIN_RED, LOW);
  }
}

static void set_ui_lights(bool on) {
  digitalWrite(PIN_RED, on ? HIGH : LOW);
  digitalWrite(PIN_GREEN, on ? HIGH : LOW);
  digitalWrite(PIN_BACKLIGHT, on ? HIGH : LOW);
}

static void buzzer_tone(uint16_t freq_hz, uint16_t duration_ms) {
  if (freq_hz == 0) {  // rest
    digitalWrite(PIN_BUZZER, LOW);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    return;
  }

  const uint32_t half_period_us = 1000000UL / (uint32_t)(freq_hz * 2UL);
  const uint32_t cycles = ((uint32_t)duration_ms * 1000UL) / (half_period_us * 2UL);

  for (uint32_t i = 0; i < cycles; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delayMicroseconds(half_period_us);
    digitalWrite(PIN_BUZZER, LOW);
    delayMicroseconds(half_period_us);
  }
}

static void buzzer_pause(uint16_t ms) {
  digitalWrite(PIN_BUZZER, LOW);
  vTaskDelay(pdMS_TO_TICKS(ms));
}

// Note frequencies (Hz)
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_REST 0

static void play_happy_birthday(void) {
  // Each pair: {frequency, duration_ms}
  static const uint16_t melody[][2] = {
    {NOTE_C4, 200}, {NOTE_C4, 200}, {NOTE_D4, 400}, {NOTE_C4, 400}, {NOTE_F4, 400}, {NOTE_E4, 800},
    {NOTE_C4, 200}, {NOTE_C4, 200}, {NOTE_D4, 400}, {NOTE_C4, 400}, {NOTE_G4, 400}, {NOTE_F4, 800},
    {NOTE_C4, 200}, {NOTE_C4, 200}, {NOTE_C5, 400}, {NOTE_A4, 400}, {NOTE_F4, 400}, {NOTE_E4, 400}, {NOTE_D4, 800},
    {NOTE_AS4,200}, {NOTE_AS4,200}, {NOTE_A4, 400}, {NOTE_F4, 400}, {NOTE_G4, 400}, {NOTE_F4, 800},
  };

  for (size_t i = 0; i < (sizeof(melody) / sizeof(melody[0])); i++) {
    buzzer_tone(melody[i][0], melody[i][1]);
    buzzer_pause(40); // small gap between notes
  }

  digitalWrite(PIN_BUZZER, LOW);
}


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
TaskHandle_t backend_task_handle = nullptr;


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
    std::array<uint8_t, 4> nfcGpioPins{NFC_PN532_SS, NFC_PN532_SCK, NFC_PN532_MISO, NFC_PN532_MOSI};
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

void start_backend(){
  xEventGroupSetBits(g_backend_ev, BACKEND_START_BIT);
}

void stop_backend()
{
  xEventGroupSetBits(g_backend_ev, BACKEND_STOP_BIT);
}

// -------------- Network Ready Gate --------------
static EventGroupHandle_t g_net_ev = nullptr;
#define NET_GOT_IP_BIT BIT0

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(g_net_ev, NET_GOT_IP_BIT);

    auto *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG_NET, "GOT_IP: " IPSTR, IP2STR(&ev->ip_info.ip));

    if (!g_espnow_busy) {
      start_backend();
    } else {
      ESP_LOGW(TAG_NET, "GOT_IP ignored because ESP-NOW busy");
    }

  } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    xEventGroupClearBits(g_net_ev, NET_GOT_IP_BIT);
    ESP_LOGW(TAG_NET, "LOST_IP");

    if (!g_espnow_busy) {
      stop_backend();
    } else {
      ESP_LOGW(TAG_NET, "LOST_IP ignored because ESP-NOW busy");
    }
  }
}

static void safe_event_register(esp_event_base_t base, int32_t id,
                                esp_event_handler_t cb, void *arg,
                                const char *name)
{
  esp_err_t err = esp_event_handler_register(base, id, cb, arg);

  // In Arduino/HomeSpan, INVALID_STATE can happen if event loop isn’t ready yet.
  if (err == ESP_OK) {
    ESP_LOGI("NET", "Registered %s", name);
    return;
  }

  if (err == ESP_ERR_INVALID_STATE) {
    ESP_LOGW("NET", "Event loop not ready for %s yet (ESP_ERR_INVALID_STATE). Will continue.", name);
    return;
  }

  ESP_LOGE("NET", "Failed register %s: %s", name, esp_err_to_name(err));
}

static esp_err_t wait_for_ip(uint32_t timeout_ms) {
  if (!g_net_ev) return ESP_FAIL;
  EventBits_t bits = xEventGroupWaitBits(
      g_net_ev, NET_GOT_IP_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
  return (bits & NET_GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void reset_device_nvs(void) {
  nvs_handle_t h;
  if (nvs_open("device_cfg", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG_MAIN, "Device config NVS erased");
  }
}

static void seed_nvs_if_empty(bool force_reset) {
  device_config_t cfg{};
  esp_err_t e = nvs_config_load(&cfg);

  bool need_seed = force_reset ||
                   (e != ESP_OK) ||
                   !nvs_config_has_wifi(&cfg) ||
                   !nvs_config_has_identity(&cfg) ||
                   !nvs_config_has_api_base(&cfg);

  if (!need_seed) {
    ESP_LOGI(TAG_MAIN, "NVS already provisioned. No seeding needed.");
    return;
  }

  ESP_LOGW(TAG_MAIN, "Seeding NVS config (FORCED=%d)", force_reset);

  // ---- DEFAULTS ----
  const char *DEF_SSID   = "YRUS90011";
  const char *DEF_PASS   = "YRUS90011";

  //JT-001
  const char *DEF_UUID   = "e5db0a64-d476-4f5b-b080-736b150daad6";
  const char *DEF_SECRET = "IHd0z7gevTOv1iB_fira42uAyF-Y2VZuVxZ7_xA90Kw";  //test

  //JT-002
  //const char *DEF_UUID   = "af257216-660a-49ee-b443-71907ee0d4bf";
  //const char *DEF_SECRET = "boHaGtcn3MX4MgSj1OzAO6W8_vjsqHQH2KU90vba55U";  //office

  //JT-003
  //const char *DEF_UUID   = "bd02422c-0894-405b-b0bb-8c9ba02460b3";
  //const char *DEF_SECRET = "1BHWXoYeke6igsuOzLGetu5mU8YrHiOoi4ztYP5EauU";  //shubh

  
  //const char *DEF_API    = "https://api.junotech.com.np";
  const char *DEF_API    = "https://api.junotechnologies.com.np";

  memset(&cfg, 0, sizeof(cfg));
  strncpy(cfg.wifi_ssid, DEF_SSID, sizeof(cfg.wifi_ssid) - 1);
  strncpy(cfg.wifi_pass, DEF_PASS, sizeof(cfg.wifi_pass) - 1);
  strncpy(cfg.device_uuid, DEF_UUID, sizeof(cfg.device_uuid) - 1);
  strncpy(cfg.device_secret, DEF_SECRET, sizeof(cfg.device_secret) - 1);
  strncpy(cfg.api_base, DEF_API, sizeof(cfg.api_base) - 1);
  cfg.last_fw_id = -1;

  e = nvs_config_save(&cfg);
  if (e != ESP_OK) {
    ESP_LOGE(TAG_MAIN, "NVS seed failed: %s", esp_err_to_name(e));
  } else {
    ESP_LOGW(TAG_MAIN,
      "NVS seeded:\n  SSID=%s\n  UUID=%s\n  API=%s",
      cfg.wifi_ssid, cfg.device_uuid, cfg.api_base);
  }
}

static void load_pin_from_nvs_or_default(void) {
  esp_err_t err = nvs_config_get_pin(correctPIN, sizeof(correctPIN));

  if (err == ESP_OK && correctPIN[0] != '\0') {
    ESP_LOGI("PIN", "PIN loaded from NVS");
    return;
  }

  ESP_LOGW("PIN", "PIN not found in NVS, writing default PIN");

  const char *default_pin = "927410";

  err = nvs_config_set_pin(default_pin);
  if (err == ESP_OK) {
    strncpy(correctPIN, default_pin, sizeof(correctPIN) - 1);
    correctPIN[sizeof(correctPIN) - 1] = '\0';
    ESP_LOGI("PIN", "Default PIN saved to NVS");
  } else {
    ESP_LOGE("PIN", "Failed to save default PIN: %s", esp_err_to_name(err));

    // Emergency fallback so keypad still works during this boot
    strncpy(correctPIN, default_pin, sizeof(correctPIN) - 1);
    correctPIN[sizeof(correctPIN) - 1] = '\0';
  }
}

// ----------------- OTA gating ------------------
static bool parse_ota_fw_id(const char *cmd, int *out_fw_id) {
  // cmd example: "ota_update, fw=1"
  if (!cmd || !out_fw_id) return false;
  if (strncmp(cmd, "ota_update", 10) != 0) return false;

  const char *p = strstr(cmd, "fw=");
  if (!p) return false;
  p += 3;

  int id = atoi(p);
  if (id <= 0) return false;

  *out_fw_id = id;
  return true;
}

static void ws_send_ota_result(const char *status, int fw_id, const char *message) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"type\":\"event\",\"payload\":{\"response\":\"ota_result\",\"status\":\"%s\",\"fw\":%d,\"message\":\"%s\"}}",
           status ? status : "error", fw_id, message ? message : "");
  (void)ws_client_send_text(buf);
}


bool cmd_get_kv(const char *cmd, const char *key, char *out, size_t out_len)
{
    if (!cmd || !key || !out || out_len == 0) return false;

    // Look for "key=" exactly to avoid matching "uid" inside "delete_uid"
    char pat[48];
    int pn = snprintf(pat, sizeof(pat), "%s=", key);
    if (pn <= 0 || (size_t)pn >= sizeof(pat)) return false;

    const char *p = strstr(cmd, pat);
    if (!p) return false;

    p += pn;  // move past "key="

    size_t i = 0;
    while (*p && *p != ',' && *p != '\r' && *p != '\n' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = 0;

    return i > 0;
}

static bool cmd_is(const char *cmd, const char *name) {
  if (!cmd || !name) return false;
  size_t n = strlen(name);
  return strncmp(cmd, name, n) == 0;
}


static bool cmd_is_exact(const char *cmd, const char *name) {
  return cmd && name && strcmp(cmd, name) == 0;
}


static void fw_list_task(void *arg)
{
  device_ctx_t *a = (device_ctx_t *)arg;

  char *json = NULL;
  int len = 0;

  esp_err_t e = api_get_firmware_list_json(a->api_base, a->device_uuid, a->token, &json, &len);

  if (e != ESP_OK || !json) {
    char msg[256];
    snprintf(msg, sizeof(msg),
      "{\"type\":\"event\",\"payload\":{\"response\":\"fw_list_result\",\"status\":\"error\",\"message\":\"fetch failed\"}}");
    (void)ws_client_send_text(msg);
    free(a);
    vTaskDelete(NULL);
    return;
  }

  // IMPORTANT: JSON may be large. If your ws_client has max frame limits,
  // you may need chunking. For now, send as one message.
  // Escape not needed because json is already JSON; we embed it as a nested object/array (not a string).

  // Build: {"type":"event","payload":{"command":"fw_list_result","status":"success","items":[...]}}
  // We avoid string-embedding to prevent escaping issues.
  // We'll stitch prefix + json + suffix.

  const char *prefix = "{\"type\":\"event\",\"response\":{\"command\":\"fw_list_result\",\"status\":\"success\",\"items\":";
  const char *suffix = "}}";

  int out_sz = (int)strlen(prefix) + len + (int)strlen(suffix) + 1;
  char *out = (char *)malloc(out_sz);
  if (!out) {
    free(json);
    free(a);
    vTaskDelete(NULL);
    return;
  }

  // json from backend is an array: [...]
  snprintf(out, out_sz, "%s%.*s%s", prefix, len, json, suffix);

  (void)ws_client_send_text(out);

  free(out);
  free(json);
  free(a);
  vTaskDelete(NULL);
}


static void ws_send_monitor_snapshot(void)
{
    monitor_snapshot_t snap;
    int rc = monitor_collect_once(&snap);

    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddItemToObject(root, "payload", payload);

    cJSON_AddStringToObject(payload, "command", "monitor_once_result");

    if (rc != 0) {
        cJSON_AddStringToObject(payload, "status", "error");
        cJSON_AddNumberToObject(payload, "code", rc);
    } else {
        cJSON_AddStringToObject(payload, "status", "success");

        cJSON *heap = cJSON_CreateObject();
        cJSON_AddItemToObject(payload, "heap", heap);

        cJSON_AddNumberToObject(heap, "free8", snap.free8);
        cJSON_AddNumberToObject(heap, "min8",  snap.min8);
        cJSON_AddNumberToObject(heap, "lfb8",  snap.lfb8);

        cJSON_AddNumberToObject(heap, "free_int", snap.free_int);
        cJSON_AddNumberToObject(heap, "min_int",  snap.min_int);
        cJSON_AddNumberToObject(heap, "lfb_int",  snap.lfb_int);

        cJSON *tasks = cJSON_CreateArray();
        cJSON_AddItemToObject(payload, "tasks", tasks);

        for (uint32_t i = 0; i < snap.task_count; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", snap.tasks[i].name);
            cJSON_AddNumberToObject(t, "prio", snap.tasks[i].prio);
            cJSON_AddNumberToObject(t, "state", snap.tasks[i].state);
            cJSON_AddNumberToObject(t, "hwm_bytes", snap.tasks[i].hwm_bytes);
            cJSON_AddItemToArray(tasks, t);
        }
    }

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (txt) {
        (void)ws_client_send_text(txt);
        free(txt);
    }
}

static void ws_send_json(const nlohmann::json &j) {
  std::string s = j.dump();
  (void)ws_client_send_text(s.c_str());
}

static void ws_send_ok(const char *cmd, const nlohmann::json &data = {}) {
  nlohmann::json j;
  j["type"] = "event";
  j["payload"]["command"] = cmd;
  j["payload"]["ok"] = true;
  j["payload"]["data"] = data;
  ws_send_json(j);
}

static void ws_send_err(const char *cmd, const char *msg) {
  nlohmann::json j;
  j["type"] = "event";
  j["payload"]["command"] = cmd;
  j["payload"]["ok"] = false;
  j["payload"]["error"] = msg ? msg : "error";
  ws_send_json(j);
}

static nlohmann::json build_hkinfo_json() {
  using json = nlohmann::json;
  json serialized;

  // Start from readerData (msgpack/json compat)
  json input = readerData;

  if (input.contains("group_identifier")) {
    serialized["group_identifier"] =
      red_log::bufToHexString(readerData.reader_gid.data(), readerData.reader_gid.size(), true);
  }
  if (input.contains("unique_identifier")) {
    serialized["unique_identifier"] =
      red_log::bufToHexString(readerData.reader_id.data(), readerData.reader_id.size(), true);
  }

  if (input.contains("issuers")) {
    serialized["issuers"] = json::array();
    for (auto it = input.at("issuers").begin(); it != input.at("issuers").end(); ++it) {
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
      serialized["issuers"].push_back(issuer);
    }
  }

  return serialized;
}


static bool ws_apply_and_save_misc(const nlohmann::json &incoming, std::string &err) {
  using json = nlohmann::json;

  json configData = espConfig::miscConfig;

  // Validate keys/types
  for (auto it = incoming.begin(); it != incoming.end(); ++it) {
    if (!configData.contains(it.key())) { err = "Unknown key: " + it.key(); return false; }

    if (it.key() == "setupCode") {
      if (!it.value().is_string()) { err = "setupCode must be string"; return false; }
      std::string code = it.value().get<std::string>();
      bool allDigits = !code.empty() && std::find_if(code.begin(), code.end(),
                      [](unsigned char c){ return !std::isdigit(c); }) == code.end();
      if (!allDigits || code.length() != 8) { err = "setupCode must be exactly 8 digits"; return false; }
      if (homeSpan.controllerListBegin() != homeSpan.controllerListEnd() &&
          code != configData.at("setupCode").get<std::string>()) {
        err = "Setup Code can only be changed when no devices are paired";
        return false;
      }
    }

    if (it.key().size() >= 3 && it.key().rfind("Pin") == it.key().size() - 3) {
      if (!it.value().is_number()) { err = "Pin must be number: " + it.key(); return false; }
      int v = it.value().get<int>();
      if (v < 0 || v > 255) { err = "Pin out of range: " + it.key(); return false; }
      if (v != 255 && (!GPIO_IS_VALID_GPIO(v) && !GPIO_IS_VALID_OUTPUT_GPIO(v))) {
        err = "Invalid GPIO for: " + it.key();
        return false;
      }
    }

    if (configData.at(it.key()).is_boolean() && it.value().is_number()) {
      // accept 0/1
      // NOTE: can't assign to it.value() directly (const). We'll convert during apply.
    } else if (configData.at(it.key()).is_boolean() && !it.value().is_boolean()) {
      err = "Expected boolean for: " + it.key();
      return false;
    } else if (configData.at(it.key()).type() != it.value().type() && !configData.at(it.key()).is_boolean()) {
      err = "Type mismatch for: " + it.key();
      return false;
    }
  }

  // Apply updates
  for (auto it = incoming.begin(); it != incoming.end(); ++it) {

    if (it.key() == "setupCode") {
      std::string code = it.value().get<std::string>();
      if (homeSpan.controllerListBegin() == homeSpan.controllerListEnd()) {
        homeSpan.setPairingCode(code.c_str());
      }
      configData.at(it.key()) = it.value();
      continue;
    }

    if (configData.at(it.key()).is_boolean() && it.value().is_number()) {
      configData.at(it.key()) = (bool)it.value().get<int>();
      continue;
    }

    configData.at(it.key()) = it.value();
  }

  // Persist MISCDATA (msgpack)
  std::vector<uint8_t> blob = json::to_msgpack(configData);
  esp_err_t a = nvs_set_blob(savedData, "MISCDATA", blob.data(), blob.size());
  esp_err_t b = nvs_commit(savedData);

  if (a != ESP_OK || b != ESP_OK) {
    err = "Failed to save MISCDATA";
    return false;
  }

  // update in-memory struct
  configData.get_to<espConfig::misc_config_t>(espConfig::miscConfig);
  return true;
}

static void ws_clear_misc() {
  nvs_erase_key(savedData, "MISCDATA");
  nvs_commit(savedData);
  espConfig::miscConfig = {};
}

static bool sha256_16(const char *input, uint8_t out16[16]) {
  if (!input || !out16) return false;

  uint8_t hash[32];
  mbedtls_sha256_context ctx;

  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)input, strlen(input));
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  memcpy(out16, hash, 16);
  return true;
}

static bool espnow_derive_keys() {
  if (!sha256_16(SHARED_UUID, espnow_pmk)) return false;
  if (!sha256_16(SHARED_UUID, espnow_lmk)) return false;
  return true;
}

static void print_key_hex(const char *label, const uint8_t *key, size_t len) {
  Serial.print(label);
  for (size_t i = 0; i < len; i++) {
    if (key[i] < 16) Serial.print("0");
    Serial.print(key[i], HEX);
  }
  Serial.println();
}

static bool espnow_enqueue_text(const char *s) {
  if (!espnow_queue || !s) return false;

  // skip leading spaces
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') return false;

  espnow_msg_t m{};
  size_t n = strlen(s);
  if (n >= sizeof(m.uid)) n = sizeof(m.uid) - 1;   // keep null-terminated
  memcpy(m.uid, s, n);
  m.uid[n] = '\0';
  m.len = (uint8_t)(n + 1);

  // non-blocking enqueue
  if (xQueueSend(espnow_queue, &m, 0) != pdTRUE) {
    ESP_LOGW("ESPNOW", "Queue full, dropping: %s", m.uid);
    return false;
  }

  ESP_LOGI("ESPNOW", "Enqueued: %s", m.uid);
  return true;
}

static void ws_send_evtlog_state(void)
{
    evtlog_state_t s;
    if (evtlog_get_state(&s) != ESP_OK) return;

    cJSON *root = cJSON_CreateObject();
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");

    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(payload, "event", "evtlog_state");

    cJSON_AddNumberToObject(payload, "window_start_epoch", s.window_start_epoch);
    cJSON_AddNumberToObject(payload, "boot_time_epoch", s.boot_time_epoch);
    cJSON_AddNumberToObject(payload, "last_reset_reason", s.last_reset_reason);

    cJSON_AddNumberToObject(payload, "boot_count", s.boot_count);
    cJSON_AddNumberToObject(payload, "ws_disconnect_count", s.ws_disc_count);
    cJSON_AddNumberToObject(payload, "nfc_fail_count", s.nfc_fail_count);
    cJSON_AddNumberToObject(payload, "mpr_fail_count", s.mpr_fail_count);

    cJSON_AddNumberToObject(payload, "last_update_epoch", s.last_update_epoch);

    char *json = cJSON_PrintUnformatted(root);
    ws_client_send_text(json);

    cJSON_Delete(root);
    free(json);
}


static void ws_keepalive_task_fn(void *arg)
{
    (void)arg;

    while (1) {
        if (ws_client_is_connected()) {

            if (g_waiting_for_live) {
                g_live_miss_count++;
                ESP_LOGW("WS_KEEP", "Missed live response count=%d", g_live_miss_count);
            }

            if (g_live_miss_count >= LIVE_MISS_LIMIT) {
              ESP_LOGE("WS", "No live response → restarting ESP");
              esp_restart();
            } else {
                esp_err_t err = ws_client_send_text("{\"type\":\"status_check\"}");
                if (err == ESP_OK) {
                    g_waiting_for_live = true;
                    ESP_LOGI("WS_KEEP", "Sent status_check");
                } else {
                    ESP_LOGW("WS_KEEP", "status_check send failed: %s", esp_err_to_name(err));
                }
            }

        } else {
            ESP_LOGW("WS_KEEP", "WS not connected");
        }

        vTaskDelay(pdMS_TO_TICKS(30000));   // 30 sec
    }
}

static bool parse_mac_string(const char *s, uint8_t out[6]) {
  if (!s || !out) return false;

  unsigned int b[6];

  int n = sscanf(
    s,
    "%x:%x:%x:%x:%x:%x",
    &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]
  );

  if (n != 6) {
    n = sscanf(
      s,
      "%x,%x,%x,%x,%x,%x",
      &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]
    );
  }

  if (n != 6) return false;

  for (int i = 0; i < 6; i++) {
    if (b[i] > 0xFF) return false;
    out[i] = (uint8_t)b[i];
  }

  return true;
}

static void receiver_mac_to_string(char *out, size_t out_len) {
  snprintf(out, out_len,
           "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
}

static void save_receiver_mac_to_nvs() {
  nvs_handle_t h;

  if (nvs_open("espnow_cfg", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE("ESPNOW", "Failed to open espnow_cfg NVS");
    return;
  }

  nvs_set_blob(h, "receiver_mac", receiverMAC, 6);
  nvs_commit(h);
  nvs_close(h);

  ESP_LOGI("ESPNOW", "Receiver MAC saved to NVS");
}

static bool load_receiver_mac_from_nvs() {
  nvs_handle_t h;

  if (nvs_open("espnow_cfg", NVS_READONLY, &h) != ESP_OK) {
    ESP_LOGW("ESPNOW", "No espnow_cfg NVS yet, using default receiverMAC");
    return false;
  }

  size_t len = 6;
  esp_err_t err = nvs_get_blob(h, "receiver_mac", receiverMAC, &len);
  nvs_close(h);

  if (err != ESP_OK || len != 6) {
    ESP_LOGW("ESPNOW", "No saved receiver MAC, using default");
    return false;
  }

  char macStr[18];
  receiver_mac_to_string(macStr, sizeof(macStr));
  ESP_LOGI("ESPNOW", "Loaded receiver MAC from NVS: %s", macStr);

  return true;
}

static void on_ws_event(const char *type, const char *payload_command) {

      if (type && strcmp(type, "live") == 0) {
        g_live_miss_count = 0;
        g_waiting_for_live = false;
        ESP_LOGI("WS_KEEP", "LIVE received -> miss counter reset");
        return;
    }

  if (!payload_command || payload_command[0] == 0) return;
  
  // ---------------- OTA command ----------------
  int fw_id = 0;
  if (parse_ota_fw_id(payload_command, &fw_id)) {

    if (g_ctx.api_base[0] == 0 || g_ctx.device_uuid[0] == 0 || g_ctx.token[0] == 0) {
      ws_send_ota_result("error", fw_id, "device ctx not ready");
      return;
    }

    if (ota_client_is_running()) {
      ws_send_ota_result("error", fw_id, "ota already running");
      return;
    }

    ws_send_ota_result("started", fw_id, "starting ota");
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    esp_err_t err = ota_client_start_by_id(g_ctx.api_base, g_ctx.device_uuid, g_ctx.token, fw_id);
    if (err != ESP_OK) {
      (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      ws_send_ota_result("error", fw_id, "failed to start ota task");
    }
    return;
    }

    // ---------------- Ping command ----------------
    if (cmd_is(payload_command, "ping")) {
      (void)ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"pong\"}}");
      return;
    }

    if (cmd_is(payload_command, "evtlog_get")) {
      ws_send_evtlog_state();
      return;
    }

    if (cmd_is(payload_command, "monitor_stop")) {
      monitor_stop();
      ws_client_send_text("{\"type\":\"event\",\"payload\":{\"command\":\"monitor_stop\",\"ok\":true}}");
      return;
    }

    if (cmd_is(payload_command, "add_new_card") || cmd_is(payload_command, "request_card_number")) {
      store_new_card = true;
      ESP_LOGI("WS", "Backend requested card number");
      return;
    }

    if (cmd_is(payload_command, "uids_list")) {
      auto uids = listUIDs();

      // Build JSON in a safe-ish fixed buffer (adjust if you can have many UIDs)
      char msg[1024];
      int n = 0;

      n += snprintf(msg + n, sizeof(msg) - n,
                    "{\"type\":\"event\",\"payload\":{\"command\":\"uids_list\",\"uids\":[");

      for (size_t i = 0; i < uids.size() && n < (int)sizeof(msg) - 8; i++) {
          n += snprintf(msg + n, sizeof(msg) - n,
                        "%s\"%s\"",
                        (i ? "," : ""), uids[i].c_str());
      }

      n += snprintf(msg + n, sizeof(msg) - n, "]}}");

      (void)ws_client_send_text(msg);
      return;
    }

    if (strcmp(payload_command, "get_wifi_channel") == 0) {
      wifi_second_chan_t second;
      uint8_t primary = 0;

      esp_err_t err = esp_wifi_get_channel(&primary, &second);

      if (err == ESP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg),
          "{\"type\":\"event\",\"payload\":{\"command\":\"get_wifi_channel\",\"ok\":true,\"channel\":%u}}",
          primary
        );
        ws_client_send_text(msg);
      } else {
        char msg[160];
        snprintf(msg, sizeof(msg),
          "{\"type\":\"event\",\"payload\":{\"command\":\"get_wifi_channel\",\"ok\":false,\"error\":\"%s\"}}",
          esp_err_to_name(err)
        );
        ws_client_send_text(msg);
      }

      return;
    }

    if (strcmp(payload_command, "get_mac") == 0) {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_WIFI_STA);

      char macStr[18];
      snprintf(macStr, sizeof(macStr),
              "%02X:%02X:%02X:%02X:%02X:%02X",
              mac[0], mac[1], mac[2],
              mac[3], mac[4], mac[5]);

      char msg[160];
      snprintf(msg, sizeof(msg),
        "{\"type\":\"event\",\"payload\":{\"command\":\"get_mac\",\"ok\":true,\"mac\":\"%s\"}}",
        macStr
      );

      ws_client_send_text(msg);
      return;
    }

    if (strcmp(payload_command, "get_receiver_mac") == 0) {
      char macStr[18];
      receiver_mac_to_string(macStr, sizeof(macStr));

      char msg[192];
      snprintf(msg, sizeof(msg),
        "{\"type\":\"event\",\"payload\":{\"command\":\"get_receiver_mac\",\"ok\":true,\"mac\":\"%s\"}}",
        macStr
      );

      ws_client_send_text(msg);
      return;
    }


    if (strncmp(payload_command, "set_receiver_mac", 16) == 0) {
      const char *arg = payload_command + 16;

      while (*arg == ' ' || *arg == '\t' || *arg == ',' || *arg == '=') {
        arg++;
      }

      uint8_t newMac[6];

      if (!parse_mac_string(arg, newMac)) {
        ws_client_send_text(
          "{\"type\":\"event\",\"payload\":{\"command\":\"set_receiver_mac\",\"ok\":false,\"error\":\"invalid mac\"}}"
        );
        return;
      }

      memcpy(receiverMAC, newMac, 6);
      save_receiver_mac_to_nvs();

      if (espnow_initialized) {
        esp_now_del_peer(receiverMAC);
        espnow_initialized = false;
      }

      char macStr[18];
      receiver_mac_to_string(macStr, sizeof(macStr));

      char msg[224];
      snprintf(msg, sizeof(msg),
        "{\"type\":\"event\",\"payload\":{\"command\":\"set_receiver_mac\",\"ok\":true,\"mac\":\"%s\",\"restart_espnow\":true}}",
        macStr
      );

      ws_client_send_text(msg);
      return;
    }

    const char *prefix_blast = "espnow_blast";
    const size_t plen_blast = strlen(prefix_blast);

    if (!strncmp(payload_command, prefix_blast, plen_blast) &&
        (payload_command[plen_blast] == '\0' || payload_command[plen_blast] == ' ' || payload_command[plen_blast] == '\t')) {

        const char *arg = payload_command + plen_blast;

        while (*arg == ' ' || *arg == '\t') arg++;

        if (*arg == '\0') {
            ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"espnow_blast_failed\",\"reason\":\"empty\"}}");
            return;
        }

        // copy to heap (important because task runs async)
        char *msg = strdup(arg);
        if (!msg) {
            ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"espnow_blast_failed\",\"reason\":\"alloc\"}}");
            return;
        }

        xTaskCreatePinnedToCore(
            espnow_blast_task,
            "espnow_blast",
            4096,
            msg,
            3,
            NULL,
            0
        );

        ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"espnow_blast_started\"}}");
        return;
    }

    if (cmd_is(payload_command, "delete_uid")) {

      ESP_LOGI("DELETE", "Delete command received: %s", payload_command);

      char uid[32] = {0};

      if (!cmd_get_kv(payload_command, "uid", uid, sizeof(uid))) {
          ESP_LOGE("DELETE", "Failed to parse uid");
          return;
      }

      ESP_LOGI("DELETE", "Parsed UID: %s", uid);

      bool ok = deleteUID(uid);

      ESP_LOGI("DELETE", "deleteUID result: %s", ok ? "true" : "false");

      char msg[160];
      snprintf(msg, sizeof(msg),
              "{\"type\":\"event\",\"payload\":{\"command\":\"delete_uid\",\"uid\":\"%s\",\"ok\":%s}}",
              uid, ok ? "true" : "false");

      ws_client_send_text(msg);
      return;
    }

    if (cmd_is(payload_command, "deactivate")) {
      char uid[32] = {0};

      if (!cmd_get_kv(payload_command, "uid", uid, sizeof(uid))) {
          ESP_LOGE("DEACT", "Failed to parse uid from: %s", payload_command);
          return;
      }

      bool ok = deactivateUID(uid);

      char msg[160];
      snprintf(msg, sizeof(msg),
              "{\"type\":\"event\",\"payload\":{\"command\":\"deactivate\",\"uid\":\"%s\",\"ok\":%s}}",
              uid, ok ? "true" : "false");

      (void)ws_client_send_text(msg);
      return;
    }

    if (cmd_is(payload_command, "activate")) {
      char uid[32] = {0};

      if (!cmd_get_kv(payload_command, "uid", uid, sizeof(uid))) {
          ESP_LOGE("ACT", "Failed to parse uid from: %s", payload_command);
          return;
      }

      bool removed = activateUID(uid);

      char msg[160];
      snprintf(msg, sizeof(msg),
              "{\"type\":\"event\",\"payload\":{\"command\":\"activate\",\"uid\":\"%s\",\"ok\":%s}}",
              uid, removed ? "true" : "false");

      (void)ws_client_send_text(msg);
      return;
    }

    const char *prefix1 = "monitor_start";
    const size_t plen1 = strlen(prefix1);

    if (!strncmp(payload_command, prefix1, plen1) &&
        (payload_command[plen1] == '\0' || payload_command[plen1] == ' ' || payload_command[plen1] == '\t')) {

      // default 15s
      uint32_t timeout_ms = 15000;

      const char *arg = payload_command + plen1;   // points to "" or " <timeout>"
      while (*arg == ' ' || *arg == '\t') arg++;

      if (*arg) {
        char *end = NULL;
        long v = strtol(arg, &end, 10);
        // accept only if we parsed at least 1 digit
        if (end != arg && v >= 0) {
          timeout_ms = (uint32_t)v;
        }
      }

      monitor_start(timeout_ms);

      char resp[128];
      snprintf(resp, sizeof(resp),
              "{\"type\":\"event\",\"payload\":{\"response\":\"monitor_started\",\"timeout_ms\":%u}}",
              (unsigned)timeout_ms);
      (void)ws_client_send_text(resp);
      return;
    }

     // ---- send_via_esp <data> ----
    const char *prefix = "send_via_esp";
    const size_t plen = strlen(prefix);

    if (!strncmp(payload_command, prefix, plen) &&
      (payload_command[plen] == '\0' || payload_command[plen] == ' ' || payload_command[plen] == '\t')) {

    const char *arg = payload_command + plen;   // points to "" or " <data>"
    bool ok = espnow_enqueue_text(arg);
      if (ok) {
        (void)ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"espnow_queued\"}}");
      } else {
        (void)ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"espnow_queue_failed\"}}");
      }
      return;
    }

    // list firmwares for this device
    if (cmd_is_exact(payload_command, "fw_list")) {
      // ctx must be ready
      if (g_ctx.api_base[0] == 0 || g_ctx.device_uuid[0] == 0 || g_ctx.token[0] == 0) {
        (void)ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"fw_list_result\",\"status\":\"error\",\"message\":\"device ctx not ready\"}}");
        return;
      }

      device_ctx_t *a = (device_ctx_t *)calloc(1, sizeof(*a));
      if (!a) return;

      strncpy(a->api_base, g_ctx.api_base, sizeof(a->api_base) - 1);
      strncpy(a->device_uuid, g_ctx.device_uuid, sizeof(a->device_uuid) - 1);
      strncpy(a->token, g_ctx.token, sizeof(a->token) - 1);

      xTaskCreatePinnedToCore(fw_list_task, "fw_list", 8192, a, 4, NULL, 0);
      return;
    }

    if (cmd_is_exact(payload_command, "monitor_task")) {
      ws_send_monitor_snapshot();
      return;
    }

    if (strcmp(payload_command, "version") == 0) {
      printf("PRINTF_TEST_LINE\n");
      ws_send_ok("printf_test, version=2.5");
      return;
    }

  // 2) Simple commands (no JSON)
    if (strcmp(payload_command, "reboot") == 0) {
      ws_send_ok("reboot");
      vTaskDelay(pdMS_TO_TICKS(250));
      ESP.restart();
      return;
    }

    if (strcmp(payload_command, "start_config_ap") == 0) {
      ws_send_ok("start_config_ap");
      vTaskDelay(pdMS_TO_TICKS(250));
      homeSpan.processSerialCommand("A");
      return;
    }

    if (strcmp(payload_command, "reset_hk_pair") == 0) {
      ws_send_ok("reset_hk_pair");
      vTaskDelay(pdMS_TO_TICKS(250));
      deleteReaderData();
      homeSpan.processSerialCommand("H");
      return;
    }

    if (strcmp(payload_command, "reset_wifi_cred") == 0) {
      ws_send_ok("reset_wifi_cred");
      vTaskDelay(pdMS_TO_TICKS(250));
      homeSpan.processSerialCommand("X");
      return;
    }

    if (strcmp(payload_command, "get_wifi_rssi") == 0) {
      nlohmann::json d;
      d["rssi"] = WiFi.RSSI();
      ws_send_ok("get_wifi_rssi", d);
      return;
    }

    if (strcmp(payload_command, "get_misc") == 0) {
      ws_send_ok("get_misc", nlohmann::json(espConfig::miscConfig));
      return;
    }

    if (strcmp(payload_command, "get_hkinfo") == 0) {
      ws_send_ok("get_hkinfo", build_hkinfo_json());
      return;
    }

    if (strcmp(payload_command, "clear_misc") == 0) {
      ws_clear_misc();
      ws_send_ok("clear_misc");
      return;
    }

    if (strcmp(payload_command, "wifi_disconnect") == 0) {
      esp_err_t e = esp_wifi_disconnect();   // disconnect STA from AP
      if (e == ESP_OK) {
        (void)ws_client_send_text("{\"type\":\"event\",\"payload\":{\"command\":\"wifi_disconnect\",\"ok\":true}}");
      } else {
        char msg[160];
        snprintf(msg, sizeof(msg),
                "{\"type\":\"event\",\"payload\":{\"command\":\"wifi_disconnect\",\"ok\":false,\"err\":%d}}",
                (int)e);
        (void)ws_client_send_text(msg);
      }
      return;
    }

    // 3) save_misc,<json>
    if (strncmp(payload_command, "save_misc,", 10) == 0) {
      const char *json_str = payload_command + 10;

      // exception-free parse
      nlohmann::json incoming = nlohmann::json::parse(json_str, nullptr, false);
      if (incoming.is_discarded() || !incoming.is_object()) {
        ws_send_err("save_misc", "invalid json (expected object)");
        return;
      }

      std::string err;
      if (!ws_apply_and_save_misc(incoming, err)) {
        ws_send_err("save_misc", err.c_str());
        return;
      }

      ws_send_ok("save_misc");
      vTaskDelay(pdMS_TO_TICKS(250));
      ESP.restart();
      return;
    }
      // Unknown command (optional)
      ESP_LOGW(TAG_WS, "Unknown ws command: %s", payload_command);
  }

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

void deleteReaderData(const char* buf) {
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

String indexProcess(const String& var) {
  if (var == "VERSION") {
    const esp_app_desc_t* app_desc = esp_app_get_description();
    std::string app_version = app_desc->version;
    return String(app_version.c_str());
  }
  return "";
}

// ----------------- HomeSpan callbacks -----------------
static void wifiCallback(int status) {
  ESP_LOGI(TAG_NET, "HomeSpan wifiCallback status=%d", status);
}

static void ws_start_task(void *p) {
  (void)p;

  ESP_LOGI(TAG_WS, "Starting WS...");
  esp_err_t e = ws_client_start(
                  g_ctx.api_base,
                  g_ctx.token,
                  on_ws_event
                );

  if (e != ESP_OK) {
    ESP_LOGE(TAG_WS, "ws_client_start failed: %s", esp_err_to_name(e));
    vTaskDelete(NULL);
    return;
  }

  g_backend_started = true;

  ESP_LOGI(TAG_WS, "Backend fully started");
  vTaskDelete(NULL);
}

static void force_dns(const char *dns_ip)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;

    ip4_addr_t a;
    if (!ip4addr_aton(dns_ip, &a)) {
        ESP_LOGW("DNS", "Invalid DNS IP string: %s", dns_ip);
        return;
    }

    dns.ip.u_addr.ip4.addr = a.addr;   // copy the raw address (works across types)

    esp_err_t e = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI("DNS", "Set DNS %s => %s", dns_ip, esp_err_to_name(e));
}

static void relogin_retry_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;

    ESP_LOGW(TAG_MAIN, "Relogin retry in %d ms", delay_ms);

    vTaskDelay(pdMS_TO_TICKS(delay_ms));

    request_relogin();

    vTaskDelete(NULL);
}

static void login_task(void *p) {
  (void)p;

  esp_err_t e = wait_for_ip(60000);
  if (e != ESP_OK) {
    ESP_LOGE(TAG_MAIN, "No IP, login aborted (%s)", esp_err_to_name(e));
    g_login_inflight = false;
    vTaskDelete(NULL);
    ESP.restart();
    return;
  }

  device_config_t cfg{};
  if (nvs_config_load(&cfg) != ESP_OK ||
      !nvs_config_has_identity(&cfg) ||
      !nvs_config_has_api_base(&cfg)) {
    ESP_LOGW(TAG_MAIN, "Missing or invalid config; login aborted");
    g_login_inflight = false;
    vTaskDelete(NULL);
    return;
  }

  LOG(I, "LOGIN here:");
  app_tasks_suspend_all();
  (void)esp_wifi_set_ps(WIFI_PS_NONE);
  force_dns("1.1.1.1");

  char token[768] = {0};
  ESP_LOGI(TAG_MAIN, "Logging in...");
  e = api_device_login(cfg.api_base, cfg.device_uuid, cfg.device_secret,
                       token, sizeof(token));

  if (e != ESP_OK) {
      ESP_LOGE(TAG_MAIN, "Login failed: %s", esp_err_to_name(e));
      app_tasks_resume_all();
      g_backend_running = false;
      g_login_inflight = false;
      xTaskCreatePinnedToCore(relogin_retry_task, "relogin_retry", 4096, (void *)5000, 4, NULL, 0);
      vTaskDelete(NULL);
      return;
  }

  strncpy(g_ctx.api_base, cfg.api_base, sizeof(g_ctx.api_base) - 1);
  strncpy(g_ctx.device_uuid, cfg.device_uuid, sizeof(g_ctx.device_uuid) - 1);
  strncpy(g_ctx.token, token, sizeof(g_ctx.token) - 1);

  ESP_LOGI(TAG_MAIN, "Login OK (token_len=%u)", (unsigned)strlen(g_ctx.token));

  app_tasks_resume_all();
  xTaskCreatePinnedToCore(ws_start_task, "ws_start", 8192, NULL, 4, NULL, 0);

  g_backend_running = true;
  g_login_inflight = false;
  vTaskDelete(NULL);
}

static void request_relogin(void){
    if (g_login_inflight) {
      ESP_LOGW(TAG_MAIN, "Login already inflight, skip");
      return;
    }
    g_login_inflight = true;
    xTaskCreatePinnedToCore(login_task, "login_task", 24576, NULL, 3, NULL, 0);
}

static void backend_task(void *arg)
{
    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            g_backend_ev,
            BACKEND_START_BIT | BACKEND_STOP_BIT,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        if (bits & BACKEND_STOP_BIT) {
            if (g_backend_running) {
                ESP_LOGW("BACKEND", "Stopping backend");
                ws_client_stop();
                g_backend_running = false;
            }
        }

        if (bits & BACKEND_START_BIT) {
            if (!g_backend_running && !g_espnow_busy) {
                ESP_LOGI("BACKEND", "Starting backend");

                ws_client_set_relogin_cb(request_relogin);

                request_relogin();   // login + WS start
                g_backend_running = true;
            } else {
                ESP_LOGW("BACKEND", "Start ignored, already running");
            }
        }
    }
}

static void netif_start(){ 
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGE("NET","netif init %s", esp_err_to_name(e));

  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGE("NET","event loop %s", esp_err_to_name(e));

  g_net_ev = xEventGroupCreate();

  safe_event_register(IP_EVENT, IP_EVENT_STA_GOT_IP,  &ip_event_handler, NULL, "GOT_IP");
  safe_event_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, NULL, "LOST_IP");
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

std::vector<std::string> listUIDs()
{
    std::vector<std::string> out;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("my-app", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error opening NVS handle (%s)", esp_err_to_name(err));
        return out;
    }

    int32_t numUIDs = 0;
    err = nvs_get_i32(nvs_handle, "numUIDs", &numUIDs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        numUIDs = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to read numUIDs (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return out;
    }

    if (numUIDs <= 0) {
        nvs_close(nvs_handle);
        return out;
    }

    out.reserve((size_t)numUIDs);

    char key[24];

    for (int i = 0; i < numUIDs; i++) {
        snprintf(key, sizeof(key), "uid_%d", i);

        size_t needed = 0;
        err = nvs_get_str(nvs_handle, key, NULL, &needed);
        if (err == ESP_ERR_NVS_NOT_FOUND || needed == 0) {
            continue; // tolerate holes
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG_NVS, "Failed to read size %s (%s)", key, esp_err_to_name(err));
            continue;
        }

        char *buf = (char *)malloc(needed);
        if (!buf) {
            ESP_LOGE(TAG_NVS, "malloc failed");
            break;
        }

        err = nvs_get_str(nvs_handle, key, buf, &needed);
        if (err == ESP_OK) {
            out.emplace_back(buf);
        } else {
            ESP_LOGW(TAG_NVS, "Failed to read %s (%s)", key, esp_err_to_name(err));
        }

        free(buf);
    }

    nvs_close(nvs_handle);
    return out;
}

bool activateUID(const std::string& uid)
{
    if (uid.empty()) return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open("my-app", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_open failed (%s)", esp_err_to_name(err));
        return false;
    }

    int32_t n = 0;
    err = nvs_get_i32(h, "numDeactivated", &n);
    if (err == ESP_ERR_NVS_NOT_FOUND) n = 0;
    else if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_get_i32 numDeactivated failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    if (n <= 0) {
        nvs_close(h);
        return false;
    }

    // Compact list in-place: keep entries that are NOT uid
    int32_t write_idx = 0;
    bool removed_any = false;

    for (int32_t read_idx = 0; read_idx < n; read_idx++) {
        char key_src[24];
        snprintf(key_src, sizeof(key_src), "deact_%ld", (long)read_idx);

        size_t needed = 0;
        err = nvs_get_str(h, key_src, NULL, &needed);
        if (err == ESP_ERR_NVS_NOT_FOUND || needed == 0) {
            continue;
        }
        if (err != ESP_OK) {
            continue;
        }

        char *buf = (char *)malloc(needed);
        if (!buf) {
            ESP_LOGE(TAG_DEACT, "malloc failed");
            break;
        }

        err = nvs_get_str(h, key_src, buf, &needed);
        if (err != ESP_OK) {
            free(buf);
            continue;
        }

        if (uid == buf) {
            removed_any = true;
            free(buf);
            continue; // skip this (delete)
        }

        // keep this entry: move to write_idx if needed
        if (write_idx != read_idx) {
            char key_dst[24];
            snprintf(key_dst, sizeof(key_dst), "deact_%ld", (long)write_idx);

            err = nvs_set_str(h, key_dst, buf);
            if (err != ESP_OK) {
                ESP_LOGE(TAG_DEACT, "nvs_set_str %s failed (%s)", key_dst, esp_err_to_name(err));
                free(buf);
                nvs_close(h);
                return removed_any;
            }
        }

        free(buf);
        write_idx++;
    }

    // Erase leftover keys from write_idx..n-1
    for (int32_t i = write_idx; i < n; i++) {
        char key[24];
        snprintf(key, sizeof(key), "deact_%ld", (long)i);
        (void)nvs_erase_key(h, key);
    }

    // Update count
    err = nvs_set_i32(h, "numDeactivated", write_idx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_set_i32 numDeactivated failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return removed_any;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_commit failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return removed_any;
    }

    nvs_close(h);
    return removed_any;
}

bool deactivateUID(const std::string& uid)
{
    if (uid.empty()) return false;

    // avoid duplicates
    if (isUIDDeactivated(uid)) return true;

    nvs_handle_t h;
    esp_err_t err = nvs_open("my-app", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_open failed (%s)", esp_err_to_name(err));
        return false;
    }

    int32_t n = 0;
    err = nvs_get_i32(h, "numDeactivated", &n);
    if (err == ESP_ERR_NVS_NOT_FOUND) n = 0;
    else if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_get_i32 numDeactivated failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    char key[16];
    snprintf(key, sizeof(key), "deact_%ld", (long)n);

    err = nvs_set_str(h, key, uid.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_set_str %s failed (%s)", key, esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    n++;
    err = nvs_set_i32(h, "numDeactivated", n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_set_i32 numDeactivated failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_commit failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    nvs_close(h);
    return true;
}

bool deleteUID(const std::string& uid)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("my-app", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Error opening NVS handle (%s)", esp_err_to_name(err));
        return false;
    }

    int32_t numUIDs = 0;
    err = nvs_get_i32(nvs_handle, "numUIDs", &numUIDs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        numUIDs = 0;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to read numUIDs (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    if (numUIDs <= 0) {
        nvs_close(nvs_handle);
        return false;
    }

    // Find UID index
    int found_idx = -1;
    char key[16];

    for (int i = 0; i < numUIDs; i++) {
        snprintf(key, sizeof(key), "uid_%d", i);

        size_t needed = 0;
        err = nvs_get_str(nvs_handle, key, NULL, &needed);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err != ESP_OK || needed == 0) continue;

        // Read value
        char *val = (char *)malloc(needed);
        if (!val) {
            ESP_LOGE(TAG_NVS, "malloc failed");
            nvs_close(nvs_handle);
            return false;
        }

        err = nvs_get_str(nvs_handle, key, val, &needed);
        if (err == ESP_OK) {
            if (uid == val) {
                found_idx = i;
                free(val);
                break;
            }
        }
        free(val);
    }

    if (found_idx < 0) {
        nvs_close(nvs_handle);
        return false; // not found
    }

    // Shift items down to keep uid_0..uid_(numUIDs-2) contiguous
    for (int i = found_idx; i < numUIDs - 1; i++) {
        char key_src[16], key_dst[16];
        snprintf(key_src, sizeof(key_src), "uid_%d", i + 1);
        snprintf(key_dst, sizeof(key_dst), "uid_%d", i);

        size_t needed = 0;
        err = nvs_get_str(nvs_handle, key_src, NULL, &needed);
        if (err == ESP_ERR_NVS_NOT_FOUND || needed == 0) {
            // If there's a hole (shouldn't happen), erase dst and continue
            (void)nvs_erase_key(nvs_handle, key_dst);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG_NVS, "Failed read size %s (%s)", key_src, esp_err_to_name(err));
            nvs_close(nvs_handle);
            return false;
        }

        char *val = (char *)malloc(needed);
        if (!val) {
            ESP_LOGE(TAG_NVS, "malloc failed");
            nvs_close(nvs_handle);
            return false;
        }

        err = nvs_get_str(nvs_handle, key_src, val, &needed);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_NVS, "Failed read %s (%s)", key_src, esp_err_to_name(err));
            free(val);
            nvs_close(nvs_handle);
            return false;
        }

        err = nvs_set_str(nvs_handle, key_dst, val);
        free(val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_NVS, "Failed write %s (%s)", key_dst, esp_err_to_name(err));
            nvs_close(nvs_handle);
            return false;
        }
    }

    // Erase last entry (now duplicated or stale)
    char key_last[16];
    snprintf(key_last, sizeof(key_last), "uid_%d", (int)(numUIDs - 1));
    (void)nvs_erase_key(nvs_handle, key_last);

    // Update count
    numUIDs--;
    err = nvs_set_i32(nvs_handle, "numUIDs", numUIDs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to write numUIDs (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to commit (%s)", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }

    nvs_close(nvs_handle);
    return true;
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

bool isUIDDeactivated(const std::string& uid)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("my-app", NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_open failed (%s)", esp_err_to_name(err));
        return false;
    }

    int32_t n = 0;
    err = nvs_get_i32(h, "numDeactivated", &n);
    if (err == ESP_ERR_NVS_NOT_FOUND) n = 0;
    else if (err != ESP_OK) {
        ESP_LOGE(TAG_DEACT, "nvs_get_i32 numDeactivated failed (%s)", esp_err_to_name(err));
        nvs_close(h);
        return false;
    }

    char key[24];

    for (int32_t i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "deact_%ld", (long)i);

        size_t needed = 0;
        err = nvs_get_str(h, key, NULL, &needed);
        if (err == ESP_ERR_NVS_NOT_FOUND || needed == 0) continue;
        if (err != ESP_OK) continue;

        char *buf = (char *)malloc(needed);
        if (!buf) {
            ESP_LOGE(TAG_DEACT, "malloc failed");
            break;
        }

        err = nvs_get_str(h, key, buf, &needed);
        if (err == ESP_OK) {
            if (uid == buf) {
                free(buf);
                nvs_close(h);
                return true;
            }
        }

        free(buf);
    }

    nvs_close(h);
    return false;
}

static bool espnow_init_once() {
  if (espnow_initialized) return true;

  WiFi.mode(WIFI_STA);
  delay(50);

  if (!espnow_derive_keys()) {
    ESP_LOGE("ESPNOW", "Key derivation failed");
    return false;
  }

  print_key_hex("ESPNOW PMK: ", espnow_pmk, 16);
  print_key_hex("ESPNOW LMK: ", espnow_lmk, 16);

  if (esp_now_init() != ESP_OK) {
    ESP_LOGE("ESPNOW", "esp_now_init failed");
    return false;
  }

  if (esp_now_set_pmk(espnow_pmk) != ESP_OK) {
    ESP_LOGE("ESPNOW", "esp_now_set_pmk failed");
    return false;
  }

  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, ESP_NOW_ETH_ALEN);
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = true;
  memcpy(peerInfo.lmk, espnow_lmk, 16);

  if (esp_now_is_peer_exist(receiverMAC)) {
    esp_now_del_peer(receiverMAC);
  }

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    ESP_LOGE("ESPNOW", "Failed to add encrypted peer");
    return false;
  }

  espnow_initialized = true;
  ESP_LOGI("ESPNOW", "Initialized with encryption");
  printWifiChannel("TX after ESP-NOW");
  return true;
}

static void espnow_task_entry(void *arg) {
  ESP_LOGI("ESPNOW", "Task started");
  load_receiver_mac_from_nvs();

  while (!espnow_init_once()) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  espnow_msg_t msg{};
  while (true) {
    if (xQueueReceive(espnow_queue, &msg, portMAX_DELAY) == pdTRUE) {
      g_espnow_busy = true;

      ESP_LOGW("ESPNOW", "Pausing HomeSpan/WiFi reconnect for ESP-NOW send");

      esp_wifi_disconnect();
      vTaskDelay(pdMS_TO_TICKS(300));

      esp_err_t ch = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
      if (ch != ESP_OK) {
        ESP_LOGE("ESPNOW", "set channel %u failed: %s",
                ESPNOW_CHANNEL, esp_err_to_name(ch));
        g_espnow_busy = false;
        esp_wifi_connect();
        continue;
      }

      printWifiChannel("TX forced channel");

      xSemaphoreTake(g_espnow_send_done, 0);

      esp_err_t r = esp_now_send(
        receiverMAC,
        reinterpret_cast<const uint8_t *>(msg.uid),
        msg.len
      );

      if (r != ESP_OK) {
        ESP_LOGW("ESPNOW", "esp_now_send failed: %s", esp_err_to_name(r));
      } else {
        if (xSemaphoreTake(g_espnow_send_done, pdMS_TO_TICKS(1500)) == pdTRUE) {
          ESP_LOGI("ESPNOW", "Send completed, status=%s",
                  g_espnow_last_status == ESP_NOW_SEND_SUCCESS ? "success" : "fail");
        } else {
          ESP_LOGW("ESPNOW", "Send callback timeout");
        }
      }

      vTaskDelay(pdMS_TO_TICKS(200));

      g_espnow_busy = false;

      ESP_LOGW("ESPNOW", "Resuming HomeSpan/WiFi reconnect");
      esp_wifi_connect();
    }
  }
}

static void espnow_blast_task(void *arg)
{
    char *msg = (char *)arg;

    ESP_LOGI("ESPNOW", "Starting blast: %s", msg);

    TickType_t start = xTaskGetTickCount();
    TickType_t duration = pdMS_TO_TICKS(30000);  // 1 minute

    while ((xTaskGetTickCount() - start) < duration) {

        espnow_enqueue_text(msg);

        vTaskDelay(pdMS_TO_TICKS(200));  // send every 200ms (~5 msgs/sec)
    }

    ESP_LOGI("ESPNOW", "Blast finished");

    free(msg);

    ws_client_send_text("{\"type\":\"event\",\"payload\":{\"response\":\"espnow_blast_done\"}}");

    vTaskDelete(NULL);
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


void nfc_thread_entry(void* arg) {
  uint32_t versiondata = nfc->getFirmwareVersion();if (!versiondata) {ESP_LOGE("NFC_SETUP", "Error establishing PN532 connection");nfc->stop();xTaskCreatePinnedToCore(nfc_retry, "nfc_reconnect_task", 8192, NULL, 10, &nfc_reconnect_task, 0);evtlog_push(EVT_NFC_FAIL);app_task_register(nfc_reconnect_task, true);vTaskSuspend(NULL);} else {unsigned int model = (versiondata >> 24) & 0xFF;ESP_LOGI("NFC_SETUP", "Found chip PN5%x", model);int maj = (versiondata >> 16) & 0xFF;int min = (versiondata >> 8) & 0xFF;ESP_LOGI("NFC_SETUP", "Firmware ver. %d.%d", maj, min);nfc->SAMConfig();nfc->setRFField(0x02, 0x01);nfc->setPassiveActivationRetries(0);ESP_LOGI("NFC_SETUP", "Waiting for an ISO14443A card");}

  // Refresh ECP with GID + CRCif (readerData.reader_gid.size() > 0) {memcpy(ecpData + 8, readerData.reader_gid.data(), readerData.reader_gid.size());with_crc16(ecpData, 16, ecpData + 16);}

  while (1) {
  // Keep PN532 stable
  uint8_t res[4];
  uint16_t resLen = 4;

  bool writeStatus = nfc->writeRegister(0x633d, 0, true);
  if (!writeStatus) {
    LOG(W, "writeRegister failed, reconnecting PN532");
    nfc->stop();
    xTaskCreatePinnedToCore(nfc_retry, "nfc_reconnect_task", 8192, NULL, 10, &nfc_reconnect_task, 0);
    evtlog_push(EVT_NFC_FAIL);
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
    /*if (espnow_queue) {
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
    }*/

    //ESP_LOGI("NFC_TAG", "Detected UID: %s", detectedUID.c_str());

    // Optional UID whitelist unlock (non-HomeKey cards)
    if (isUIDStored(detectedUID) && !isUIDDeactivated(detectedUID)) {
      ESP_LOGI("NFC", "UID found");
      //espnow_enqueue_text(detectedUID.c_str());
      espnow_enqueue_text("UNLOCK");
      beep_correct(true);
    } else if((!isUIDStored(detectedUID) && !store_new_card) || isUIDDeactivated(detectedUID)){
      beep_correct(false);
      ESP_LOGI("NFC", "UID not found");
    } else if (!isUIDStored(detectedUID) && store_new_card) {
        beep_short();
        set_ui_lights(true);
        addUID(detectedUID);   // still store locally if required
        char msg[192];
        snprintf(msg, sizeof(msg), "{\"type\":\"card_number\",\"payload\":{\"card_number\":\"%s\"}}", detectedUID.c_str());
        ws_client_send_text(msg);
        ESP_LOGI("WS", "Sent card_number: %s", detectedUID.c_str());
        store_new_card = false;
        beep_short();
        set_ui_lights(false);
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

        espnow_enqueue_text("UNLOCK");
        ESP_LOGI("ESPNOW", "HomeKey UID sent via ESP-NOW: %s", detectedUID.c_str());

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
        if (touchPinMap[ele] == ELE_ENTER) {
          continue;
        }
        if (touchPinMap[ele] == ELE_CANCEL){
          return false; // cancel input
        }
        beep_short();
        int digit = touchPinMap[ele];
        ESP_LOGI("KEYPAD", "Pin %d touched", digit);
        if (digit >= 0 && digit <= 9) {
          out6[count++] = char('0' + digit);
        }
        // small debounce
        vTaskDelay(pdMS_TO_TICKS(150));
        break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  out6[6] = '\0';
  return true;
}

static bool read_code_digits(MPR121_t *dev, char *out, int digits, TickType_t timeout_ticks) {
  int count = 0;
  TickType_t start = xTaskGetTickCount();

  while (count < digits) {
    if ((xTaskGetTickCount() - start) > timeout_ticks) {
      out[0] = '\0';
      return false;
    }
    MPR121_updateAll(dev);
    for (int ele = 0; ele < 12; ele++) {
      if (!MPR121_isNewTouch(dev, ele)) continue;
      if (touchPinMap[ele] == ELE_ENTER) continue;
      if (touchPinMap[ele] == ELE_CANCEL) {
        out[0] = '\0';
        return false;
      }
      int digit = touchPinMap[ele];
      if (digit >= 0 && digit <= 9) {
        beep_short();
        out[count++] = (char)('0' + digit);
      }
      vTaskDelay(pdMS_TO_TICKS(150));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  out[count] = '\0';
  return true;
}

static bool check_pin_code(const char *pin, int mode) {
  if (!pin || pin[0] == '\0') return false;

  if (strcmp(pin, correctPIN) == 0 || strcmp(pin, masterPIN) == 0) {
    // success
    if (mode == 0) {
      beep_correct(true);
    } else {
      beep_short();
    }
    return true;
  }
  beep_correct(false);
  return false;
}

static void reset_wifi(void) {
  homeSpan.processSerialCommand("X");
  beep_short();
  beep_short();
}

static void change_password_flow(MPR121_t *dev) {
  set_ui_lights(true);

  char pin[7];
  if (!read_pin_code(dev, pin, pdMS_TO_TICKS(15000))) {
    beep_long();
    set_ui_lights(false);
    return;
  }

  if (!check_pin_code(pin, 1)) {
    beep_long();
    set_ui_lights(false);
    return;
  }

  char new_pin[7], re_pin[7];

  if (!read_pin_code(dev, new_pin, pdMS_TO_TICKS(15000))) {
    beep_long();
    set_ui_lights(false);
    return;
  }

  beep_short(); // between new_pin and re_pin

  if (!read_pin_code(dev, re_pin, pdMS_TO_TICKS(15000)) ||
      strcmp(new_pin, re_pin) != 0) {
    beep_long();
    set_ui_lights(false);
    return;
  }

  esp_err_t err = nvs_config_set_pin(new_pin);
  if (err != ESP_OK) {
    ESP_LOGE("PIN", "Failed to save new PIN: %s", esp_err_to_name(err));
    beep_long();
    set_ui_lights(false);
    return;
  }

  strncpy(correctPIN, new_pin, sizeof(correctPIN) - 1);
  correctPIN[sizeof(correctPIN) - 1] = '\0';

  ESP_LOGI("PIN", "New PIN saved to NVS");

  beep_short();
  beep_short();
  set_ui_lights(false);
}

static void special_menu_level2(MPR121_t *dev) {
  char menu_code[4]; // 3 digits + null
  if (!read_code_digits(dev, menu_code, 3, pdMS_TO_TICKS(8000))) {
    beep_long();
    return;
  }

  if (strcmp(menu_code, MENU2_CODE_CHANGE_PIN) == 0) {
    beep_short();
    change_password_flow(dev);
    return;
  }

  if (strcmp(menu_code, MENU2_CODE_RESET_WIFI) == 0) {
    beep_short();
    reset_wifi();
    return;
  }
  
  if (strcmp(menu_code, MENU2_CODE_PLAY) == 0) {
    beep_short();
    play_happy_birthday();
    return;
  }

  if (strcmp(menu_code, MENU2_CODE_RESTART) == 0) {
    beep_short();
    ESP.restart();
    return;
  }

  beep_long(); // unknown code
}

static menu_input_result_t read_menu_input(
    MPR121_t *dev,
    char *out,
    int digits,
    TickType_t timeout_ticks
) {
  int count = 0;
  TickType_t start = xTaskGetTickCount();

  while (true) {

    if ((xTaskGetTickCount() - start) > timeout_ticks) {
      out[0] = '\0';
      return MENU_INPUT_TIMEOUT;
    }

    MPR121_updateAll(dev);

    for (int ele = 0; ele < 12; ele++) {
      if (!MPR121_isNewTouch(dev, ele)) continue;

      // CANCEL
      if (touchPinMap[ele] == ELE_CANCEL) {
        out[0] = '\0';
        return MENU_INPUT_CANCEL;
      }

      // ENTER
      if (touchPinMap[ele] == ELE_ENTER) {
        out[0] = '\0';
        return MENU_INPUT_ENTER;
      }

      // DIGIT
      int digit = touchPinMap[ele];
      if (digit >= 0 && digit <= 9) {
        beep_short();
        out[count++] = (char)('0' + digit);

        if (count >= digits) {
          out[count] = '\0';
          return MENU_INPUT_DIGITS;
        }

        vTaskDelay(pdMS_TO_TICKS(150));
      }

      break;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Returns true if it "handled" the ENTER press (so caller should return)
static void special_menu_level1(MPR121_t *dev) {

  set_ui_lights(true);

  char code6[7];

  menu_input_result_t result =
      read_menu_input(dev, code6, 3, pdMS_TO_TICKS(15000));

  switch (result) {

    case MENU_INPUT_ENTER:
      beep_short();
      change_password_flow(dev);
      break;

    case MENU_INPUT_DIGITS:
      if (strcmp(code6, "901") == 0) {
        beep_short();
        special_menu_level2(dev);
      } 
      if (strcmp(code6, "111") == 0) {
        char pin[7];
        if (read_pin_code(dev, pin, pdMS_TO_TICKS(15000))) {
          if (check_pin_code(pin, 0)) {
            beep_short();
            store_new_card = true;
          }
        }
      } 
      else {
        beep_long();
      }
      break;

    case MENU_INPUT_CANCEL:
      beep_long();
      break;

    case MENU_INPUT_TIMEOUT:
    default:
      beep_long();
      break;
  }

  set_ui_lights(false);
}

static void touch_keypad(MPR121_t *dev) {

  // Always refresh touch state
  MPR121_updateAll(dev);

  for (int ele = 0; ele < 12; ele++) {

    if (!MPR121_isNewTouch(dev, ele)) continue;

    // CANCEL key
    if (touchPinMap[ele] == ELE_CANCEL) {
      beep_long();
      ui_state = UI_IDLE;
      espnow_enqueue_text("RINGBELL");
      return;
    }
    
    
    // ENTER key: change PIN flow (requires existing PIN first)
    if (touchPinMap[ele] == ELE_ENTER) {
      if (ui_state == UI_IDLE) {
        ui_state = UI_SPECIAL_MENU_ARMED;
        beep_short();        // armed
        return;
      }

      if (ui_state == UI_SPECIAL_MENU_ARMED) {
        // 2nd ENTER enters Level-1 selector:
        special_menu_level1(dev);
        ui_state = UI_IDLE;
        return;
      }

      ui_state = UI_IDLE;
      return;
    }

    // Any normal key: turn on backlight, ask for PIN, validate
    digitalWrite(PIN_BACKLIGHT, HIGH);
    //beep_short();

    char pin[7];
    if (read_pin_code(dev, pin, pdMS_TO_TICKS(15000))) {
      if (check_pin_code(pin, 0)) {
        espnow_enqueue_text("UNLOCK");

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

    digitalWrite(PIN_BACKLIGHT, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));

    bool ret = MPR121_begin(&dev, CONFIG_I2C_ADDRESS,
                            touchThreshold, releaseThreshold,
                            CONFIG_IRQ_GPIO, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO);

    ESP_LOGI(TAG, "MPR121_begin=%d", ret);
    if (!ret) {
        ESP_LOGE(TAG, "MPR121 init failed, err=%d", (int)MPR121_getError(&dev));
        evtlog_push(EVT_MPR_FAIL);
        vTaskDelete(NULL);
    }

    MPR121_setFFI(&dev, FFI_10);
    MPR121_setSFI(&dev, SFI_10);
    MPR121_setGlobalCDT(&dev, CDT_4US);
    MPR121_autoSetElectrodesDefault(&dev, true);
    MPR121_updateTouchData(&dev);

    digitalWrite(PIN_BACKLIGHT, LOW);
    vTaskDelay(pdMS_TO_TICKS(50));

    while (1) {
        touch_keypad(&dev);
        homeSpan.resetWatchdog();
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
  
  load_pin_from_nvs_or_default();

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
  create_nfc_driver(espConfig::miscConfig.nfcGpioPins);
  if (!nfc->getFirmwareVersion() &&
      !nfc_pin_sets_equal(espConfig::miscConfig.nfcGpioPins, kDefaultNfcGpioPins)) {
    ESP_LOGW("NFC_SETUP", "PN532 did not answer using saved pins; retrying default pins");
    destroy_nfc_driver();
    espConfig::miscConfig.nfcGpioPins = kDefaultNfcGpioPins;
    create_nfc_driver(espConfig::miscConfig.nfcGpioPins);
  }
  io_init();

  if (espConfig::miscConfig.gpioActionPin && espConfig::miscConfig.gpioActionPin != 255) {
    pinMode(espConfig::miscConfig.gpioActionPin, OUTPUT);
  }

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
  monitor_start(15000);
  //print_homespan_wifidata_from_nvs();

  if (!homespan_has_wifi_data_in_nvs()) {
    homeSpan.enableAutoStartAP();
  }
  else{
    netif_start();
    //reset_device_nvs();
    seed_nvs_if_empty(false);
    ESP_ERROR_CHECK(evtlog_init_and_start());
    printWifiChannel("TX after WiFi connect");
  }

  
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
  ota_mark_valid_if_pending_verify();
  
  if (espConfig::miscConfig.gpioActionPin != 255 || espConfig::miscConfig.hkDumbSwitchMode) {
    xTaskCreatePinnedToCore(gpio_task, "gpio_task", 4096, NULL, 5, &gpio_lock_task_handle, 0);
    app_task_register(gpio_lock_task_handle, true);
  }
  xTaskCreatePinnedToCore(espnow_task_entry, "espnow_task", 6144, NULL, 3, &espnow_task_handle, 1);
  //app_task_register(espnow_task_handle, true);

  xTaskCreatePinnedToCore(nfc_thread_entry, "nfc_task", 12288, NULL, 2, &nfc_poll_task, 1);
  //app_task_register(nfc_poll_task, true);
  
  xTaskCreatePinnedToCore(mpr121_task_entry, "mpr121_task", 6144, NULL, 1, &mpr121_task_handle, 1);
  //app_task_register(mpr121_task_handle, true);
  monitor_stop();
  g_backend_ev = xEventGroupCreate();
  xTaskCreatePinnedToCore(backend_task, "backend_task", 8192, NULL, 5, NULL, 0);

  xTaskCreatePinnedToCore(ws_keepalive_task_fn, "ws_keepalive", 4096, NULL, 4, &g_ws_keepalive_task, 0);
  app_task_register(g_ws_keepalive_task, true);

  esp_log_level_set("wifi", ESP_LOG_WARN);
  g_espnow_send_done = xSemaphoreCreateBinary();
 }

//////////////////////////////////////

void loop() {
  if (!g_espnow_busy) {
    homeSpan.poll();
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}
