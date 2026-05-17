#include "network.h"
#include "shared.h"
#include "config.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ctime>

namespace {

char backendHost[20] = "";
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
TaskHandle_t networkTaskHandle = nullptr;

bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void startTimeSync() {
    configTzTime(kTimezone, kNtpServer, "time.google.com", "time.nist.gov");
    timeSyncStarted = true;
}

void setSysMessage(const char* msg) {
    DataLockGuard lock;
    if (strncmp(sharedData.sysMessage, msg, sizeof(sharedData.sysMessage)) != 0) {
        copyText(sharedData.sysMessage, msg);
        sharedData.dirty = true;
    }
}

void saveCurrentNetwork(const String& ssid, const String& pass) {
    if (ssid.isEmpty()) return;
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
        String s1 = prefs.getString("s1", "");
        String p1 = prefs.getString("p1", "");
        String s2 = prefs.getString("s2", "");
        String p2 = prefs.getString("p2", "");
        prefs.putString("s0", s1); prefs.putString("p0", p1);
        prefs.putString("s1", s2); prefs.putString("p1", p2);
        prefs.putString("s2", ssid); prefs.putString("p2", pass);
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
    if (bestIdx == -1) return false;
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
    udp.begin(kUdpPort);
    uint32_t start = millis();
    while (millis() - start < 5000) {
        udp.beginPacket(IPAddress(255, 255, 255, 255), kUdpPort);
        udp.print(kDiscoveryMsg);
        udp.endPacket();
        uint32_t waitTime = millis();
        while (millis() - waitTime < 1000) {
            int packetSize = udp.parsePacket();
            if (packetSize > 0) {
                char incomingPacket[255];
                int len = udp.read(incomingPacket, 255);
                if (len > 0) incomingPacket[len] = 0;
                if (strncmp(incomingPacket, "BACKEND_HERE", 12) == 0) {
                    strncpy(backendHost, udp.remoteIP().toString().c_str(), sizeof(backendHost) - 1);
                    return true;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    return false;
}

void parseDashboardJson(JsonDocument& doc) {
    DataLockGuard lock;
    if (doc.containsKey("weather")) copyText(sharedData.weather, doc["weather"]);
    if (doc.containsKey("sysMessage")) copyText(sharedData.sysMessage, doc["sysMessage"]);
    if (doc.containsKey("camera_log")) copyText(sharedData.cameraLog, doc["camera_log"]);

    if (doc.containsKey("controllable_devices")) {
        sharedData.devices.clear();
        JsonArray devicesArr = doc["controllable_devices"].as<JsonArray>();
        for (JsonObject dev : devicesArr) {
            SmartDevice newDev{};
            copyText(newDev.entity_id, dev["entity_id"]);
            copyText(newDev.friendly_name, dev["friendly_name"]);
            copyText(newDev.ui_type, dev["ui_type"] | "toggle");
            newDev.state = dev["state"] | false;
            newDev.value = dev["value"] | 0;
            sharedData.devices.push_back(newDev);
        }
    }
    sharedData.dirty = true;
}

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
    if (strcmp(topic, kMqttTopicHeartbeat) == 0) {
        DynamicJsonDocument doc(256);
        if (!deserializeJson(doc, payload, length)) {
            DataLockGuard lock;
            sharedData.lastHeartbeatMs = millis();
            const char* haStatus = doc["ha_status"] | "Online";
            sharedData.haOffline = (strcmp(haStatus, "HA_Offline") == 0);
            sharedData.dirty = true;
        }
        return;
    }

    DynamicJsonDocument doc(kMqttJsonCapacity);
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
    if (!isWiFiConnected() || strlen(backendHost) == 0) return false;
    if (mqttClient.connected()) return true;
    if (!mqttClient.connect(kMqttClientId)) return false;
    mqttClient.subscribe(kMqttTopicState);
    mqttClient.subscribe(kMqttTopicHeartbeat);
    return true;
}

void sendTelemetry() {
    if (!mqttClient.connected()) return;
    DynamicJsonDocument doc(256);
    doc["rssi"] = WiFi.RSSI();
    doc["uptime"] = millis() / 1000;
    doc["free_heap"] = ESP.getFreeHeap();
    char buffer[256];
    size_t n = serializeJson(doc, buffer);
    mqttClient.publish(kMqttTopicTelemetry, buffer, n);
}

bool sendActionCommand(const ActionTask& task) {
    if (!mqttClient.connected()) return false;
    DynamicJsonDocument doc(256);
    doc["entity_id"] = task.entity_id;
    doc["action"] = task.action;
    doc["value"] = task.value;
    char buffer[256];
    size_t n = serializeJson(doc, buffer);
    return mqttClient.publish(kMqttTopicCommand, buffer, n);
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
            saveCurrentNetwork(wm.getWiFiSSID(), wm.getWiFiPass());
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

    uint32_t lastTelemetryMs = 0;
    int mqttFails = 0;
    bool wasWifiConnected = isWiFiConnected();
    uint32_t currentReconnectDelay = kReconnectDelayMs;
    static bool wasMqttOk = false;

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
                currentReconnectDelay = (currentReconnectDelay * 2 > 60000) ? 60000 : currentReconnectDelay * 2;
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
                setSysMessage("");
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }

        bool mqttOk = connectMqtt();
        if (mqttOk) {
            if (!wasMqttOk) {
                DataLockGuard lock;
                sharedData.lastHeartbeatMs = millis();
                wasMqttOk = true;
                if (time(nullptr) < 100000) startTimeSync(); 
            }
            mqttFails = 0;
            mqttClient.loop();
        } else {
            wasMqttOk = false;
            mqttFails++;
        }

        bool isReady = false;
        char errMsg[64] = "";
        char sysMsg[16] = "";

        std::vector<ActionTask> tasksToSend;

        {
            DataLockGuard lock;
            bool backendDead = false;
            bool haOffline = sharedData.haOffline;

            if (sharedData.lastHeartbeatMs > 0 && (millis() - sharedData.lastHeartbeatMs > 12000)) {
                backendDead = true;
            }

            if (mqttFails >= 2) {
                copyText(errMsg, "MQTT Offline\nControls disabled");
                copyText(sysMsg, "No MQTT");
            } else if (backendDead) {
                copyText(errMsg, "Backend Offline\nControls disabled");
                copyText(sysMsg, "Backend Err");
                if (millis() - sharedData.lastHeartbeatMs > 30000) backendHost[0] = '\0';
            } else if (haOffline) {
                copyText(errMsg, "HA Offline\nControls disabled");
                copyText(sysMsg, "HA Error");
            } else {
                isReady = true;
                copyText(sysMsg, "");
            }

            if (sharedData.isSmartReady != isReady || strcmp(sharedData.smartErrorMsg, errMsg) != 0) {
                sharedData.isSmartReady = isReady;
                copyText(sharedData.smartErrorMsg, errMsg);
                sharedData.dirty = true;
            }

            tasksToSend = actionQueue;
            actionQueue.clear();
        }

        if (isReady) {
            for (const auto& task : tasksToSend) {
                bool success = sendActionCommand(task);
                if (success) {
                    DataLockGuard lock;
                    for (auto& dev : sharedData.devices) {
                        if (strcmp(dev.entity_id, task.entity_id) == 0) {
                            if (strcmp(task.action, "slider") == 0) {
                                dev.value = task.value;
                                dev.state = (task.value > 0);
                            } else if (strcmp(task.action, "toggle") == 0) {
                                dev.state = !dev.state;
                            }
                            break;
                        }
                    }
                    sharedData.dirty = true;
                }
            }
        }

        setSysMessage(sysMsg);

        if (millis() - lastTelemetryMs >= kTelemetryIntervalMs || lastTelemetryMs == 0) {
            if (mqttOk) sendTelemetry();
            lastTelemetryMs = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(kNetworkLoopDelayMs));
    }
}

void networkTask(void*) {
    runNetworkLoop();
}

}

void setupNetworkTask() {
    const BaseType_t result = xTaskCreatePinnedToCore(
        networkTask, "NetworkTask", kNetworkTaskStackSize, nullptr, kNetworkTaskPriority, &networkTaskHandle, 0
    );
    if (result != pdPASS || networkTaskHandle == nullptr) haltSystem("Fatal: network task failed");
}