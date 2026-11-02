#include "mqtt_service.h"
#include "mqtt_commands.h"
#include "app_state.h"
#include "rtos.h"
#include "config.h"
#include "event_log.h"
#include "diagnostics.h"
#include "relay_control.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Arduino.h>
#include <cstdlib>
#include <cstring>

static WiFiClient espClient;
static PubSubClient mqttClient(espClient);

bool mqttIsConnected(void) {
    return mqttClient.connected();
}

String getCurrentTimestamp(void) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) {
        return "N/A";
    }
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(timestamp);
}

void processPendingNtp(void) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    unsigned long now = millis();
    if (timeSync && (now - lastNTPSync < NTP_SYNC_INTERVAL_MS)) {
        return;
    }
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 50)) {
        timeSync = true;
        lastNTPSync = now;
    }
}

void mqttEnqueuePublish(const char *topic, const char *payload, bool retain) {
    if (!mqttPublishQueue || !topic || !payload) {
        return;
    }
    /* MqttPublishItem is over 3 KB. Callers run on modeTask, otaTask and
     * linkTestTask, whose stacks cannot absorb it, so stage the copy on the
     * heap; xQueueSend takes its own copy before this returns. */
    MqttPublishItem *item = (MqttPublishItem *)calloc(1, sizeof(MqttPublishItem));
    if (!item) {
        return;
    }
    strncpy(item->topic, topic, sizeof(item->topic) - 1);
    strncpy(item->payload, payload, sizeof(item->payload) - 1);
    item->retain = retain;
    xQueueSend(mqttPublishQueue, item, pdMS_TO_TICKS(50));
    free(item);
}

static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    (void)topic;
    /* Only mqttTask dispatches PubSubClient callbacks, so one static staging
     * item keeps this 3 KB copy off the stack. */
    static MqttCmdItem item;
    memset(&item, 0, sizeof(item));
    unsigned int n = length;
    if (n >= MQTT_CMD_PAYLOAD_MAX) {
        n = MQTT_CMD_PAYLOAD_MAX - 1;
    }
    memcpy(item.payload, payload, n);
    item.payload[n] = 0;
    item.length = n;
    if (xQueueSend(cmdQueue, &item, pdMS_TO_TICKS(20)) != pdTRUE) {
        diagnosticsLog("mqttRx", "FAIL command queue full bytes=%u", n);
    } else {
        diagnosticsLog("mqttRx", "command queued bytes=%u depth=%u", n,
                       (unsigned)uxQueueMessagesWaiting(cmdQueue));
    }
}

void mqttServiceBegin(void) {
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    /* PubSubClient waits for CONNACK, and for the rest of a partial packet, in
     * a tight loop that never yields. mqttTask outranks IDLE0, so a wait longer
     * than the task watchdog window starves IDLE0 and panics the chip. Keep the
     * socket timeout well under that window; a timed-out attempt just retries. */
    mqttClient.setSocketTimeout(3);
    mqttClient.setBufferSize(4096);
}

void publishSensorData(uint8_t nodeId, float temperature, float humidity, uint8_t batteryPercent) {
    JsonDocument doc;
    doc["node_id"] = nodeId;
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    if (batteryPercent != 0xFF) {
        doc["battery"] = batteryPercent;
    }
    doc["sleep_sec"] = sensorSleepSec;
    doc["timestamp"] = getCurrentTimestamp();
    char buffer[256];
    serializeJson(doc, buffer);
    String topic = deviceMAC + "/node/" + String(nodeId) + "/data";
    mqttEnqueuePublish(topic.c_str(), buffer, false);
}

