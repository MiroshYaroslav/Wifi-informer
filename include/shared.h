#pragma once
#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct ActionTask {
    char entity_id[64];
    char action[16];
    int value;
};

struct SmartDevice {
    char entity_id[64];
    char friendly_name[64];
    bool state;
    char ui_type[16];
    int value;
};

struct DashboardData {
    char weather[64];
    char sysMessage[32];
    std::vector<SmartDevice> devices;
    bool needWifiSetup;
    bool isSmartReady;
    char smartErrorMsg[64];
    bool haOffline;
    uint32_t lastHeartbeatMs;
    bool dirty;
    char cameraLog[512];
};

extern DashboardData sharedData;
extern SemaphoreHandle_t dataMutex;
extern std::vector<ActionTask> actionQueue;
extern bool isScreenOn;
extern uint32_t lastTouchMs;
extern bool timeSyncStarted;

class DataLockGuard final {
public:
    DataLockGuard() {
        if (dataMutex != nullptr) {
            xSemaphoreTake(dataMutex, portMAX_DELAY);
            locked_ = true;
        }
    }
    ~DataLockGuard() {
        if (locked_) xSemaphoreGive(dataMutex);
    }
private:
    bool locked_ = false;
};

template <size_t N>
void copyText(char (&dst)[N], const char* src) {
    if (src == nullptr) { dst[0] = '\0'; return; }
    snprintf(dst, N, "%s", src);
}

[[noreturn]] void haltSystem(const char* message);