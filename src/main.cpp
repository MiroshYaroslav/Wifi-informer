#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctime>

#include "secrets.h"

LV_FONT_DECLARE(font_20);
LV_FONT_DECLARE(font_32);

namespace {
constexpr char kServerUrl[] = "http://192.168.0.103:8000/api/dashboard";

constexpr char kNtpServer[] = "pool.ntp.org";
constexpr char kTimezone[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";

constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;

constexpr uint32_t kUpdateIntervalMs = 5 * 60 * 1000;
constexpr uint32_t kWifiConnectTimeoutMs = 10000;
constexpr uint16_t kHttpTimeoutMs = 2500;
constexpr uint16_t kUiLoopDelayMs = 1;
constexpr uint32_t kReconnectDelayMs = 5000;

constexpr uint8_t kTouchIrq = 36;
constexpr uint8_t kTouchMosi = 32;
constexpr uint8_t kTouchMiso = 39;
constexpr uint8_t kTouchClk = 25;
constexpr uint8_t kTouchCs = 33;

constexpr int16_t kTouchRawMinX = 200;
constexpr int16_t kTouchRawMaxX = 3800;
constexpr int16_t kTouchRawMinY = 200;
constexpr int16_t kTouchRawMaxY = 3800;

constexpr bool kTouchSwapXY = false;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;

constexpr uint16_t kDrawBufferLines = 25;
constexpr size_t kDrawBufferSize = kScreenWidth * kDrawBufferLines;

constexpr size_t kJsonCapacity = JSON_OBJECT_SIZE(5) + 256;

constexpr uint32_t kNetworkTaskStackSize = 6144;
constexpr UBaseType_t kNetworkTaskPriority = 1;

TFT_eSPI tft;
SPIClass touchSpi(HSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);

lv_disp_draw_buf_t drawBuffer{};
lv_color_t drawBufferPixels[kDrawBufferSize];

lv_obj_t* labelTime = nullptr;
lv_obj_t* labelDate = nullptr;
lv_obj_t* labelWeather = nullptr;
lv_obj_t* labelCity = nullptr;
lv_obj_t* labelFuel = nullptr;
lv_obj_t* labelRates = nullptr;
lv_obj_t* labelDev = nullptr;

SemaphoreHandle_t dataMutex = nullptr;
TaskHandle_t networkTaskHandle = nullptr;

uint32_t lastLvglTickMs = 0;
bool timeSyncStarted = false;

struct DashboardData {
    char weather[64];
    char usd[16];
    char eur[16];
    char fuel[96];
    char status[24];
    bool dirty;
};

DashboardData sharedData{
    "Loading weather...",
    "N/A",
    "N/A",
    "Loading fuel...",
    "starting...",
    false
};
}

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

[[noreturn]] void haltSystem(const char* message) {
    Serial.println(message);
    for (;;) {
        delay(1000);
    }
}

bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void startTimeSync() {
    configTzTime(kTimezone, kNtpServer);
    timeSyncStarted = true;
}

void startWiFiStation() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.begin(kSsid, kPassword);
}

void reconnectWiFi() {
    if (!isWiFiConnected()) {
        WiFi.reconnect();
    }
}

void setPendingStatus(const char* statusText) {
    DataLockGuard lock;
    copyText(sharedData.status, statusText);
    sharedData.dirty = true;
}

