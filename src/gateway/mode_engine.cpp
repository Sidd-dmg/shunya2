#include "mode_engine.h"
#include "app_state.h"
#include "rtos.h"
#include "relay_control.h"
#include "persistence.h"
#include "mqtt_service.h"
#include "mqtt_commands.h"
#include "display_link.h"
#include "diagnostics.h"
#include "button.h"
#include "hub_link.h"
#include "wifi_prov.h"
#include <Arduino.h>
#include <cstring>

namespace {
int8_t s_pendingMode = -1;
unsigned long s_nodeReadingSaveMs = 0;
unsigned long s_queuePauseStartedMs = 0;

void saveNodeReadings(void) {
    const unsigned long now = millis();
    if (s_nodeReadingSaveMs != 0 && now - s_nodeReadingSaveMs < 300000UL) {
        return;
    }
    s_nodeReadingSaveMs = now;
    saveNodesToEEPROM();
}
}

void runManualMode(void) {}

void runAutomationMode(void) {
    uint8_t trig = 1;
    for (int i = 0; i < MAX_AUTO_RULES; i++) {
        if (!autoRules[i].enabled) {
            continue;
        }
        if (autoRules[i].relayId < 1 || autoRules[i].relayId > RELAY_COUNT) {
            continue;
        }
        if (autoRules[i].nodeId < 1 || autoRules[i].nodeId > MAX_NODES) {
            continue;
        }
        int8_t nodeIndex = findNodeById(autoRules[i].nodeId);
        if (nodeIndex < 0 || !nodes[nodeIndex].active) {
            continue;
        }
        if (millis() - nodes[nodeIndex].lastSeen > 300000UL) {
            continue;
        }
        float temp = nodes[nodeIndex].lastTemperature;
        float hum = nodes[nodeIndex].lastHumidity;
        bool tempShouldActivate = false;
        bool humShouldActivate = false;
        if (autoRules[i].useTempControl) {
            if (temp > autoRules[i].tempMax) {
                tempShouldActivate = true;
            } else if (temp < autoRules[i].tempMin) {
                tempShouldActivate = false;
            } else {
                tempShouldActivate = autoRules[i].lastState;
            }
        }
        if (autoRules[i].useHumControl) {
            if (hum > autoRules[i].humMax) {
                humShouldActivate = true;
            } else if (hum < autoRules[i].humMin) {
                humShouldActivate = false;
            } else {
                humShouldActivate = autoRules[i].lastState;
            }
        }
        bool shouldActivate = false;
        if (autoRules[i].useTempControl && autoRules[i].useHumControl) {
            shouldActivate = autoRules[i].logicOR ? (tempShouldActivate || humShouldActivate)
                                                  : (tempShouldActivate && humShouldActivate);
        } else if (autoRules[i].useTempControl) {
            shouldActivate = tempShouldActivate;
        } else if (autoRules[i].useHumControl) {
            shouldActivate = humShouldActivate;
        }
        if (shouldActivate != autoRules[i].lastState) {
            unsigned long currentMillis = millis();
            unsigned long timeSinceLastChange = currentMillis - autoRules[i].lastStateChange;
            if (timeSinceLastChange >= 3000 || autoRules[i].lastStateChange == 0) {
                requestRelay(autoRules[i].relayId - 1, shouldActivate, trig);
                autoRules[i].lastState = shouldActivate;
                autoRules[i].lastStateChange = currentMillis;
            }
        }
    }
}

void runQueueMode(void) {
    if (!queueRunning || queueConfig.stepCount == 0 || queuePaused) {
        return;
    }
    unsigned long currentMillis = millis();
    unsigned long elapsed = (currentMillis - queueStepStartTime) / 1000;
    QueueStep &currentStep = queueConfig.steps[currentQueueStep];
    if (elapsed < currentStep.duration) {
        return;
    }
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (currentStep.relayMask & (1 << i)) {
            requestRelay(i, false, 2);
        }
    }
    currentQueueStep++;
    if (currentQueueStep >= queueConfig.stepCount) {
        if (queueConfig.loop) {
            currentQueueStep = 0;
        } else {
            queueRunning = false;
            requestAllRelays(false, 2);
            if (s_pendingMode >= 0) {
                const uint8_t targetMode = (uint8_t)s_pendingMode;
                s_pendingMode = -1;
                transitionToMode(targetMode, false);
            }
            publishStatus();
            return;
        }
    }
    queueStepStartTime = currentMillis;
    QueueStep &newStep = queueConfig.steps[currentQueueStep];
    if (newStep.relayMask != 0) {
        for (int i = 0; i < RELAY_COUNT; i++) {
            if (newStep.relayMask & (1 << i)) {
                requestRelay(i, true, 2);
            }
        }
    }
    publishStatus();
}

