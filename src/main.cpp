#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>

namespace {
#include "secrets.h"
constexpr char kServerUrl[] = "http://192.168.1.100:8000/api/dashboard";

constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;

constexpr uint32_t kUpdateIntervalMs = 60000;
constexpr uint32_t kWifiConnectTimeoutMs = 10000;
constexpr uint16_t kHttpTimeoutMs = 3000;
constexpr uint16_t kLoopDelayMs = 5;

constexpr uint8_t kTouchIrq = 36;
constexpr uint8_t kTouchMosi = 32;
constexpr uint8_t kTouchMiso = 39;
constexpr uint8_t kTouchClk = 25;
constexpr uint8_t kTouchCs = 33;

constexpr int16_t kTouchMinX = 200;
constexpr int16_t kTouchMaxX = 3800;
constexpr int16_t kTouchMinY = 200;
constexpr int16_t kTouchMaxY = 3800;

constexpr size_t kDrawBufferSize = kScreenWidth * kScreenHeight / 10;
constexpr size_t kJsonCapacity = JSON_OBJECT_SIZE(4) + 128;

TFT_eSPI tft;
SPIClass touchSpi(HSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);

lv_disp_draw_buf_t drawBuffer;
lv_color_t drawBufferPixels[kDrawBufferSize];

lv_obj_t* labelTime = nullptr;
lv_obj_t* labelWeather = nullptr;
lv_obj_t* labelFinance = nullptr;
lv_obj_t* labelDev = nullptr;

uint32_t lastDataUpdateMs = 0;
uint32_t lastLvglTickMs = 0;
}  // namespace

void setLabelText(lv_obj_t* label, const char* text) {
    if (label != nullptr) {
        lv_label_set_text(label, text);
    }
}

bool isWiFiConnected() {
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    return WiFi.status() == WL_CONNECTED;
}

void startWiFiStation() {
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    WiFi.mode(WIFI_STA);
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    WiFi.begin(kSsid, kPassword);
}

void reconnectWiFi() {
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    WiFi.reconnect();
}

void updateLvglTick() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastLvglTickMs;
    lastLvglTickMs = now;

    if (elapsed > 0) {
        lv_tick_inc(elapsed);
    }

    lv_timer_handler();
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
    if (!touch.tirqTouched() || !touch.touched()) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    const TS_Point point = touch.getPoint();

    const int32_t mappedX = map(point.y, kTouchMinY, kTouchMaxY, 0, kScreenWidth - 1);
    const int32_t mappedY = map(point.x, kTouchMinX, kTouchMaxX, 0, kScreenHeight - 1);

    data->point.x = static_cast<lv_coord_t>(
        constrain(mappedX, 0, static_cast<int32_t>(kScreenWidth - 1)));
    data->point.y = static_cast<lv_coord_t>(
        constrain(mappedY, 0, static_cast<int32_t>(kScreenHeight - 1)));
    data->state = LV_INDEV_STATE_PR;
}

void buildUi() {
    lv_obj_t* tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 40);

    lv_obj_t* tabClock = lv_tabview_add_tab(tabview, "Clock");
    lv_obj_t* tabWeather = lv_tabview_add_tab(tabview, "Weather");
    lv_obj_t* tabFinance = lv_tabview_add_tab(tabview, "Finance");
    lv_obj_t* tabDev = lv_tabview_add_tab(tabview, "Dev");

    labelTime = lv_label_create(tabClock);
    lv_label_set_text(labelTime, "--:--");
    lv_obj_align(labelTime, LV_ALIGN_CENTER, 0, 0);

    labelWeather = lv_label_create(tabWeather);
    lv_label_set_text(labelWeather, "Loading weather...");
    lv_obj_align(labelWeather, LV_ALIGN_CENTER, 0, 0);

    labelFinance = lv_label_create(tabFinance);
    lv_label_set_text(labelFinance, "Loading rates...");
    lv_obj_align(labelFinance, LV_ALIGN_CENTER, 0, 0);

    labelDev = lv_label_create(tabDev);
    lv_label_set_text(labelDev, "System: starting...");
    lv_obj_align(labelDev, LV_ALIGN_CENTER, 0, 0);
}

void connectToWiFi() {
    if (isWiFiConnected()) {
        return;
    }

    startWiFiStation();
    setLabelText(labelDev, "WiFi: connecting...");

    const uint32_t startMs = millis();

    while (!isWiFiConnected() && (millis() - startMs) < kWifiConnectTimeoutMs) {
        updateLvglTick();
        delay(kLoopDelayMs);
    }

    if (isWiFiConnected()) {
        Serial.println("WiFi connected");
        setLabelText(labelDev, "Server: waiting...");
    } else {
        Serial.println("WiFi connection failed");
        setLabelText(labelDev, "WiFi: failed");
    }
}

void fetchDataFromServer() {
    if (!isWiFiConnected()) {
        setLabelText(labelDev, "WiFi: offline");
        return;
    }

    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(kServerUrl)) {
        Serial.println("HTTP begin failed");
        setLabelText(labelDev, "HTTP: init failed");
        return;
    }

    const int httpResponseCode = http.GET();

    if (httpResponseCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", httpResponseCode);
        setLabelText(labelDev, "Server: offline");
        http.end();
        return;
    }

    StaticJsonDocument<kJsonCapacity> doc;
    const DeserializationError error = deserializeJson(doc, http.getStream());

    if (error) {
        Serial.print("JSON parse failed: ");
        Serial.println(error.c_str());
        setLabelText(labelDev, "JSON: parse error");
        http.end();
        return;
    }

    const char* timeText = doc["time"] | "--:--";
    const char* weatherText = doc["weather"] | "N/A";
    const char* usdText = doc["usd"] | "N/A";
    const char* serverStatusText = doc["status"] | "Online";

    String financeText = "USD: ";
    financeText += usdText;

    String devText = "Server: ";
    devText += serverStatusText;

    setLabelText(labelTime, timeText);
    setLabelText(labelWeather, weatherText);
    setLabelText(labelFinance, financeText.c_str());
    setLabelText(labelDev, devText.c_str());

    http.end();
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
    lv_indev_drv_register(&indevDrv);
}

void setup() {
    Serial.begin(115200);

    setupDisplayAndTouch();
    setupLvgl();
    buildUi();

    lastLvglTickMs = millis();

    connectToWiFi();
    fetchDataFromServer();
    lastDataUpdateMs = millis();
}

void loop() {
    updateLvglTick();

    if (!isWiFiConnected()) {
        reconnectWiFi();
        setLabelText(labelDev, "WiFi: reconnecting...");
    }

    if (millis() - lastDataUpdateMs >= kUpdateIntervalMs) {
        fetchDataFromServer();
        lastDataUpdateMs = millis();
    }

    delay(kLoopDelayMs);
}