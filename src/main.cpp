#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <PubSubClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctime>
#include <cstring>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

LV_FONT_DECLARE(font_20);
LV_FONT_DECLARE(font_32);

namespace {

char backendHost[20] = "";

constexpr uint16_t kBackendPort = 8000;
constexpr uint16_t kMqttPort = 1883;
constexpr uint16_t kUdpPort = 5555;

constexpr char kDiscoveryMsg[] = "DISCOVER_SMART_DASHBOARD";
constexpr char kDashboardPath[] = "/api/dashboard";
constexpr char kTogglePath[] = "/api/smarthome/toggle?device=";
constexpr char kMqttTopicState[] = "smart/dashboard/state";
constexpr char kMqttTopicTelemetry[] = "smart/dashboard/telemetry";
constexpr char kMqttTopicHeartbeat[] = "smart/dashboard/heartbeat";
constexpr char kMqttClientId[] = "cyd-dashboard";

constexpr char kNtpServer[] = "pool.ntp.org";
constexpr char kTimezone[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";

constexpr char kDeviceKomp[] = "komp_iuter";
constexpr char kDeviceSvitlo[] = "svitlo";

constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;

constexpr uint32_t kDashboardPollIntervalMs = 150000UL;
constexpr uint32_t kTelemetryIntervalMs = 60000UL;
constexpr uint16_t kHttpTimeoutMs = 6000;
constexpr uint16_t kUiLoopDelayMs = 1;
constexpr uint32_t kReconnectDelayMs = 3000UL;
constexpr uint32_t kNetworkLoopDelayMs = 50UL;
constexpr uint32_t kSwitchAckTimeoutMs = 3000UL;

constexpr uint8_t kBacklightPin = 21;
constexpr uint32_t kScreenSaverTimeoutMs = 30000UL;
constexpr uint8_t kBrightnessMax = 255;
constexpr uint8_t kBrightnessDim = 100;
constexpr uint32_t kWatchdogTimeoutS = 15;

constexpr uint8_t kTouchIrq = 36;
constexpr uint8_t kTouchMosi = 32;
constexpr uint8_t kTouchMiso = 39;
constexpr uint8_t kTouchClk = 25;
constexpr uint8_t kTouchCs = 33;

constexpr int16_t kTouchRawMinX = 200;
constexpr int16_t kTouchRawMaxX = 3800;
constexpr int16_t kTouchRawMinY = 200;
constexpr int16_t kTouchRawMaxY = 3800;

constexpr uint16_t kDrawBufferLines = 25;
constexpr size_t kDrawBufferSize = kScreenWidth * kDrawBufferLines;
constexpr size_t kDashboardJsonCapacity = 1024;
constexpr size_t kMqttJsonCapacity = 1024;

constexpr uint32_t kNetworkTaskStackSize = 8192;
constexpr UBaseType_t kNetworkTaskPriority = 1;

TFT_eSPI tft;
SPIClass touchSpi(HSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);

WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

lv_disp_draw_buf_t drawBuffer{};
lv_color_t drawBufferPixels[kDrawBufferSize];

lv_obj_t* labelTime = nullptr;
lv_obj_t* labelDate = nullptr;
lv_obj_t* labelWeather = nullptr;
lv_obj_t* labelCity = nullptr;
lv_obj_t* labelFuel = nullptr;
lv_obj_t* labelRates = nullptr;
lv_obj_t* lblKomp = nullptr;
lv_obj_t* swKomp = nullptr;
lv_obj_t* lblSvitlo = nullptr;
lv_obj_t* swSvitlo = nullptr;
lv_obj_t* labelSysStatus = nullptr;
lv_obj_t* wifiSetupOverlay = nullptr;
lv_obj_t* lblSmartError = nullptr;

SemaphoreHandle_t dataMutex = nullptr;
TaskHandle_t networkTaskHandle = nullptr;

uint32_t lastLvglTickMs = 0;
uint32_t lastTouchMs = 0;
bool isScreenOn = true;
bool timeSyncStarted = false;
bool isApplyingSwitchState = false;

bool reqToggleKomp = false;
bool reqToggleSvitlo = false;

bool localKompTogglePending = false;
bool localSvitloTogglePending = false;
uint32_t localKompToggleDeadlineMs = 0;
uint32_t localSvitloToggleDeadlineMs = 0;

struct DashboardData {
    char weather[64];
    char usd[16];
    char eur[16];
    char fuel[96];
    char status[32];
    char sysMessage[32];
    bool kompOn;
    bool svitloOn;
    bool needWifiSetup;
    bool isSmartReady;
    char smartErrorMsg[64];
    bool haOffline;
    uint32_t lastHeartbeatMs;
    bool dirty;
};

DashboardData sharedData{
    "Loading...",
    "...",
    "...",
    "Loading...",
    "starting...",
    "",
    false,
    false,
    false,
    false,
    "Init...",
    false,
    0,
    false
};

class DataLockGuard final {
public:
    DataLockGuard() {
        if (dataMutex != nullptr) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            locked_ = true;
        }
    }

    ~DataLockGuard() {
        if (locked_) {
            xSemaphoreGive(dataMutex);
        }
    }

    DataLockGuard(const DataLockGuard&) = delete;
    DataLockGuard& operator=(const DataLockGuard&) = delete;

private:
    bool locked_ = false;
};

template <size_t N>
void copyText(char (&dst)[N], const char* src) {
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, N, "%s", src);
}