void runHybridMode(void) {
    for (int i = 0; i < RELAY_COUNT; i++) {
        automationOwns[i] = false;
    }
    automationActive = false;
    for (int i = 0; i < MAX_AUTO_RULES; i++) {
        if (!autoRules[i].enabled) {
            continue;
        }
        if (autoRules[i].relayId < 1 || autoRules[i].relayId > RELAY_COUNT) {
            continue;
        }
        int8_t nodeIndex = findNodeById(autoRules[i].nodeId);
        if (nodeIndex < 0 || !nodes[nodeIndex].active) {
            continue;
        }
        if (millis() - nodes[nodeIndex].lastSeen > 60000UL) {
            continue;
        }
        float temp = nodes[nodeIndex].lastTemperature;
        float hum = nodes[nodeIndex].lastHumidity;
        bool tempShouldActivate = false;
        bool humShouldActivate = false;
        if (autoRules[i].useTempControl) {
            if (temp > autoRules[i].tempMax) {
                tempShouldActivate = true;
            } else if (temp < autoRules[i].tempMin) {
                tempShouldActivate = false;
            } else {
                tempShouldActivate = autoRules[i].lastState;
            }
        }
        if (autoRules[i].useHumControl) {
            if (hum > autoRules[i].humMax) {
                humShouldActivate = true;
            } else if (hum < autoRules[i].humMin) {
                humShouldActivate = false;
            } else {
                humShouldActivate = autoRules[i].lastState;
            }
        }
        bool shouldActivate = false;
        if (autoRules[i].useTempControl && autoRules[i].useHumControl) {
            shouldActivate = autoRules[i].logicOR ? (tempShouldActivate || humShouldActivate)
                                                  : (tempShouldActivate && humShouldActivate);
        } else if (autoRules[i].useTempControl) {
            shouldActivate = tempShouldActivate;
        } else if (autoRules[i].useHumControl) {
            shouldActivate = humShouldActivate;
        }
        if (shouldActivate != autoRules[i].lastState) {
            if (millis() - autoRules[i].lastStateChange >= 3000 || autoRules[i].lastStateChange == 0) {
                autoRules[i].lastState = shouldActivate;
                autoRules[i].lastStateChange = millis();
            }
        }
        if (autoRules[i].lastState) {
            automationOwns[autoRules[i].relayId - 1] = true;
            automationActive = true;
        }
    }
    for (int i = 0; i < MAX_AUTO_RULES; i++) {
        if (!autoRules[i].enabled) {
            continue;
        }
        uint8_t relayIdx = autoRules[i].relayId - 1;
        if (relayIdx >= RELAY_COUNT) {
            continue;
        }
        if (automationOwns[relayIdx]) {
            hybridRelayStates[relayIdx] = true;
            if (!relayStates[relayIdx]) {
                requestRelay(relayIdx, true, 3);
            }
        }
    }
    queueActive = false;
    if (queueRunning && queueConfig.stepCount > 0 && !queuePaused) {
        unsigned long elapsed = (millis() - queueStepStartTime) / 1000;
        QueueStep &currentStep = queueConfig.steps[currentQueueStep];
        if (elapsed >= currentStep.duration) {
            for (int i = 0; i < RELAY_COUNT; i++) {
                if ((currentStep.relayMask & (1 << i)) && !automationOwns[i]) {
                    requestRelay(i, false, 3);
                    hybridRelayStates[i] = false;
                }
            }
            currentQueueStep++;
            if (currentQueueStep >= queueConfig.stepCount) {
                if (queueConfig.loop) {
                    currentQueueStep = 0;
                } else {
                    queueRunning = false;
                    publishStatus();
                    return;
                }
            }
            queueStepStartTime = millis();
            QueueStep &newStep = queueConfig.steps[currentQueueStep];
            if (newStep.relayMask != 0) {
                for (int i = 0; i < RELAY_COUNT; i++) {
                    if ((newStep.relayMask & (1 << i)) && !automationOwns[i]) {
                        requestRelay(i, true, 3);
                        hybridRelayStates[i] = true;
                        queueActive = true;
                    }
                }
            }
            publishStatus();
        } else if (currentStep.relayMask != 0) {
            for (int i = 0; i < RELAY_COUNT; i++) {
                if ((currentStep.relayMask & (1 << i)) && !automationOwns[i]) {
                    if (!relayStates[i]) {
                        requestRelay(i, true, 3);
                        hybridRelayStates[i] = true;
                    }
                    queueActive = true;
                }
            }
        }
    }
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (!automationOwns[i] && relayStates[i]) {
            bool queueWantsIt = false;
            if (queueRunning && !queuePaused && queueConfig.stepCount > 0) {
                QueueStep &currentStep = queueConfig.steps[currentQueueStep];
                if (currentStep.relayMask & (1 << i)) {
                    queueWantsIt = true;
                }
            }
            if (!queueWantsIt && hybridRelayStates[i]) {
                requestRelay(i, false, 3);
                hybridRelayStates[i] = false;
            }
        }
    }
}