void setPendingDashboardData(
    const char* weatherText,
    const char* usdText,
    const char* eurText,
    const char* fuelText,
    const char* statusText
) {
    DataLockGuard lock;
    copyText(sharedData.weather, weatherText);
    copyText(sharedData.usd, usdText);
    copyText(sharedData.eur, eurText);
    copyText(sharedData.fuel, fuelText);
    copyText(sharedData.status, statusText);
    sharedData.dirty = true;
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

void updateLvglTick() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastLvglTickMs;
    lastLvglTickMs = now;

    if (elapsed > 0) {
        lv_tick_inc(elapsed);
    }
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

    const TS_Point p1 = touch.getPoint();
    const TS_Point p2 = touch.getPoint();

    const int32_t rawX = (static_cast<int32_t>(p1.x) + static_cast<int32_t>(p2.x)) / 2;
    const int32_t rawY = (static_cast<int32_t>(p1.y) + static_cast<int32_t>(p2.y)) / 2;

    const int32_t srcX = kTouchSwapXY ? rawY : rawX;
    const int32_t srcY = kTouchSwapXY ? rawX : rawY;

    constexpr int32_t kSrcXMin = kTouchSwapXY ? kTouchRawMinY : kTouchRawMinX;
    constexpr int32_t kSrcXMax = kTouchSwapXY ? kTouchRawMaxY : kTouchRawMaxX;
    constexpr int32_t kSrcYMin = kTouchSwapXY ? kTouchRawMinX : kTouchRawMinY;
    constexpr int32_t kSrcYMax = kTouchSwapXY ? kTouchRawMaxX : kTouchRawMaxY;

    int32_t mappedX = map(srcX, kSrcXMin, kSrcXMax, 0, kScreenWidth - 1);
    int32_t mappedY = map(srcY, kSrcYMin, kSrcYMax, 0, kScreenHeight - 1);

    if (kTouchInvertX) {
        mappedX = (kScreenWidth - 1) - mappedX;
    }
    if (kTouchInvertY) {
        mappedY = (kScreenHeight - 1) - mappedY;
    }

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
        lv_label_set_text(labelTime, "--:--:--");
        lv_label_set_text(labelDate, "Syncing time...");
        return;
    }

    struct tm timeinfo{};
    localtime_r(&now, &timeinfo);

    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    lv_label_set_text(labelTime, timeStr);

    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%a, %d.%m.%Y", &timeinfo);
    lv_label_set_text(labelDate, dateStr);
}

