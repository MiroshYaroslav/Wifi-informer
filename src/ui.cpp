#include "ui.h"
#include "shared.h"
#include "config.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <ctime>

LV_FONT_DECLARE(font_20);
LV_FONT_DECLARE(font_32);

namespace {

TFT_eSPI tft;
SPIClass touchSpi(HSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);

lv_disp_draw_buf_t drawBuffer{};
lv_color_t drawBufferPixels[kDrawBufferSize];

lv_obj_t* labelTime = nullptr;
lv_obj_t* labelDate = nullptr;
lv_obj_t* labelWeather = nullptr;
lv_obj_t* labelCity = nullptr;
lv_obj_t* tabCam = nullptr;
lv_obj_t* tabLED = nullptr;
lv_obj_t* tabGate = nullptr;
lv_obj_t* labelCamLog = nullptr;
lv_obj_t* labelSysStatus = nullptr;
lv_obj_t* wifiSetupOverlay = nullptr;

uint32_t lastLvglTickMs = 0;
bool isApplyingSwitchState = false;

struct UiDevice {
    char entity_id[64];
    char ui_type[16];
    lv_obj_t* label;
    lv_obj_t* control_obj;
    lv_obj_t* extra_obj;
    lv_obj_t* extra_label;
    bool togglePending;
    uint32_t toggleDeadlineMs;
};
std::vector<UiDevice> uiDevices;

void updateLabelIfChanged(lv_obj_t* label, const char* text) {
    if (label == nullptr || text == nullptr) return;
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void updateLvglTick() {
    const uint32_t now = millis();
    const uint32_t elapsed = now - lastLvglTickMs;
    lastLvglTickMs = now;
    if (elapsed > 0) lv_tick_inc(elapsed);
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
    if (labelTime == nullptr || labelDate == nullptr) return;
    const time_t now = time(nullptr);
    if (now < 100000 || !timeSyncStarted) {
        updateLabelIfChanged(labelTime, "--:--");
        updateLabelIfChanged(labelDate, "Syncing time...");
        return;
    }
    struct tm timeinfo{};
    localtime_r(&now, &timeinfo);
    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    updateLabelIfChanged(labelTime, timeStr);

    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%a, %d.%m.%Y", &timeinfo);
    updateLabelIfChanged(labelDate, dateStr);
}

void dynamicControlEventCb(lv_event_t* event) {
    if (isApplyingSwitchState) return;
    int idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t* target = lv_event_get_target(event);

    DataLockGuard lock;
    if (idx < 0 || idx >= uiDevices.size()) return;

    ActionTask task{};
    strcpy(task.entity_id, uiDevices[idx].entity_id);
    task.value = 0;

    if (strcmp(uiDevices[idx].ui_type, "toggle") == 0 && code == LV_EVENT_VALUE_CHANGED) {
        strcpy(task.action, "toggle");
        actionQueue.push_back(task);
        uiDevices[idx].togglePending = true;
        uiDevices[idx].toggleDeadlineMs = millis() + kSwitchAckTimeoutMs;
    }
    else if (strcmp(uiDevices[idx].ui_type, "slider") == 0) {
        if (target == uiDevices[idx].extra_obj && code == LV_EVENT_VALUE_CHANGED) {
            strcpy(task.action, "toggle");
            actionQueue.push_back(task);
            uiDevices[idx].togglePending = true;
            uiDevices[idx].toggleDeadlineMs = millis() + kSwitchAckTimeoutMs;
        }
        else if (target == uiDevices[idx].control_obj) {
            if (code == LV_EVENT_VALUE_CHANGED) {
                int val = (int)lv_slider_get_value(uiDevices[idx].control_obj);
                if (uiDevices[idx].extra_label) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d%%", val);
                    lv_label_set_text(uiDevices[idx].extra_label, buf);
                }
                if (uiDevices[idx].extra_obj) {
                    if (val > 0) lv_obj_add_state(uiDevices[idx].extra_obj, LV_STATE_CHECKED);
                    else lv_obj_clear_state(uiDevices[idx].extra_obj, LV_STATE_CHECKED);
                }
            }
            else if (code == LV_EVENT_RELEASED) {
                strcpy(task.action, "slider");
                task.value = lv_slider_get_value(uiDevices[idx].control_obj);
                actionQueue.push_back(task);
                uiDevices[idx].togglePending = true;
                uiDevices[idx].toggleDeadlineMs = millis() + kSwitchAckTimeoutMs;
            }
        }
    }
    else if (strcmp(uiDevices[idx].ui_type, "button") == 0 && code == LV_EVENT_CLICKED) {
        strcpy(task.action, "button");
        actionQueue.push_back(task);
        uiDevices[idx].togglePending = true;
        uiDevices[idx].toggleDeadlineMs = millis() + kSwitchAckTimeoutMs;
    }
}

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
    indevDrv.scroll_limit = 5; 
    indevDrv.scroll_throw = 20;
    lv_indev_drv_register(&indevDrv);
    lastLvglTickMs = millis();
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