void publishStatus(void) {
    /* Relay commands are applied by the higher-priority relay task. Wait for
     * commands queued before this snapshot so mode/queue state and GPIO state
     * are reported atomically instead of one transition apart. */
    if (!relayWaitForIdle(150)) {
        diagnosticsLog("mqtt/status", "relay settle timeout");
    }
    JsonDocument doc;
    doc["mode"] = operationMode;
    doc["mode_name"] = (operationMode == MODE_MANUAL) ? "manual" :
                       (operationMode == MODE_AUTOMATION) ? "automation" :
                       (operationMode == MODE_QUEUE) ? "queue" : "hybrid";
    doc["timestamp"] = getCurrentTimestamp();
    doc["uptime_ms"] = millis();
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["wifi_channel"] = WiFi.channel();
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_ssid"] = WiFi.SSID();
        doc["ip_address"] = WiFi.localIP().toString();
    }
    doc["mqtt_connected"] = mqttClient.connected();
    doc["pairing_mode"] = pairingMode;
    doc["paired_nodes"] = nodeCount;
    doc["sensor_sleep_sec"] = sensorSleepSec;
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (int i = 0; i < RELAY_COUNT; i++) {
        relays.add(relayStates[i]);
    }
    JsonArray nodesArray = doc["nodes"].to<JsonArray>();
    for (int i = 0; i < nodeCount; i++) {
        JsonObject node = nodesArray.add<JsonObject>();
        node["id"] = nodes[i].nodeId;
        node["active"] = nodes[i].active;
        node["temperature"] = nodes[i].lastTemperature;
        node["humidity"] = nodes[i].lastHumidity;
        node["last_seen_ms"] = nodes[i].lastSeen ? (millis() - nodes[i].lastSeen) : 0;
        node["lqi"] = nodes[i].lqi;
        if (nodes[i].batteryPercent != 0xFF) {
            node["battery"] = nodes[i].batteryPercent;
        }
        node["short_addr"] = nodes[i].shortAddr;
        char ieeeStr[20];
        formatIeee(nodes[i].ieee, ieeeStr, sizeof(ieeeStr));
        node["ieee"] = ieeeStr;
    }
    if (operationMode == MODE_QUEUE || operationMode == MODE_HYBRID) {
        JsonObject queue = doc["queue"].to<JsonObject>();
        queue["running"] = queueRunning;
        queue["paused"] = queuePaused;
        queue["loop_enabled"] = queueConfig.loop;
        queue["auto_start"] = queueConfig.autoStart;
        queue["total_steps"] = queueConfig.stepCount;
        queue["current_step"] = queueRunning ? (int)currentQueueStep + 1 : 0;
        JsonArray stepsArr = queue["steps"].to<JsonArray>();
        for (int i = 0; i < queueConfig.stepCount; i++) {
            JsonObject step = stepsArr.add<JsonObject>();
            step["step"] = i + 1;
            step["relay_mask"] = queueConfig.steps[i].relayMask;
            step["duration"] = queueConfig.steps[i].duration;
            step["type"] = (queueConfig.steps[i].relayMask == 0) ? "delay" : "relay";
            step["active"] = (queueRunning && !queuePaused && (int)currentQueueStep == i);
        }
    }
    if (operationMode == MODE_AUTOMATION || operationMode == MODE_HYBRID) {
        int enabledCount = 0;
        JsonArray rules = doc["automation_rules"].to<JsonArray>();
        for (int i = 0; i < MAX_AUTO_RULES; i++) {
            if (!autoRules[i].enabled) {
                continue;
            }
            enabledCount++;
            JsonObject rule = rules.add<JsonObject>();
            rule["rule_index"] = i + 1;
            rule["node_id"] = autoRules[i].nodeId;
            rule["relay_id"] = autoRules[i].relayId;
            rule["use_temp"] = autoRules[i].useTempControl;
            rule["use_hum"] = autoRules[i].useHumControl;
            rule["logic"] = autoRules[i].logicOR ? "OR" : "AND";
            rule["last_state"] = autoRules[i].lastState ? "ON" : "OFF";
        }
        doc["automation_rule_count"] = enabledCount;
    }
    if (operationMode == MODE_HYBRID) {
        JsonObject hybrid = doc["hybrid"].to<JsonObject>();
        hybrid["logic"] = hybridLogic ? "OR" : "AND";
        hybrid["automation_active"] = automationActive;
        hybrid["queue_active"] = queueActive;
    }
    JsonObject eventLog = doc["event_log"].to<JsonObject>();
    eventLog["pending_count"] = getEventLogCount();
    eventLog["max_entries"] = EVENT_LOG_MAX_ENTRIES;
    eventLog["topic"] = mqtt_event_log_topic;
    doc["ota_available"] = ota_available;
    doc["firmware_version"] = CURRENT_VERSION;
    char *buffer = (char *)malloc(MQTT_PUB_PAYLOAD_MAX);
    if (!buffer) {
        return;
    }
    serializeJson(doc, buffer, MQTT_PUB_PAYLOAD_MAX);
    mqttEnqueuePublish(mqtt_status_topic.c_str(), buffer, true);
    free(buffer);
}

void mqttTask(void *pvParameters) {
    (void)pvParameters;
    unsigned long lastStatus = 0;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                wifi_flag = 1;
                mqtt_flag = 0;
                xSemaphoreGive(stateMutex);
            }
            WiFi.reconnect();
            diagnosticsLog("mqttTask", "Wi-Fi offline; reconnect requested");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (wifi_flag != 2) {
            wifi_flag = 2;
        }
        processPendingNtp();
        if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!mqttClient.connected()) {
                mqtt_flag = 0;
                bool connected = mqttClient.connect(mqtt_client_id.c_str(), MQTT_USER, MQTT_PASSWORD);
                if (connected) {
                    diagnosticsLog("mqttTask", "broker connected; subscribing");
                    mqtt_flag = 1;
                    mqttClient.subscribe(mqtt_command_topic.c_str());
                    xSemaphoreGive(mqttMutex);
                    if (getEventLogCount() > 0) {
                        sendEventLogs();
                    }
                    publishStatus();
                } else {
                    diagnosticsLog("mqttTask", "FAIL broker connect state=%d",
                                   mqttClient.state());
                    xSemaphoreGive(mqttMutex);
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            } else {
                mqttClient.loop();
                xSemaphoreGive(mqttMutex);
            }
        }
        MqttPublishItem item;
        while (xQueueReceive(mqttPublishQueue, &item, 0) == pdTRUE) {
            if (xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (mqttClient.connected()) {
                    const bool published =
                        mqttClient.publish(item.topic, item.payload, item.retain);
                    diagnosticsLog("mqttTask", "publish topic=%s bytes=%u result=%s",
                                   item.topic, (unsigned)strlen(item.payload),
                                   published ? "ok" : "FAIL");
                }
                xSemaphoreGive(mqttMutex);
            }
        }
        if (millis() - lastStatus >= STATUS_PUBLISH_INTERVAL_MS) {
            publishStatus();
            lastStatus = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