uint8_t automationReleaseMask(void) {
    uint8_t mask = 0;
    if (operationMode != MODE_AUTOMATION && operationMode != MODE_HYBRID) return 0;
    for (int i = 0; i < MAX_AUTO_RULES; ++i) {
        if (!autoRules[i].enabled || !autoRules[i].lastState) continue;
        if (autoRules[i].relayId < 1 || autoRules[i].relayId > RELAY_COUNT) continue;
        mask |= (uint8_t)(1u << (autoRules[i].relayId - 1));
    }
    return mask;
}

void applyAutomationRelease(uint8_t mask) {
    for (uint8_t i = 0; i < RELAY_COUNT; ++i) {
        if ((mask & (uint8_t)(1u << i)) == 0) continue;
        hybridRelayStates[i] = false;
        requestRelay(i, false, operationMode);
    }
}

void pauseQueue(void) {
    if (!queueRunning || queuePaused) return;
    s_queuePauseStartedMs = millis();
    queuePaused = true;
}

void resumeQueue(void) {
    if (!queueRunning || !queuePaused) return;
    queueStepStartTime += millis() - s_queuePauseStartedMs;
    queuePaused = false;
}

void transitionToMode(uint8_t newMode, bool graceful) {
    if (newMode == operationMode) {
        return;
    }
    if (graceful && operationMode == MODE_QUEUE && queueRunning) {
        queueConfig.loop = false;
        s_pendingMode = (int8_t)newMode;
        return;
    }
    s_pendingMode = -1;
    const uint8_t oldMode = operationMode;
    if (operationMode == MODE_QUEUE || operationMode == MODE_HYBRID) {
        queueRunning = false;
        queuePaused = false;
    }
    operationMode = newMode;
    if (oldMode != MODE_MANUAL || operationMode != MODE_MANUAL) {
        requestAllRelays(false, operationMode);
    }
    if ((operationMode == MODE_QUEUE || operationMode == MODE_HYBRID) && queueConfig.stepCount > 0) {
        queueRunning = true;
        queuePaused = false;
        queueConfig.autoStart = true;
        currentQueueStep = 0;
        queueStepStartTime = millis();
        QueueStep &firstStep = queueConfig.steps[0];
        if (firstStep.relayMask != 0) {
            for (int i = 0; i < RELAY_COUNT; i++) {
                if (firstStep.relayMask & (1 << i)) {
                    if (operationMode == MODE_QUEUE) {
                        requestRelay(i, true, 2);
                    } else {
                        queueActive = true;
                        hybridRelayStates[i] = true;
                    }
                }
            }
        }
        saveQueueConfig();
    }
    control_states[4] = newMode;
}

void processControlRequest(uint8_t buttonId, uint8_t requestedState) {
    switch (buttonId) {
        case 5:
            transitionToMode(requestedState, false);
            break;
        case 6:
            if (requestedState == 1) {
                enablePairingMode();
            } else if (requestedState == 0) {
                disablePairingMode();
            } else if (requestedState == 2) {
                resetPairedNodes();
            }
            control_states[buttonId - 1] = requestedState;
            break;
        case 7:
            if (requestedState == 1) {
                resetWiFiCredentials();
            }
            control_states[buttonId - 1] = requestedState;
            break;
        default:
            break;
    }
    if (operationMode == MODE_MANUAL) {
        /* IDs 5-7 are mode, pairing, and Wi-Fi on this queue. Relays 5-7
         * are commanded from MQTT and the HMI, which use requestRelay(). */
        if (buttonId >= 1 && buttonId <= 4) {
            requestRelay(buttonId - 1, requestedState != 0, 0);
            relayWaitForIdle(200);
            saveManualModeConfig();
        }
    }
}

