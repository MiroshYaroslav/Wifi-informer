#pragma once

#include <Arduino.h>

constexpr uint16_t kBackendPort = 8000;
constexpr uint16_t kMqttPort = 1883;
constexpr uint16_t kUdpPort = 5555;

constexpr char kDiscoveryMsg[] = "DISCOVER_SMART_DASHBOARD";
constexpr char kDashboardPath[] = "/api/dashboard";
constexpr char kMqttTopicState[] = "smart/dashboard/state";
constexpr char kMqttTopicTelemetry[] = "smart/dashboard/telemetry";
constexpr char kMqttTopicHeartbeat[] = "smart/dashboard/heartbeat";
constexpr char kMqttTopicCommand[] = "smart/dashboard/command";
constexpr char kMqttClientId[] = "cyd-dashboard";

constexpr char kNtpServer[] = "pool.ntp.org";
constexpr char kTimezone[] = "EET-2EEST,M3.5.0/3,M10.5.0/4";

constexpr uint16_t kScreenWidth = 320;
constexpr uint16_t kScreenHeight = 240;

constexpr uint32_t kDashboardPollIntervalMs = 2.5 * 60 * 1000;
constexpr uint32_t kTelemetryIntervalMs = 60 * 1000;
constexpr uint16_t kHttpTimeoutMs = 6000;
constexpr uint16_t kUiLoopDelayMs = 1;
constexpr uint32_t kReconnectDelayMs = 3000;
constexpr uint32_t kNetworkLoopDelayMs = 50;
constexpr uint32_t kSwitchAckTimeoutMs = 3000;

constexpr uint8_t kBacklightPin = 21;
constexpr uint32_t kScreenSaverTimeoutMs = 0.5 * 60 * 1000;
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
constexpr size_t kDashboardJsonCapacity = 8192;
constexpr size_t kMqttJsonCapacity = 8192;

constexpr uint32_t kNetworkTaskStackSize = 8192;
constexpr UBaseType_t kNetworkTaskPriority = 1;