    lv_obj_t* tabMain = lv_tabview_add_tab(tabview, "Main");
    tabCam = lv_tabview_add_tab(tabview, "Cam");
    tabLED = lv_tabview_add_tab(tabview, "LED");
    tabGate = lv_tabview_add_tab(tabview, "Gate");

    lv_obj_clear_flag(tabMain, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tabMain, 0, 0);

    lv_obj_add_flag(tabCam, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tabCam, LV_DIR_VER);
    lv_obj_add_flag(tabLED, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tabLED, LV_DIR_VER);
    lv_obj_add_flag(tabGate, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tabGate, LV_DIR_VER);

    labelTime = lv_label_create(tabMain);
    lv_obj_set_style_text_font(labelTime, &font_32, 0);
    lv_label_set_text(labelTime, "--:--");
    lv_obj_align(labelTime, LV_ALIGN_TOP_MID, 0, 20);

    labelDate = lv_label_create(tabMain);
    lv_obj_set_style_text_font(labelDate, &font_20, 0);
    lv_obj_set_style_text_color(labelDate, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(labelDate, "Syncing...");
    lv_obj_align(labelDate, LV_ALIGN_TOP_MID, 0, 60);

    labelWeather = lv_label_create(tabMain);
    lv_obj_set_style_text_font(labelWeather, &font_32, 0);
    lv_label_set_text(labelWeather, "Loading...");
    lv_obj_align(labelWeather, LV_ALIGN_TOP_MID, 0, 110);

    labelCity = lv_label_create(tabMain);
    lv_obj_set_style_text_font(labelCity, &font_20, 0);
    lv_obj_set_style_text_color(labelCity, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(labelCity, "");
    lv_obj_align(labelCity, LV_ALIGN_TOP_MID, 0, 150);

    lv_timer_create(updateTimeLabelCb, 1000, nullptr);
}

void applyPendingUiUpdateIfAny() {
    DashboardData localCopy{};
    {
        DataLockGuard lock;
        if (!sharedData.dirty) return;
        localCopy = sharedData;
        sharedData.dirty = false;
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
            lv_label_set_text(label, "Wi-Fi Setup Required\n\n1. Connect to AP:\n#FFFF00 Smart_Setup#\n\n2. Open browser:\n#FFFF00 192.168.4.1#");
            lv_obj_center(label);
        }
    } else {
        if (wifiSetupOverlay != nullptr) {
            lv_obj_del(wifiSetupOverlay);
            wifiSetupOverlay = nullptr;
        }
    }

    if (labelSysStatus != nullptr) {
        updateLabelIfChanged(labelSysStatus, localCopy.sysMessage);
    }

    if (labelWeather != nullptr && labelCity != nullptr) {
        char weatherBuffer[64];
        copyText(weatherBuffer, localCopy.weather);
        char* newline = strchr(weatherBuffer, '\n');
        if (newline != nullptr) {
            *newline = '\0';
            char* weatherStr = newline + 1;
            updateLabelIfChanged(labelCity, weatherBuffer);
            updateLabelIfChanged(labelWeather, weatherStr);
        } else {
            updateLabelIfChanged(labelWeather, weatherBuffer);
            updateLabelIfChanged(labelCity, "");
        }
    }

    if (localCopy.isSmartReady) {
        if (uiDevices.size() != localCopy.devices.size()) {
            for (auto& uiDev : uiDevices) {
                if(uiDev.label) lv_obj_del(uiDev.label);
                if(uiDev.control_obj) lv_obj_del(uiDev.control_obj);
                if(uiDev.extra_obj) lv_obj_del(uiDev.extra_obj);
                if(uiDev.extra_label) lv_obj_del(uiDev.extra_label);
            }
            uiDevices.clear();

            int y_cam = 20;
            int y_led = 20;
            int y_gate = 20;

            for (size_t i = 0; i < localCopy.devices.size(); ++i) {
                UiDevice uiDev{};
                uiDev.label = nullptr;
                uiDev.control_obj = nullptr;
                uiDev.extra_obj = nullptr;
                uiDev.extra_label = nullptr;

                strcpy(uiDev.entity_id, localCopy.devices[i].entity_id);
                strcpy(uiDev.ui_type, localCopy.devices[i].ui_type);
                uiDev.togglePending = false;

                lv_obj_t* targetTab;
                int* current_y;

                if (strcmp(uiDev.ui_type, "slider") == 0) {
                    targetTab = tabLED;
                    current_y = &y_led;
                } else if (strcmp(uiDev.ui_type, "button") == 0) {
                    targetTab = tabGate;
                    current_y = &y_gate;
                } else {
                    targetTab = tabCam;
                    current_y = &y_cam;
                }

                if (strcmp(uiDev.ui_type, "toggle") == 0) {
                    uiDev.label = lv_label_create(targetTab);
                    lv_obj_set_style_text_font(uiDev.label, &font_20, 0);
                    lv_label_set_text(uiDev.label, localCopy.devices[i].friendly_name);
                    lv_obj_align(uiDev.label, LV_ALIGN_TOP_LEFT, 20, *current_y + 10);

                    uiDev.control_obj = lv_switch_create(targetTab);
                    lv_obj_set_size(uiDev.control_obj, 70, 35);
                    lv_obj_set_ext_click_area(uiDev.control_obj, 20);
                    lv_obj_clear_flag(uiDev.control_obj, LV_OBJ_FLAG_SCROLL_CHAIN);
                    lv_obj_align(uiDev.control_obj, LV_ALIGN_TOP_RIGHT, -20, *current_y);
                    lv_obj_add_event_cb(uiDev.control_obj, dynamicControlEventCb, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void*>(i));

                    *current_y += 60;
                }
                else if (strcmp(uiDev.ui_type, "slider") == 0) {
                    uiDev.label = lv_label_create(targetTab);
                    lv_obj_set_style_text_font(uiDev.label, &font_20, 0);
                    lv_label_set_text(uiDev.label, localCopy.devices[i].friendly_name);
                    lv_obj_align(uiDev.label, LV_ALIGN_TOP_LEFT, 20, *current_y + 10);

                    uiDev.extra_obj = lv_switch_create(targetTab);
                    lv_obj_set_size(uiDev.extra_obj, 50, 25);
                    lv_obj_clear_flag(uiDev.extra_obj, LV_OBJ_FLAG_SCROLL_CHAIN);
                    lv_obj_align(uiDev.extra_obj, LV_ALIGN_TOP_MID, 0, *current_y + 5);
                    lv_obj_add_event_cb(uiDev.extra_obj, dynamicControlEventCb, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void*>(i));

                    uiDev.extra_label = lv_label_create(targetTab);
                    lv_obj_set_style_text_font(uiDev.extra_label, &font_20, 0);
                    lv_obj_set_style_text_color(uiDev.extra_label, lv_color_hex(0x00FFFF), 0);
                    char pctStr[8];
                    snprintf(pctStr, sizeof(pctStr), "%d%%", localCopy.devices[i].value);
                    lv_label_set_text(uiDev.extra_label, pctStr);
                    lv_obj_align(uiDev.extra_label, LV_ALIGN_TOP_RIGHT, -20, *current_y + 10);

                    uiDev.control_obj = lv_slider_create(targetTab);
                    lv_obj_set_size(uiDev.control_obj, kScreenWidth - 40, 20);
                    lv_slider_set_range(uiDev.control_obj, 0, 100);
                    lv_obj_align(uiDev.control_obj, LV_ALIGN_TOP_MID, 0, *current_y + 40);
                    lv_obj_add_event_cb(uiDev.control_obj, dynamicControlEventCb, LV_EVENT_VALUE_CHANGED, reinterpret_cast<void*>(i));
                    lv_obj_add_event_cb(uiDev.control_obj, dynamicControlEventCb, LV_EVENT_RELEASED, reinterpret_cast<void*>(i));

                    *current_y += 80;
                }
                else if (strcmp(uiDev.ui_type, "button") == 0) {
                    uiDev.label = lv_label_create(targetTab);
                    lv_obj_set_style_text_font(uiDev.label, &font_20, 0);
                    lv_label_set_text(uiDev.label, localCopy.devices[i].friendly_name);
                    lv_obj_align(uiDev.label, LV_ALIGN_TOP_LEFT, 20, *current_y + 10);

                    uiDev.control_obj = lv_btn_create(targetTab);
                    lv_obj_set_size(uiDev.control_obj, 90, 40);
                    lv_obj_align(uiDev.control_obj, LV_ALIGN_TOP_RIGHT, -20, *current_y);
                    lv_obj_add_event_cb(uiDev.control_obj, dynamicControlEventCb, LV_EVENT_CLICKED, reinterpret_cast<void*>(i));

                    uiDev.extra_label = lv_label_create(uiDev.control_obj);
                    lv_label_set_text(uiDev.extra_label, "ACT");
                    lv_obj_center(uiDev.extra_label);

                    *current_y += 60;
                }

                uiDevices.push_back(uiDev);
            }

            if (labelCamLog) { lv_obj_del(labelCamLog); }
            labelCamLog = lv_label_create(tabCam);
            lv_obj_set_style_text_font(labelCamLog, &font_20, 0);
            lv_obj_set_style_text_color(labelCamLog, lv_color_hex(0xFFFF00), 0);
            lv_obj_set_width(labelCamLog, kScreenWidth - 40);
            lv_label_set_long_mode(labelCamLog, LV_LABEL_LONG_WRAP);
            lv_obj_align(labelCamLog, LV_ALIGN_TOP_LEFT, 20, y_cam + 20);
        }

        if (labelCamLog != nullptr) {
            lv_obj_clear_flag(labelCamLog, LV_OBJ_FLAG_HIDDEN);
            updateLabelIfChanged(labelCamLog, localCopy.cameraLog);
        }

        isApplyingSwitchState = true;
        uint32_t now = millis();
        for (size_t i = 0; i < localCopy.devices.size(); ++i) {
            for(auto& uiDev : uiDevices) {
                if (strcmp(uiDev.entity_id, localCopy.devices[i].entity_id) == 0) {
                    if(uiDev.label) lv_obj_clear_flag(uiDev.label, LV_OBJ_FLAG_HIDDEN);
                    if(uiDev.control_obj) lv_obj_clear_flag(uiDev.control_obj, LV_OBJ_FLAG_HIDDEN);
                    if(uiDev.extra_obj) lv_obj_clear_flag(uiDev.extra_obj, LV_OBJ_FLAG_HIDDEN);
                    if(uiDev.extra_label) lv_obj_clear_flag(uiDev.extra_label, LV_OBJ_FLAG_HIDDEN);

                    if (uiDev.togglePending && static_cast<int32_t>(now - uiDev.toggleDeadlineMs) >= 0) {
                        uiDev.togglePending = false;
                    }

                    if (!uiDev.togglePending) {
                        if (strcmp(uiDev.ui_type, "toggle") == 0) {
                            if (localCopy.devices[i].state) lv_obj_add_state(uiDev.control_obj, LV_STATE_CHECKED);
                            else lv_obj_clear_state(uiDev.control_obj, LV_STATE_CHECKED);
                        }
                        else if (strcmp(uiDev.ui_type, "slider") == 0) {
                            if (!lv_obj_has_state(uiDev.control_obj, LV_STATE_PRESSED)) {
                                if (uiDev.extra_obj) {
                                    if (localCopy.devices[i].state) lv_obj_add_state(uiDev.extra_obj, LV_STATE_CHECKED);
                                    else lv_obj_clear_state(uiDev.extra_obj, LV_STATE_CHECKED);
                                }
                                lv_slider_set_value(uiDev.control_obj, localCopy.devices[i].value, LV_ANIM_OFF);
                                if (uiDev.extra_label) {
                                    char buf[16];
                                    snprintf(buf, sizeof(buf), "%d%%", localCopy.devices[i].value);
                                    lv_label_set_text(uiDev.extra_label, buf);
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
        isApplyingSwitchState = false;
    } else {
        for (auto& uiDev : uiDevices) {
            if(uiDev.label) lv_obj_add_flag(uiDev.label, LV_OBJ_FLAG_HIDDEN);
            if(uiDev.control_obj) lv_obj_add_flag(uiDev.control_obj, LV_OBJ_FLAG_HIDDEN);
            if(uiDev.extra_obj) lv_obj_add_flag(uiDev.extra_obj, LV_OBJ_FLAG_HIDDEN);
            if(uiDev.extra_label) lv_obj_add_flag(uiDev.extra_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (labelCamLog) lv_obj_add_flag(labelCamLog, LV_OBJ_FLAG_HIDDEN);
    }
}

void pumpUiAndPendingData() {
    updateLvglTick();
    applyPendingUiUpdateIfAny();
    lv_timer_handler();
}