void buildUi() {
    lv_obj_t* tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 40);

    lv_obj_t* content = lv_tabview_get_content(tabview);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_HOR);

    lv_obj_t* tabClock = lv_tabview_add_tab(tabview, "Clock");
    lv_obj_t* tabWeather = lv_tabview_add_tab(tabview, "Weather");
    lv_obj_t* tabFinance = lv_tabview_add_tab(tabview, "Finance");
    lv_obj_t* tabDev = lv_tabview_add_tab(tabview, "Dev");

    lv_obj_clear_flag(tabClock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tabWeather, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tabFinance, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tabDev, LV_OBJ_FLAG_SCROLLABLE);

    labelTime = lv_label_create(tabClock);
    lv_obj_set_width(labelTime, LV_PCT(100));
    lv_obj_set_style_text_align(labelTime, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(labelTime, &font_32, 0);
    lv_label_set_text(labelTime, "--:--:--");
    lv_obj_align(labelTime, LV_ALIGN_TOP_MID, 0, 55);

    labelDate = lv_label_create(tabClock);
    lv_obj_set_width(labelDate, LV_PCT(100));
    lv_obj_set_style_text_align(labelDate, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(labelDate, &font_20, 0);
    lv_obj_set_style_text_color(labelDate, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(labelDate, "Syncing time...");
    lv_obj_align(labelDate, LV_ALIGN_TOP_MID, 0, 105);

    labelWeather = lv_label_create(tabWeather);
    lv_obj_set_width(labelWeather, LV_PCT(100));
    lv_obj_set_style_text_align(labelWeather, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(labelWeather, &font_32, 0);
    lv_label_set_text(labelWeather, "Loading...");
    lv_obj_align(labelWeather, LV_ALIGN_TOP_MID, 0, 55);

    labelCity = lv_label_create(tabWeather);
    lv_obj_set_width(labelCity, LV_PCT(100));
    lv_obj_set_style_text_align(labelCity, LV_TEXT_ALIGN_CENTER, 0);
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

    labelDev = lv_label_create(tabDev);
    lv_label_set_text(labelDev, "System: starting...");
    lv_obj_align(labelDev, LV_ALIGN_CENTER, 0, 0);

    lv_timer_create(updateTimeLabelCb, 1000, nullptr);
}

void applyPendingUiUpdateIfAny() {
    DashboardData localCopy{};
    if (!takePendingDashboardData(localCopy)) {
        return;
    }

    char ratesText[64];
    char devText[40];

    snprintf(ratesText, sizeof(ratesText), "USD: %s\nEUR: %s", localCopy.usd, localCopy.eur);
    snprintf(devText, sizeof(devText), "Server: %s", localCopy.status);

    if (labelWeather != nullptr && labelCity != nullptr) {
        char weatherBuffer[64];
        copyText(weatherBuffer, localCopy.weather);

        char* newline = strchr(weatherBuffer, '\n');
        if (newline != nullptr) {
            *newline = '\0';
            lv_label_set_text(labelCity, weatherBuffer);
            lv_label_set_text(labelWeather, newline + 1);
        } else {
            lv_label_set_text(labelWeather, weatherBuffer);
            lv_label_set_text(labelCity, "");
        }
    }

    if (labelFuel != nullptr) {
        lv_label_set_text(labelFuel, localCopy.fuel);
    }
    if (labelRates != nullptr) {
        lv_label_set_text(labelRates, ratesText);
    }
    if (labelDev != nullptr) {
        lv_label_set_text(labelDev, devText);
    }
}

void pumpUiOnce() {
    updateLvglTick();
    applyPendingUiUpdateIfAny();
    lv_timer_handler();
}

void connectToWiFi() {
    if (isWiFiConnected()) {
        return;
    }

    startWiFiStation();
    setPendingStatus("connecting...");

    const uint32_t startMs = millis();

    while (!isWiFiConnected() && (millis() - startMs) < kWifiConnectTimeoutMs) {
        pumpUiOnce();
        delay(kUiLoopDelayMs);
    }

    if (isWiFiConnected()) {
        Serial.println("WiFi connected");
        setPendingStatus("waiting...");
        startTimeSync();
        Serial.println("NTP time sync started");
    } else {
        Serial.println("WiFi connection failed");
        setPendingStatus("WiFi failed");
    }
}

void fetchDataFromServer() {
    if (!isWiFiConnected()) {
        setPendingStatus("WiFi offline");
        return;
    }

    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, kServerUrl)) {
        Serial.println("HTTP begin failed");
        setPendingStatus("HTTP init fail");
        return;
    }

    const int httpResponseCode = http.GET();

    if (httpResponseCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", httpResponseCode);
        setPendingStatus("server offline");
        http.end();
        return;
    }

    StaticJsonDocument<kJsonCapacity> doc;
    const DeserializationError error = deserializeJson(doc, http.getStream());
    http.end();

    if (error) {
        Serial.print("JSON parse failed: ");
        Serial.println(error.c_str());
        setPendingStatus("JSON error");
        return;
    }

    const char* weatherText = doc["weather"] | "N/A";
    const char* usdText = doc["usd"] | "N/A";
    const char* eurText = doc["eur"] | "N/A";
    const char* fuelText = doc["fuel"] | "N/A";
    const char* serverStatusText = doc["status"] | "Online";

    setPendingDashboardData(weatherText, usdText, eurText, fuelText, serverStatusText);
}

[[noreturn]] void runNetworkLoop() {
    bool wasConnected = isWiFiConnected();

    for (;;) {
        const bool connected = isWiFiConnected();

        if (!connected) {
            reconnectWiFi();
            setPendingStatus("reconnecting...");
            wasConnected = false;
            vTaskDelay(pdMS_TO_TICKS(kReconnectDelayMs));
            continue;
        }

        if (!wasConnected) {
            startTimeSync();
            wasConnected = true;
        }

        fetchDataFromServer();
        vTaskDelay(pdMS_TO_TICKS(kUpdateIntervalMs));
    }
}

void networkTask(void*) {
    runNetworkLoop();
}

void setupDisplayAndTouch() {
#ifdef TFT_BL
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif

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

    connectToWiFi();
    setupNetworkTask();
}

void loop() {
    pumpUiOnce();
    delay(kUiLoopDelayMs);
}