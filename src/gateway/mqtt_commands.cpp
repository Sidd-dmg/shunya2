#include "mqtt_commands.h"
#include "app_state.h"
#include "config.h"
#include "mode_engine.h"
#include "relay_control.h"
#include "persistence.h"
#include "mqtt_service.h"
#include "event_log.h"
#include "ota_service.h"
#include "button.h"
#include "link_test.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <EEPROM.h>

/* ArduinoJson 7 deprecates containsKey(); a missing key yields a null variant. */
static inline bool hasKey(JsonVariantConst value) {
    return !value.isNull();
}

static void handleModeCommand(JsonDocument &doc) {
    uint8_t newMode = doc["mode"];
    if (newMode > MODE_HYBRID) {
        return;
    }
    bool graceful = false;
    if (hasKey(doc["transition"])) {
        graceful = (doc["transition"].as<String>() == "graceful");
    }
    transitionToMode(newMode, graceful);
    if (doc["auto_save"].as<bool>()) {
        EEPROM.write(EEPROM_MODE_ADDR, operationMode);
        EEPROM.commit();
    }
    publishStatus();
}

static void handleManualCommand(JsonDocument &doc) {
    bool changed = false;
    if (hasKey(doc["all"])) {
        requestAllRelays(doc["all"].as<bool>(), 0);
        changed = true;
    }
    for (int i = 0; i < 4; i++) {
        String relayKey = "relay" + String(i + 1);
        if (hasKey(doc[relayKey])) {
            requestRelay(i, doc[relayKey].as<bool>(), 0);
            changed = true;
        }
    }
    if (changed) {
        saveManualModeConfig();
        publishStatus();
    }
}

static void handleAutomationConfig(JsonDocument &doc) {
    if (!hasKey(doc["automation"])) {
        return;
    }
    JsonObject automation = doc["automation"];
    if (automation["clear"] | false) {
        initializeAutomationRules();
        saveAutomationConfig();
        publishStatus();
        return;
    }
    if (!hasKey(automation["rules"])) {
        return;
    }
    initializeAutomationRules();
    int ruleIndex = 0;
    for (JsonObject ruleObj : automation["rules"].as<JsonArray>()) {
        if (ruleIndex >= MAX_AUTO_RULES) {
            break;
        }
        if (!hasKey(ruleObj["node"]) || !hasKey(ruleObj["relay"])) {
            continue;
        }
        autoRules[ruleIndex].nodeId = ruleObj["node"];
        autoRules[ruleIndex].relayId = ruleObj["relay"];
        autoRules[ruleIndex].enabled = true;
        bool hasTemp = hasKey(ruleObj["temp"]);
        bool hasHum = hasKey(ruleObj["humidity"]);
        autoRules[ruleIndex].useTempControl = hasTemp;
        autoRules[ruleIndex].useHumControl = hasHum;
        autoRules[ruleIndex].logicOR = true;
        if (hasKey(ruleObj["logic"])) {
            String logic = ruleObj["logic"].as<String>();
            autoRules[ruleIndex].logicOR = (logic == "OR" || logic == "or");
        }
        if (hasTemp) {
            autoRules[ruleIndex].tempMin = ruleObj["temp"]["min"];
            autoRules[ruleIndex].tempMax = ruleObj["temp"]["max"];
        } else {
            autoRules[ruleIndex].tempMin = -999.0;
            autoRules[ruleIndex].tempMax = 999.0;
        }
        if (hasHum) {
            autoRules[ruleIndex].humMin = ruleObj["humidity"]["min"];
            autoRules[ruleIndex].humMax = ruleObj["humidity"]["max"];
        } else {
            autoRules[ruleIndex].humMin = 0;
            autoRules[ruleIndex].humMax = 100;
        }
        autoRules[ruleIndex].lastState = false;
        autoRules[ruleIndex].lastStateChange = 0;
        ruleIndex++;
    }
    saveAutomationConfig();
    publishStatus();
}

static void handleQueueConfig(JsonDocument &doc) {
    if (!hasKey(doc["queue"])) {
        return;
    }
    queueRunning = false;
    JsonObject queueObj = doc["queue"];
    queueConfig.loop = queueObj["loop"] | true;
    queueConfig.autoStart = queueObj["auto_start"] | true;
    if (hasKey(queueObj["steps"])) {
        JsonArray steps = queueObj["steps"];
        queueConfig.stepCount = 0;
        for (JsonObject step : steps) {
            if (queueConfig.stepCount >= MAX_QUEUE_STEPS) {
                break;
            }
            queueConfig.steps[queueConfig.stepCount].relayMask = 0;
            queueConfig.steps[queueConfig.stepCount].duration = 0;
            if (hasKey(step["delay"])) {
                queueConfig.steps[queueConfig.stepCount].duration = step["delay"];
            } else if (hasKey(step["relays"])) {
                uint8_t mask = 0;
                for (uint8_t r : step["relays"].as<JsonArray>()) {
                    if (r >= 1 && r <= 4) {
                        mask |= (1 << (r - 1));
                    }
                }
                queueConfig.steps[queueConfig.stepCount].relayMask = mask;
                queueConfig.steps[queueConfig.stepCount].duration = step["duration"];
            } else if (hasKey(step["relay"])) {
                uint8_t r = step["relay"];
                if (r >= 1 && r <= 4) {
                    queueConfig.steps[queueConfig.stepCount].relayMask = (1 << (r - 1));
                }
                queueConfig.steps[queueConfig.stepCount].duration = step["duration"];
            }
            queueConfig.stepCount++;
        }
    }
    saveQueueConfig();
    if (queueConfig.stepCount > 0) {
        queueRunning = true;
        queuePaused = false;
        currentQueueStep = 0;
        queueStepStartTime = millis();
        QueueStep &firstStep = queueConfig.steps[0];
        if (firstStep.relayMask != 0) {
            for (int i = 0; i < 4; i++) {
                if (firstStep.relayMask & (1 << i)) {
                    requestRelay(i, true, 2);
                }
            }
        }
    }
    publishStatus();
}