void updateLabelIfChanged(lv_obj_t* label, const char* text) {
    if (label == nullptr || text == nullptr) {
        return;
    }
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

[[noreturn]] void haltSystem(const char* message) {
    Serial.println(message);
    for (;;) {
        delay(1000);
    }
}

bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

template <size_t N>
void buildHttpUrl(char (&out)[N], const char* path) {
    snprintf(out, N, "http://%s:%u%s", backendHost, kBackendPort, path);
}

template <size_t N>
void buildToggleUrl(char (&out)[N], const char* device) {
    snprintf(out, N, "http://%s:%u%s%s", backendHost, kBackendPort, kTogglePath, device);
}

void startTimeSync() {
    configTzTime(kTimezone, kNtpServer, "time.google.com", "time.nist.gov");
    timeSyncStarted = true;

    const uint32_t waitStart = millis();
    while (time(nullptr) < 100000 && millis() - waitStart < 3000) {
        delay(100);
    }
}

void setSysMessage(const char* msg) {
    DataLockGuard lock;
    if (strncmp(sharedData.sysMessage, msg, sizeof(sharedData.sysMessage)) != 0) {
        copyText(sharedData.sysMessage, msg);
        sharedData.dirty = true;
    }
}

void updateLvglTick() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastLvglTickMs;
    lastLvglTickMs = now;
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
    }
}

bool takePendingDashboardData(DashboardData& out) {
    DataLockGuard lock;
    if (!sharedData.dirty) {
        return false;
    }
    out = sharedData;
    sharedData.dirty = false;
    return true;
}

void displayFlush(lv_disp_drv_t* dispDrv, const lv_area_t* area, lv_color_t* colorPtr) {
    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, width, height);
    tft.pushColors(reinterpret_cast<uint16_t*>(&colorPtr->full), width * height, true);
    tft.endWrite();

    lv_disp_flush_ready(dispDrv);
}

void touchpadRead(lv_indev_drv_t*, lv_indev_data_t* data) {
    static int32_t lastX = 0;
    static int32_t lastY = 0;
    static bool active = false;

    if (!touch.touched()) {
        data->point.x = static_cast<lv_coord_t>(lastX);
        data->point.y = static_cast<lv_coord_t>(lastY);
        data->state = LV_INDEV_STATE_REL;
        active = false;
        return;
    }

    lastTouchMs = millis();

    if (!isScreenOn) {
        analogWrite(kBacklightPin, kBrightnessMax);
        isScreenOn = true;
        data->state = LV_INDEV_STATE_REL;
        active = false;
        return;
    }

    const TS_Point p1 = touch.getPoint();
    const TS_Point p2 = touch.getPoint();

    const int32_t rawX = (static_cast<int32_t>(p1.x) + static_cast<int32_t>(p2.x)) / 2;
    const int32_t rawY = (static_cast<int32_t>(p1.y) + static_cast<int32_t>(p2.y)) / 2;

    int32_t mappedX = map(rawX, kTouchRawMinX, kTouchRawMaxX, 0, kScreenWidth - 1);
    int32_t mappedY = map(rawY, kTouchRawMinY, kTouchRawMaxY, 0, kScreenHeight - 1);

    mappedX = constrain(mappedX, 0, static_cast<int32_t>(kScreenWidth - 1));
    mappedY = constrain(mappedY, 0, static_cast<int32_t>(kScreenHeight - 1));

    if (!active) {
        lastX = mappedX;
        lastY = mappedY;
        active = true;
    } else {
        lastX = (lastX + mappedX * 3) / 4;
        lastY = (lastY + mappedY * 2) / 3;
    }

    data->point.x = static_cast<lv_coord_t>(lastX);
    data->point.y = static_cast<lv_coord_t>(lastY);
    data->state = LV_INDEV_STATE_PR;
}