void relayTask(void *pvParameters) {
    (void)pvParameters;
    RelayCommand cmd;
    for (;;) {
        while (xQueueReceive(relayQueue, &cmd, 0) == pdTRUE) {
            if (xSemaphoreTake(relayMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (cmd.relayId == 0xFF) {
                    for (int i = 0; i < RELAY_COUNT; i++) {
                        applyRelayLocked(i, cmd.state, cmd.triggerSource);
                    }
                } else {
                    applyRelayLocked(cmd.relayId, cmd.state, cmd.triggerSource);
                }
                xSemaphoreGive(relayMutex);
            } else {
                diagnosticsLog("relay/apply", "FAIL mutex timeout");
            }
            relayCommandApplied();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void sensorTask(void *pvParameters) {
    (void)pvParameters;
    SensorQueueItem item;
    for (;;) {
        if (xQueueReceive(sensorQueue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (xSemaphoreTake(nodesMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (item.type == SENSOR_EVT_JOINED || item.type == SENSOR_EVT_LIST) {
            int8_t idx = findNodeByIeee(item.ieee);
            if (idx < 0 && item.nodeId >= 1) {
                idx = findNodeById(item.nodeId);
            }
            /* The hub NVS is the recovery source after an S3 reboot. If the
             * local EEPROM record was interrupted or erased, restore this
             * known hub node instead of deleting it as a "stale" entry. */
            const bool restoreFromHub = item.type == SENSOR_EVT_LIST;
            bool added = false;
            if (idx < 0 && nodeCount < MAX_NODES &&
                (item.type == SENSOR_EVT_JOINED || restoreFromHub) &&
                item.nodeId >= 1 && item.nodeId <= MAX_NODES) {
                idx = (int8_t)nodeCount;
                nodeCount++;
                memset(&nodes[idx], 0, sizeof(nodes[idx]));
                nodes[idx].nodeId = item.nodeId;
                nodes[idx].batteryPercent = BATTERY_PCT_UNKNOWN;
                added = true;
            }
            if (idx >= 0) {
                if (item.nodeId >= 1 && item.nodeId <= MAX_NODES) {
                    nodes[idx].nodeId = item.nodeId;
                }
                memcpy(nodes[idx].ieee, item.ieee, 8);
                nodes[idx].shortAddr = item.shortAddr;
                nodes[idx].lqi = item.lqi;
                nodes[idx].active = true;
                nodes[idx].lastSeen = millis();
                if (item.type == SENSOR_EVT_LIST && item.hasReading) {
                    nodes[idx].lastTemperature = item.temperature;
                    nodes[idx].lastHumidity = item.humidity;
                    nodes[idx].batteryPercent = item.batteryPercent;
                    if (nodes[idx].nodeId >= 1 && nodes[idx].nodeId <= MAX_NODES) {
                        temps[nodes[idx].nodeId - 1] = item.temperature;
                        hums[nodes[idx].nodeId - 1] = item.humidity;
                    }
                }
            }
            const bool restored = idx >= 0 && item.type == SENSOR_EVT_LIST && item.hasReading;
            const uint8_t restoredId = restored ? nodes[idx].nodeId : 0;
            const float restoredTemp = restored ? item.temperature : 0;
            const float restoredHum = restored ? item.humidity : 0;
            const uint8_t restoredBattery = restored ? item.batteryPercent : BATTERY_PCT_UNKNOWN;
            xSemaphoreGive(nodesMutex);
            if (item.type == SENSOR_EVT_JOINED || added) {
                diagnosticsLog("sensor", "joined node=%u short=0x%04X",
                               item.nodeId, item.shortAddr);
                s_nodeReadingSaveMs = millis();
                saveNodesToEEPROM();
                publishStatus();
            } else if (restored) {
                if (restoredId >= 1 && restoredId <= MAX_NODES) {
                    publishSensorData(restoredId, restoredTemp, restoredHum, restoredBattery);
                }
                publishStatus();
                saveNodeReadings();
            }
            continue;
        }
        if (item.type == SENSOR_EVT_LEFT) {
            /* Leave announcements do not unpair a node. FORGET and FORGET ALL
             * are the only paths that remove one. */
            xSemaphoreGive(nodesMutex);
            diagnosticsLog("sensor", "ignored leave for node=%u; still paired",
                           item.nodeId);
            continue;
        }
        bool adopted = false;
        int8_t idx = findNodeByIeee(item.ieee);
        if (idx < 0) {
            idx = findNodeById(item.nodeId);
        }
        if (idx < 0 && item.nodeId >= 1 && item.nodeId <= MAX_NODES &&
            nodeCount < MAX_NODES) {
            idx = (int8_t)nodeCount;
            nodeCount++;
            memset(&nodes[idx], 0, sizeof(nodes[idx]));
            nodes[idx].nodeId = item.nodeId;
            memcpy(nodes[idx].ieee, item.ieee, 8);
            nodes[idx].batteryPercent = BATTERY_PCT_UNKNOWN;
            adopted = true;
        }
        if (idx < 0) {
            xSemaphoreGive(nodesMutex);
            diagnosticsLog("sensor", "dropped data node=%u (table full)", item.nodeId);
            continue;
        }
        nodes[idx].lastTemperature = item.temperature;
        nodes[idx].lastHumidity = item.humidity;
        nodes[idx].batteryPercent = item.batteryPercent;
        nodes[idx].lastSeen = millis();
        nodes[idx].active = true;
        nodes[idx].lqi = item.lqi;
        if (item.shortAddr) {
            nodes[idx].shortAddr = item.shortAddr;
        }
        uint8_t nid = nodes[idx].nodeId;
        if (nid >= 1 && nid <= MAX_NODES) {
            temps[nid - 1] = item.temperature;
            hums[nid - 1] = item.humidity;
        }
        xSemaphoreGive(nodesMutex);
        if (adopted) {
            diagnosticsLog("sensor", "adopted node=%u from sensor data", nid);
            s_nodeReadingSaveMs = millis();
            saveNodesToEEPROM();
            publishStatus();
        } else {
            saveNodeReadings();
        }
        publishSensorData(nid, item.temperature, item.humidity, item.batteryPercent);
    }
}

void modeTask(void *pvParameters) {
    (void)pvParameters;
    unsigned long lastDisplayTime = 0;
    unsigned long lastDisplayHeartbeat = 0;
    for (;;) {
        /* MqttCmdItem is 3 KB and modeTask is its only consumer; keeping it in
         * .bss leaves the stack free for the command handlers below. */
        static MqttCmdItem cmd;
        while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
            processMqttCommandPayload(cmd.payload, cmd.length);
        }
        ControlRequest req;
        while (xQueueReceive(controlQueue, &req, 0) == pdTRUE) {
            processControlRequest(req.buttonId, req.requestedState);
        }
        displayProcessPendingCommands();
        uint8_t currentMode = operationMode;
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            currentMode = operationMode;
            xSemaphoreGive(stateMutex);
        }
        switch (currentMode) {
            case MODE_MANUAL:
                runManualMode();
                break;
            case MODE_AUTOMATION:
                runAutomationMode();
                break;
            case MODE_QUEUE:
                runQueueMode();
                break;
            case MODE_HYBRID:
                runHybridMode();
                break;
            default:
                break;
        }
        if (millis() - lastDisplayHeartbeat >= DISPLAY_LINK_HEARTBEAT_MS) {
            displaySendHeartbeat();
            lastDisplayHeartbeat = millis();
        }
        /* A waiting touch command owns the link. The periodic snapshot is
         * sent on the next free pass, and forced out if it is 10 s late. */
        if (millis() - lastDisplayTime >= DISPLAY_LINK_SNAPSHOT_REFRESH_MS) {
            if (displayHasPendingCommands() &&
                millis() - lastDisplayTime < DISPLAY_LINK_SNAPSHOT_REFRESH_MS * 2UL) {
                displayProcessPendingCommands();
            } else {
                displaySendSnapshot();
                lastDisplayTime = millis();
            }
        }
        displayProcessPendingCommands();
        if (pairingMode && (millis() - pairingModeStartTime >= PAIRING_MODE_DURATION_MS)) {
            disablePairingMode();
        }
        checkButtonPress();
        vTaskDelay(pdMS_TO_TICKS(displayHasPendingCommands() ? 1 : 50));
    }
}
