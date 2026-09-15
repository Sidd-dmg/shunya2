#include "config.h"
#include "app_state.h"
#include "rtos.h"
#include "persistence.h"
#include "event_log.h"
#include "relay_control.h"
#include "display_link.h"
#include "hub_link.h"
#include "mqtt_service.h"
#include "wifi_prov.h"
#include "ota_service.h"
#include "mode_engine.h"
#include "link_test.h"
#include "diagnostics.h"
#include "display_protocol.h"
#include <WiFi.h>
#include <Arduino.h>

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== ESP-B Gateway (Zigbee hub + FreeRTOS) ===");
    Serial.print("Firmware Version: ");
    Serial.println(CURRENT_VERSION);
    if (!display_protocol_self_test()) {
        Serial.println("FATAL: display protocol self-test failed");
        ESP.restart();
    }

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    relayInitPins();
    displayBegin();
    hubBegin();

    WiFi.mode(WIFI_STA);
    deviceMAC = WiFi.macAddress();
    deviceMAC.replace(":", "");
    mqtt_client_id = deviceMAC;
    mqtt_command_topic = deviceMAC + "/command";
    mqtt_status_topic = deviceMAC + "/status";
    mqtt_event_log_topic = deviceMAC + "/event_log";
    provServiceName = "Shunya " + WiFi.macAddress();

    persistenceBegin();
    loadNodesFromEEPROM();
    initializeAutomationRules();
    initializeQueueConfig();
    loadConfigFromEEPROM();
    initSPIFFS();

    setupWiFi();
    mqttServiceBegin();

    if (operationMode == MODE_AUTOMATION) {
        for (int i = 0; i < 4; i++) {
            digitalWrite(relayPins[i], LOW);
            relayStates[i] = false;
        }
    } else if (operationMode == MODE_QUEUE && queueConfig.stepCount > 0 && queueConfig.autoStart) {
        queueRunning = true;
        queuePaused = false;
        currentQueueStep = 0;
        queueStepStartTime = millis();
    }

    control_states[0] = relayStates[0] ? 1 : 0;
    control_states[1] = relayStates[1] ? 1 : 0;
    control_states[2] = relayStates[2] ? 1 : 0;
    control_states[3] = relayStates[3] ? 1 : 0;
    control_states[4] = operationMode;

    if (!rtosCreatePrimitives()) {
        Serial.println("FATAL: FreeRTOS primitive creation failed");
        ESP.restart();
    }

    xTaskCreatePinnedToCore(mqttTask, "mqttTask", 12288, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(otaTask, "otaTask", 8192, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(hubTxTask, "hubTx", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(hubRxTask, "hubRx", 6144, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(sensorTask, "sensor", 6144, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(displayRxTask, "displayRx", 6144, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(relayTask, "relay", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(modeTask, "mode", 8192, NULL, 2, NULL, 1);
    linkTestBegin();
    diagnosticsBegin();
    xTaskCreatePinnedToCore(linkTestTask, "linkTest", 6144, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(diagnosticsTask, "diagnostics", 4096, NULL, 1, NULL, 0);
    hubRequestSetSleep(sensorSleepSec);

    if (operationMode == MODE_QUEUE && queueRunning && queueConfig.stepCount > 0) {
        QueueStep &firstStep = queueConfig.steps[0];
        if (firstStep.relayMask != 0) {
            for (int i = 0; i < 4; i++) {
                if (firstStep.relayMask & (1 << i)) {
                    requestRelay(i, true, 2);
                }
            }
        }
    }

    markFirmwareValid();
    Serial.println("Tasks started. Command topic: " + mqtt_command_topic);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