void updateTimeLabelCb(lv_timer_t*) {
    if (labelTime == nullptr || labelDate == nullptr) {
        return;
    }

    const time_t now = time(nullptr);
    if (now < 100000 || !timeSyncStarted) {
        updateLabelIfChanged(labelTime, "--:--:--");
        updateLabelIfChanged(labelDate, "Syncing time...");
        return;
    }

    struct tm timeinfo{};
    localtime_r(&now, &timeinfo);

    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    updateLabelIfChanged(labelTime, timeStr);

    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%a, %d.%m.%Y", &timeinfo);
    updateLabelIfChanged(labelDate, dateStr);
}

void swKompEventCb(lv_event_t* event) {
    if (isApplyingSwitchState || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    DataLockGuard lock;
    reqToggleKomp = true;
    localKompTogglePending = true;
    localKompToggleDeadlineMs = millis() + kSwitchAckTimeoutMs;
}

void swSvitloEventCb(lv_event_t* event) {
    if (isApplyingSwitchState || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    DataLockGuard lock;
    reqToggleSvitlo = true;
    localSvitloTogglePending = true;
    localSvitloToggleDeadlineMs = millis() + kSwitchAckTimeoutMs;
}

void buildUi() {
    lv_obj_t* tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 40);

    lv_obj_t* sysLayer = lv_layer_sys();
    labelSysStatus = lv_label_create(sysLayer);
    lv_obj_set_style_text_font(labelSysStatus, &font_20, 0);
    lv_obj_set_style_text_color(labelSysStatus, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(labelSysStatus, "");
    lv_obj_align(labelSysStatus, LV_ALIGN_BOTTOM_RIGHT, -5, -5);

    lv_obj_t* content = lv_tabview_get_content(tabview);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_HOR);

    lv_obj_t* tabClock = lv_tabview_add_tab(tabview, "Clock");
    lv_obj_t* tabWeather = lv_tabview_add_tab(tabview, "Weather");
    lv_obj_t* tabFinance = lv_tabview_add_tab(tabview, "Finance");
    lv_obj_t* tabSmart = lv_tabview_add_tab(tabview, "Smart");

    lv_obj_clear_flag(tabClock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tabClock, 0, 0);
    lv_obj_clear_flag(tabWeather, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tabWeather, 0, 0);
    lv_obj_clear_flag(tabFinance, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tabSmart, LV_OBJ_FLAG_SCROLLABLE);

    labelTime = lv_label_create(tabClock);
    lv_obj_set_style_text_font(labelTime, &font_32, 0);
    lv_label_set_text(labelTime, "--:--:--");
    lv_obj_align(labelTime, LV_ALIGN_TOP_MID, 0, 55);

    labelDate = lv_label_create(tabClock);
    lv_obj_set_style_text_font(labelDate, &font_20, 0);
    lv_obj_set_style_text_color(labelDate, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(labelDate, "Syncing time...");
    lv_obj_align(labelDate, LV_ALIGN_TOP_MID, 0, 105);

    labelWeather = lv_label_create(tabWeather);
    lv_obj_set_style_text_font(labelWeather, &font_32, 0);
    lv_label_set_text(labelWeather, "Loading...");
    lv_obj_align(labelWeather, LV_ALIGN_TOP_MID, 0, 55);

    labelCity = lv_label_create(tabWeather);
    lv_obj_set_style_text_font(labelCity, &font_20, 0);
    lv_obj_set_style_text_color(labelCity, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(labelCity, "");
    lv_obj_align(labelCity, LV_ALIGN_TOP_MID, 0, 105);

    lv_obj_t* titleFuel = lv_label_create(tabFinance);
    lv_obj_set_style_text_font(titleFuel, &font_20, 0);
    lv_obj_set_style_text_color(titleFuel, lv_color_hex(0x888888), 0);
    lv_label_set_text(titleFuel, "FUEL (UAH)");
    lv_obj_align(titleFuel, LV_ALIGN_TOP_LEFT, 5, 10);

    labelFuel = lv_label_create(tabFinance);
    lv_obj_set_style_text_font(labelFuel, &font_20, 0);
    lv_obj_set_width(labelFuel, 140);
    lv_label_set_long_mode(labelFuel, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(labelFuel, titleFuel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_label_set_text(labelFuel, "Loading...");

    lv_obj_t* titleRates = lv_label_create(tabFinance);
    lv_obj_set_style_text_font(titleRates, &font_20, 0);
    lv_obj_set_style_text_color(titleRates, lv_color_hex(0x888888), 0);
    lv_label_set_text(titleRates, "RATES (UAH)");
    lv_obj_align(titleRates, LV_ALIGN_TOP_LEFT, 155, 10);

    labelRates = lv_label_create(tabFinance);
    lv_obj_set_style_text_font(labelRates, &font_20, 0);
    lv_obj_set_width(labelRates, 140);
    lv_label_set_long_mode(labelRates, LV_LABEL_LONG_WRAP);
    lv_obj_align_to(labelRates, titleRates, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 5);
    lv_label_set_text(labelRates, "Loading...");

    lblKomp = lv_label_create(tabSmart);
    lv_obj_set_style_text_font(lblKomp, &font_20, 0);
    lv_label_set_text(lblKomp, "PC Power");
    lv_obj_align(lblKomp, LV_ALIGN_TOP_LEFT, 20, 40);

    swKomp = lv_switch_create(tabSmart);
    lv_obj_set_size(swKomp, 80, 40);
    lv_obj_set_ext_click_area(swKomp, 20);
    lv_obj_clear_flag(swKomp, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_align(swKomp, LV_ALIGN_TOP_RIGHT, -20, 30);
    lv_obj_add_event_cb(swKomp, swKompEventCb, LV_EVENT_VALUE_CHANGED, nullptr);

    lblSvitlo = lv_label_create(tabSmart);
    lv_obj_set_style_text_font(lblSvitlo, &font_20, 0);
    lv_label_set_text(lblSvitlo, "Light");
    lv_obj_align(lblSvitlo, LV_ALIGN_TOP_LEFT, 20, 110);

    swSvitlo = lv_switch_create(tabSmart);
    lv_obj_set_size(swSvitlo, 80, 40);
    lv_obj_set_ext_click_area(swSvitlo, 20);
    lv_obj_clear_flag(swSvitlo, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_align(swSvitlo, LV_ALIGN_TOP_RIGHT, -20, 100);
    lv_obj_add_event_cb(swSvitlo, swSvitloEventCb, LV_EVENT_VALUE_CHANGED, nullptr);

    lblSmartError = lv_label_create(tabSmart);
    lv_obj_set_style_text_font(lblSmartError, &font_20, 0);
    lv_obj_set_style_text_color(lblSmartError, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(lblSmartError, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lblSmartError, "Connecting...");
    lv_obj_center(lblSmartError);
    lv_obj_add_flag(lblSmartError, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(updateTimeLabelCb, 1000, nullptr);
}

void applyPendingUiUpdateIfAny() {
    DashboardData localCopy{};
    if (!takePendingDashboardData(localCopy)) {
        return;
    }

    if (localCopy.needWifiSetup) {
        if (wifiSetupOverlay == nullptr) {
            wifiSetupOverlay = lv_obj_create(lv_scr_act());
            lv_obj_set_size(wifiSetupOverlay, kScreenWidth, kScreenHeight);
            lv_obj_set_style_bg_color(wifiSetupOverlay, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(wifiSetupOverlay, LV_OPA_COVER, 0);
            lv_obj_clear_flag(wifiSetupOverlay, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* label = lv_label_create(wifiSetupOverlay);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(label, &font_20, 0);
            lv_label_set_recolor(label, true);
            lv_label_set_text(
                label,
                "Wi-Fi Setup Required\n\n1. Connect to AP:\n#FFFF00 Smart_Setup#\n\n2. Open browser:\n#FFFF00 192.168.4.1#"
            );
            lv_obj_center(label);
        }
    } else if (wifiSetupOverlay != nullptr) {
        lv_obj_del(wifiSetupOverlay);
        wifiSetupOverlay = nullptr;
    }

    if (labelSysStatus != nullptr) {
        updateLabelIfChanged(labelSysStatus, localCopy.sysMessage);
    }

    char ratesText[64];
    snprintf(ratesText, sizeof(ratesText), "USD: %s\nEUR: %s", localCopy.usd, localCopy.eur);

    if (labelWeather != nullptr && labelCity != nullptr) {
        char weatherBuffer[64];
        copyText(weatherBuffer, localCopy.weather);
        char* newline = strchr(weatherBuffer, '\n');

        if (newline != nullptr) {
            *newline = '\0';
            updateLabelIfChanged(labelCity, weatherBuffer);
            updateLabelIfChanged(labelWeather, newline + 1);
        } else {
            updateLabelIfChanged(labelWeather, weatherBuffer);
            updateLabelIfChanged(labelCity, "");
        }
    }

    if (labelFuel != nullptr) {
        updateLabelIfChanged(labelFuel, localCopy.fuel);
    }
    if (labelRates != nullptr) {
        updateLabelIfChanged(labelRates, ratesText);
    }

    if (lblKomp != nullptr && swKomp != nullptr && lblSvitlo != nullptr && swSvitlo != nullptr && lblSmartError != nullptr) {
        if (localCopy.isSmartReady) {
            lv_obj_clear_flag(lblKomp, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(swKomp, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(lblSvitlo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(swSvitlo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lblSmartError, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(lblKomp, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(swKomp, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lblSvitlo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(swSvitlo, LV_OBJ_FLAG_HIDDEN);
            updateLabelIfChanged(lblSmartError, localCopy.smartErrorMsg);
            lv_obj_clear_flag(lblSmartError, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const uint32_t now = millis();
    isApplyingSwitchState = true;

    if (swKomp != nullptr) {
        const bool current = lv_obj_has_state(swKomp, LV_STATE_CHECKED);
        if (localKompTogglePending &&
            (localCopy.kompOn == current || static_cast<int32_t>(now - localKompToggleDeadlineMs) >= 0)) {
            localKompTogglePending = false;
        }
        if (!localKompTogglePending && current != localCopy.kompOn) {
            if (localCopy.kompOn) {
                lv_obj_add_state(swKomp, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(swKomp, LV_STATE_CHECKED);
            }
        }
    }

    if (swSvitlo != nullptr) {
        const bool current = lv_obj_has_state(swSvitlo, LV_STATE_CHECKED);
        if (localSvitloTogglePending &&
            (localCopy.svitloOn == current || static_cast<int32_t>(now - localSvitloToggleDeadlineMs) >= 0)) {
            localSvitloTogglePending = false;
        }
        if (!localSvitloTogglePending && current != localCopy.svitloOn) {
            if (localCopy.svitloOn) {
                lv_obj_add_state(swSvitlo, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(swSvitlo, LV_STATE_CHECKED);
            }
        }
    }

    isApplyingSwitchState = false;
}

void pumpUiAndPendingData() {
    updateLvglTick();
    applyPendingUiUpdateIfAny();
    lv_timer_handler();
}

void parseDashboardJson(JsonDocument& doc) {
    DataLockGuard lock;

    if (doc.containsKey("weather")) {
        copyText(sharedData.weather, doc["weather"]);
    }
    if (doc.containsKey("usd")) {
        copyText(sharedData.usd, doc["usd"]);
    }
    if (doc.containsKey("eur")) {
        copyText(sharedData.eur, doc["eur"]);
    }
    if (doc.containsKey("fuel")) {
        copyText(sharedData.fuel, doc["fuel"]);
    }
    if (doc.containsKey("status")) {
        copyText(sharedData.status, doc["status"]);
    }

    if (doc.containsKey("smarthome")) {
        sharedData.kompOn = doc["smarthome"]["komp_iuter"] | false;
        sharedData.svitloOn = doc["smarthome"]["svitlo"] | false;
    }

    sharedData.dirty = true;
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    if (strcmp(topic, kMqttTopicHeartbeat) == 0) {
        StaticJsonDocument<256> doc;
        if (!deserializeJson(doc, payload, length)) {
            DataLockGuard lock;
            sharedData.lastHeartbeatMs = millis();
            const char* haStatus = doc["ha_status"] | "Online";
            sharedData.haOffline = (strcmp(haStatus, "HA_Offline") == 0);
            sharedData.dirty = true;
        }
        return;
    }

    StaticJsonDocument<kMqttJsonCapacity> doc;
    if (!deserializeJson(doc, payload, length)) {
        parseDashboardJson(doc);
    }
}

void setupMqtt() {
    mqttClient.setServer(backendHost, kMqttPort);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(kMqttJsonCapacity);
    mqttClient.setKeepAlive(15);
    mqttClient.setSocketTimeout(2);
}

bool connectMqtt() {
    if (!isWiFiConnected() || strlen(backendHost) == 0) {
        return false;
    }
    if (mqttClient.connected()) {
        return true;
    }
    if (!mqttClient.connect(kMqttClientId)) {
        return false;
    }

    mqttClient.subscribe(kMqttTopicState);
    mqttClient.subscribe(kMqttTopicHeartbeat);
    return true;
}

void sendTelemetry() {
    if (!mqttClient.connected()) {
        return;
    }

    StaticJsonDocument<256> doc;
    doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = millis() / 1000;
    doc["free_heap"] = ESP.getFreeHeap();

    char buffer[256];
    const size_t n = serializeJson(doc, buffer);
    mqttClient.publish(kMqttTopicTelemetry, buffer, n);
}

bool fetchDataFromServer() {
    if (!isWiFiConnected() || strlen(backendHost) == 0) {
        return false;
    }

    char url[128];
    buildHttpUrl(url, kDashboardPath);

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, url)) {
        return false;
    }

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    StaticJsonDocument<kDashboardJsonCapacity> doc;
    if (!deserializeJson(doc, http.getStream())) {
        parseDashboardJson(doc);
    }

    http.end();
    return true;
}

bool sendToggleCommand(const char* device) {
    if (!isWiFiConnected() || strlen(backendHost) == 0) {
        return false;
    }

    char url[160];
    buildToggleUrl(url, device);

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, url)) {
        return false;
    }

    const int code = http.POST("");
    http.end();
    return code == HTTP_CODE_OK;
}

void saveCurrentNetwork(const String& ssid, const String& pass) {
    if (ssid.isEmpty()) {
        return;
    }

    Preferences prefs;
    prefs.begin("wifi_cfg", false);

    int count = prefs.getInt("count", 0);
    for (int i = 0; i < count; i++) {
        if (prefs.getString(("s" + String(i)).c_str(), "") == ssid) {
            prefs.putString(("p" + String(i)).c_str(), pass);
            prefs.end();
            return;
        }
    }

    if (count < 3) {
        prefs.putString(("s" + String(count)).c_str(), ssid);
        prefs.putString(("p" + String(count)).c_str(), pass);
        prefs.putInt("count", count + 1);
    } else {
        const String s1 = prefs.getString("s1", "");
        const String p1 = prefs.getString("p1", "");
        const String s2 = prefs.getString("s2", "");
        const String p2 = prefs.getString("p2", "");

        prefs.putString("s0", s1);
        prefs.putString("p0", p1);
        prefs.putString("s1", s2);
        prefs.putString("p1", p2);
        prefs.putString("s2", ssid);
        prefs.putString("p2", pass);
    }

    prefs.end();
}

bool trySavedNetworks() {
    Preferences prefs;
    prefs.begin("wifi_cfg", true);

    const int count = prefs.getInt("count", 0);
    if (count == 0) {
        prefs.end();
        return false;
    }

    String ssids[3];
    String passes[3];

    for (int i = 0; i < count; i++) {
        ssids[i] = prefs.getString(("s" + String(i)).c_str(), "");
        passes[i] = prefs.getString(("p" + String(i)).c_str(), "");
    }

    prefs.end();

    WiFi.mode(WIFI_STA);
    const int n = WiFi.scanNetworks();

    int bestIdx = -1;
    int32_t bestRssi = -1000;

    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < count; j++) {
            if (WiFi.SSID(i) == ssids[j] && WiFi.RSSI(i) > bestRssi) {
                bestRssi = WiFi.RSSI(i);
                bestIdx = j;
            }
        }
    }

    WiFi.scanDelete();

    if (bestIdx == -1) {
        return false;
    }

    WiFi.begin(ssids[bestIdx].c_str(), passes[bestIdx].c_str());

    const uint32_t startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (WiFi.status() == WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        return true;
    }

    return false;
}

bool discoverBackendServerTask() {
    WiFiUDP udp;
    if (!udp.begin(kUdpPort)) {
        return false;
    }

    const uint32_t start = millis();
    while (millis() - start < 5000) {
        udp.beginPacket(IPAddress(255, 255, 255, 255), kUdpPort);
        udp.print(kDiscoveryMsg);
        udp.endPacket();

        const uint32_t waitTime = millis();
        while (millis() - waitTime < 1000) {
            const int packetSize = udp.parsePacket();
            if (packetSize > 0) {
                char incomingPacket[255];
                const int len = udp.read(incomingPacket, sizeof(incomingPacket) - 1);
                if (len > 0) {
                    incomingPacket[len] = '\0';
                }

                if (strncmp(incomingPacket, "BACKEND_HERE", 12) == 0) {
                    strncpy(backendHost, udp.remoteIP().toString().c_str(), sizeof(backendHost) - 1);
                    backendHost[sizeof(backendHost) - 1] = '\0';
                    udp.stop();
                    return true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    udp.stop();
    return false;
}

[[noreturn]] void runNetworkLoop() {
    WiFiManager wm;
    wm.setConnectTimeout(10);
    wm.setConfigPortalTimeout(180);

    wm.setAPCallback([](WiFiManager*) {
        DataLockGuard lock;
        sharedData.needWifiSetup = true;
        sharedData.dirty = true;
    });

    if (!trySavedNetworks()) {
        if (wm.startConfigPortal("Smart_Setup")) {
            saveCurrentNetwork(WiFi.SSID(), WiFi.psk());
            vTaskDelay(pdMS_TO_TICKS(1500));
            startTimeSync();
        }
    } else {
        startTimeSync();
    }

    {
        DataLockGuard lock;
        sharedData.needWifiSetup = false;
        sharedData.dirty = true;
    }

    uint32_t currentPollInterval = kDashboardPollIntervalMs;
    uint32_t lastDashboardPollMs = 0;
    uint32_t lastTelemetryMs = 0;

    int mqttFails = 0;
    int httpFails = 0;
    bool wasWifiConnected = isWiFiConnected();
    uint32_t currentReconnectDelay = kReconnectDelayMs;
    bool wasMqttOk = false;

    for (;;) {
        if (!isWiFiConnected()) {
            {
                DataLockGuard lock;
                sharedData.isSmartReady = false;
                copyText(sharedData.smartErrorMsg, "No Wi-Fi\nControls disabled");
                sharedData.dirty = true;
            }

            setSysMessage("No Wi-Fi");

            if (trySavedNetworks()) {
                currentReconnectDelay = kReconnectDelayMs;
            } else {
                vTaskDelay(pdMS_TO_TICKS(currentReconnectDelay));
                currentReconnectDelay = (currentReconnectDelay * 2 > 60000UL) ? 60000UL : currentReconnectDelay * 2;
            }

            continue;
        }

        if (!wasWifiConnected) {
            startTimeSync();
            wasWifiConnected = true;
            currentReconnectDelay = kReconnectDelayMs;
        }

        if (strlen(backendHost) == 0) {
            {
                DataLockGuard lock;
                sharedData.isSmartReady = false;
                copyText(sharedData.smartErrorMsg, "Searching Server...\nControls disabled");
                sharedData.dirty = true;
            }

            setSysMessage("Search Srv");

            if (discoverBackendServerTask()) {
                setupMqtt();
                mqttFails = 0;
                httpFails = 0;
                setSysMessage("");
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        if (millis() - lastDashboardPollMs >= currentPollInterval || lastDashboardPollMs == 0) {
            if (fetchDataFromServer()) {
                httpFails = 0;
                currentPollInterval = kDashboardPollIntervalMs;
                if (time(nullptr) < 100000) {
                    startTimeSync();
                }
            } else {
                httpFails++;
                currentPollInterval = 10000UL;
            }
            lastDashboardPollMs = millis();
        }

        const bool mqttOk = connectMqtt();
        if (mqttOk) {
            if (!wasMqttOk) {
                DataLockGuard lock;
                sharedData.lastHeartbeatMs = millis();
            }
            wasMqttOk = true;
            mqttFails = 0;
            mqttClient.loop();
        } else {
            wasMqttOk = false;
            mqttFails++;
        }

        bool isReady = false;
        char errMsg[64] = "";
        char sysMsg[16] = "";

        bool toggleKomp = false;
        bool toggleSvitlo = false;

        {
            DataLockGuard lock;

            bool backendDead = false;
            if (sharedData.lastHeartbeatMs > 0 && (millis() - sharedData.lastHeartbeatMs > 12000UL)) {
                backendDead = true;
            }

            if (httpFails >= 2 && mqttFails >= 2) {
                copyText(errMsg, "Server Disconnected\nControls disabled");
                copyText(sysMsg, "Search Srv");
                if (httpFails >= 5) {
                    backendHost[0] = '\0';
                }
            } else if (mqttFails >= 2) {
                copyText(errMsg, "MQTT Offline\nControls disabled");
                copyText(sysMsg, "No MQTT");
            } else if (backendDead) {
                copyText(errMsg, "Backend Offline\nControls disabled");
                copyText(sysMsg, "Backend Err");
                if (millis() - sharedData.lastHeartbeatMs > 30000UL) {
                    backendHost[0] = '\0';
                }
            } else if (sharedData.haOffline) {
                copyText(errMsg, "Home Assistant Offline\nControls disabled");
                copyText(sysMsg, "HA Error");
            } else if (httpFails >= 2) {
                copyText(errMsg, "Backend API Offline\nControls disabled");
                copyText(sysMsg, "API Error");
            } else {
                isReady = true;
                copyText(sysMsg, "");
            }

            if (sharedData.isSmartReady != isReady || strcmp(sharedData.smartErrorMsg, errMsg) != 0) {
                sharedData.isSmartReady = isReady;
                copyText(sharedData.smartErrorMsg, errMsg);
                sharedData.dirty = true;
            }

            toggleKomp = reqToggleKomp;
            toggleSvitlo = reqToggleSvitlo;
            reqToggleKomp = false;
            reqToggleSvitlo = false;
        }

        if (isReady) {
            if (toggleKomp) {
                sendToggleCommand(kDeviceKomp);
            }
            if (toggleSvitlo) {
                sendToggleCommand(kDeviceSvitlo);
            }
        }

        setSysMessage(sysMsg);

        if (millis() - lastTelemetryMs >= kTelemetryIntervalMs || lastTelemetryMs == 0) {
            if (mqttOk) {
                sendTelemetry();
            }
            lastTelemetryMs = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(kNetworkLoopDelayMs));
    }
}

void networkTask(void*) {
    runNetworkLoop();
}

void setupDisplayAndTouch() {
    pinMode(kBacklightPin, OUTPUT);
    analogWrite(kBacklightPin, kBrightnessMax);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    touchSpi.begin(kTouchClk, kTouchMiso, kTouchMosi, kTouchCs);
    touch.begin(touchSpi);
    touch.setRotation(1);
}

void setupLvgl() {
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer, drawBufferPixels, nullptr, kDrawBufferSize);

    static lv_disp_drv_t dispDrv;
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = kScreenWidth;
    dispDrv.ver_res = kScreenHeight;
    dispDrv.flush_cb = displayFlush;
    dispDrv.draw_buf = &drawBuffer;
    lv_disp_drv_register(&dispDrv);

    static lv_indev_drv_t indevDrv;
    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchpadRead;
    indevDrv.scroll_limit = 2;
    indevDrv.scroll_throw = 28;
    lv_indev_drv_register(&indevDrv);
}

}  // namespace

void setupDataMutex() {
    dataMutex = xSemaphoreCreateMutex();
    if (dataMutex == nullptr) {
        haltSystem("Fatal: data mutex failed");
    }
}

void setupNetworkTask() {
    const BaseType_t result = xTaskCreatePinnedToCore(
        networkTask,
        "NetworkTask",
        kNetworkTaskStackSize,
        nullptr,
        kNetworkTaskPriority,
        &networkTaskHandle,
        0
    );

    if (result != pdPASS || networkTaskHandle == nullptr) {
        haltSystem("Fatal: network task failed");
    }
}

void setup() {
    Serial.begin(115200);

    setupDataMutex();
    setupDisplayAndTouch();
    setupLvgl();
    buildUi();

    lastLvglTickMs = millis();
    lastTouchMs = millis();

    setupNetworkTask();

    esp_task_wdt_init(kWatchdogTimeoutS, true);
    esp_task_wdt_add(nullptr);
}

void loop() {
    esp_task_wdt_reset();

    if (isScreenOn && millis() - lastTouchMs > kScreenSaverTimeoutMs) {
        analogWrite(kBacklightPin, kBrightnessDim);
        isScreenOn = false;
    }

    pumpUiAndPendingData();
    delay(kUiLoopDelayMs);
}