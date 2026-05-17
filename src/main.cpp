#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "shared.h"
#include "ui.h"
#include "network.h"

DashboardData sharedData{"Loading...", "", {}, false, false, "Init...", false, 0, false, "Waiting for data..."};
SemaphoreHandle_t dataMutex = nullptr;
std::vector<ActionTask> actionQueue;
bool isScreenOn = true;
uint32_t lastTouchMs = 0;
bool timeSyncStarted = false;

[[noreturn]] void haltSystem(const char* message) {
    Serial.println(message);
    for (;;) delay(1000);
}

void setupDataMutex() {
    dataMutex = xSemaphoreCreateMutex();
    if (dataMutex == nullptr) haltSystem("Fatal: data mutex failed");
}

void setup() {
    Serial.begin(115200);

    setupDataMutex();
    setupDisplayAndTouch();
    setupLvgl();
    buildUi();

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