static void handleQueueControl(JsonDocument &doc) {
    String control = doc["queue_control"].as<String>();
    if (control == "pause" && queueRunning && !queuePaused) {
        queuePaused = true;
    } else if (control == "resume" && queueRunning && queuePaused) {
        queuePaused = false;
    } else if (control == "stop") {
        queueRunning = false;
        queuePaused = false;
        queueConfig.autoStart = false;
        saveQueueConfig();
        requestAllRelays(false, 2);
    } else if (control == "restart") {
        requestAllRelays(false, 2);
        currentQueueStep = 0;
        queueStepStartTime = millis();
        queueRunning = true;
        queuePaused = false;
        queueConfig.autoStart = true;
        saveQueueConfig();
        QueueStep &firstStep = queueConfig.steps[0];
        if (firstStep.relayMask != 0) {
            for (int i = 0; i < 4; i++) {
                if (firstStep.relayMask & (1 << i)) {
                    requestRelay(i, true, 2);
                }
            }
        }
    } else if (control == "enable_autostart") {
        queueConfig.autoStart = true;
        saveQueueConfig();
    } else if (control == "disable_autostart") {
        queueConfig.autoStart = false;
        saveQueueConfig();
    }
    publishStatus();
}

static void handleHybridConfig(JsonDocument &doc) {
    if (!hasKey(doc["hybrid"])) {
        return;
    }
    JsonObject hybrid = doc["hybrid"];
    if (hasKey(hybrid["logic"])) {
        String logic = hybrid["logic"].as<String>();
        hybridLogic = (logic == "OR" || logic == "or") ? 1 : 0;
    }
    if (hasKey(hybrid["automation"])) {
        JsonDocument autoDoc;
        autoDoc["automation"] = hybrid["automation"];
        handleAutomationConfig(autoDoc);
    }
    if (hasKey(hybrid["queue"])) {
        JsonDocument queueDoc;
        queueDoc["queue"] = hybrid["queue"];
        handleQueueConfig(queueDoc);
    }
    saveHybridConfig();
    publishStatus();
}

static void handleEventLogCommand(JsonDocument &doc) {
    String cmd = doc["event_log"].as<String>();
    if (cmd == "send") {
        sendEventLogs();
    } else if (cmd == "clear") {
        clearEventLogs();
    } else if (cmd == "count") {
        JsonDocument resp;
        resp["status"] = "count";
        resp["count"] = getEventLogCount();
        resp["max"] = EVENT_LOG_MAX_ENTRIES;
        char buffer[256];
        serializeJson(resp, buffer);
        mqttEnqueuePublish(mqtt_event_log_topic.c_str(), buffer, false);
    } else if (cmd == "send_and_clear") {
        sendEventLogs();
        clearEventLogs();
    }
}

void processMqttCommandPayload(const char *payload, unsigned int length) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
        Serial.println("JSON parse failed");
        return;
    }
    if (hasKey(doc["event_log"])) {
        handleEventLogCommand(doc);
        return;
    }
    if (doc["ota_update"].as<bool>()) {
        requestOtaUpdate();
        return;
    }
    if (doc["ota_check"].as<bool>()) {
        requestOtaCheck();
        return;
    }
    if (hasKey(doc["mode"])) {
        handleModeCommand(doc);
        return;
    }
    if (hasKey(doc["queue_control"])) {
        handleQueueControl(doc);
        return;
    }
    if (hasKey(doc["hybrid"])) {
        handleHybridConfig(doc);
        return;
    }
    if (hasKey(doc["pair"])) {
        if (doc["pair"].as<bool>()) {
            enablePairingMode();
        } else {
            disablePairingMode();
        }
        return;
    }
    if (hasKey(doc["sensor_sleep"])) {
        applySensorSleep((uint16_t)doc["sensor_sleep"].as<unsigned int>());
        publishStatus();
        return;
    }
    if (hasKey(doc["link_test"])) {
        String t = doc["link_test"].as<String>();
        t.toLowerCase();
        if (t == "hub") {
            linkTestRequest(LINK_TEST_HUB);
        } else if (t == "display-tx") {
            linkTestRequest(LINK_TEST_DISPLAY_TX);
        } else if (t == "display") {
            linkTestRequest(LINK_TEST_DISPLAY_ECHO);
        } else if (t == "display-loop" || t == "display-loopback") {
            linkTestRequest(LINK_TEST_DISPLAY_LOOPBACK);
        } else if (t == "all") {
            linkTestRequest(LINK_TEST_ALL);
        } else if (t == "stats") {
            linkTestRequest(LINK_TEST_STATS);
        }
        return;
    }
    switch (operationMode) {
        case MODE_MANUAL:
            handleManualCommand(doc);
            break;
        case MODE_AUTOMATION:
            handleAutomationConfig(doc);
            break;
        case MODE_QUEUE:
            handleQueueConfig(doc);
            break;
        case MODE_HYBRID:
            if (hasKey(doc["automation"])) {
                handleAutomationConfig(doc);
            }
            if (hasKey(doc["queue"])) {
                handleQueueConfig(doc);
            }
            break;
        default:
            break;
    }
}
