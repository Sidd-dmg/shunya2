#include "event_log.h"
#include "app_state.h"
#include "rtos.h"
#include "mqtt_service.h"
#include "config.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <cstdlib>

#define EVENT_LOG_BATCH_BUFFER 4096

void initSPIFFS(void) {
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed! Formatting...");
        SPIFFS.format();
        if (!SPIFFS.begin(true)) {
            Serial.println("SPIFFS mount failed after format!");
            return;
        }
    }
    Serial.printf("SPIFFS Total: %d bytes, Used: %d bytes\n",
                  SPIFFS.totalBytes(), SPIFFS.usedBytes());
    if (SPIFFS.exists("/evt_tmp.jsonl")) {
        SPIFFS.remove("/evt_tmp.jsonl");
    }
}

uint32_t getEventLogCount(void) {
    if (!SPIFFS.exists(EVENT_LOG_FILE)) {
        return 0;
    }
    File f = SPIFFS.open(EVENT_LOG_FILE, "r");
    if (!f) {
        return 0;
    }
    uint32_t count = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            count++;
        }
    }
    f.close();
    return count;
}

void logRelayEvent(uint8_t relayId, bool state, uint8_t triggerSource) {
    if (mqttIsConnected()) {
        return;
    }
    if (spiffsMutex && xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    uint32_t count = getEventLogCount();
    if (count >= EVENT_LOG_MAX_ENTRIES) {
        const char *tmpFile = "/evt_tmp.jsonl";
        File fRead = SPIFFS.open(EVENT_LOG_FILE, "r");
        File fWrite = SPIFFS.open(tmpFile, "w");
        if (fRead && fWrite) {
            bool firstLine = true;
            while (fRead.available()) {
                String line = fRead.readStringUntil('\n');
                line.trim();
                if (line.length() == 0) {
                    continue;
                }
                if (firstLine) {
                    firstLine = false;
                    continue;
                }
                fWrite.println(line);
            }
        }
        if (fRead) {
            fRead.close();
        }
        if (fWrite) {
            fWrite.close();
        }
        SPIFFS.remove(EVENT_LOG_FILE);
        SPIFFS.rename(tmpFile, EVENT_LOG_FILE);
    }

    const char *modeNames[] = {"manual", "automation", "queue", "hybrid"};
    const char *sourceNames[] = {"manual", "automation", "queue", "hybrid"};
    JsonDocument doc;
    doc["relay_id"] = relayId + 1;
    doc["state"] = state ? "ON" : "OFF";
    doc["mode"] = modeNames[operationMode];
    doc["trigger"] = sourceNames[triggerSource > 3 ? 0 : triggerSource];
    doc["timestamp"] = getCurrentTimestamp();
    doc["uptime_ms"] = millis();
    JsonArray nodesArr = doc["nodes"].to<JsonArray>();
    for (int i = 0; i < nodeCount; i++) {
        if (nodes[i].active) {
            JsonObject n = nodesArr.add<JsonObject>();
            n["id"] = nodes[i].nodeId;
            n["temp"] = nodes[i].lastTemperature;
            n["hum"] = nodes[i].lastHumidity;
        }
    }
    char buffer[512];
    serializeJson(doc, buffer);
    File f = SPIFFS.open(EVENT_LOG_FILE, "a");
    if (f) {
        f.println(buffer);
        f.close();
    }
    if (spiffsMutex) {
        xSemaphoreGive(spiffsMutex);
    }
}

void sendEventLogs(void) {
    if (!mqttIsConnected()) {
        return;
    }
    if (spiffsMutex && xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    if (!SPIFFS.exists(EVENT_LOG_FILE)) {
        mqttEnqueuePublish(mqtt_event_log_topic.c_str(), "{\"status\":\"no_logs\"}", false);
        if (spiffsMutex) {
            xSemaphoreGive(spiffsMutex);
        }
        return;
    }
    uint32_t totalCount = getEventLogCount();
    JsonDocument startDoc;
    startDoc["status"] = "sending";
    startDoc["total_count"] = totalCount;
    startDoc["batch_size"] = EVENT_LOG_BATCH_SIZE;
    char startBuf[128];
    serializeJson(startDoc, startBuf);
    mqttEnqueuePublish(mqtt_event_log_topic.c_str(), startBuf, false);

    File f = SPIFFS.open(EVENT_LOG_FILE, "r");
    if (!f) {
        if (spiffsMutex) {
            xSemaphoreGive(spiffsMutex);
        }
        return;
    }
    /* Batches serialize to several kilobytes, and modeTask reaches this from
     * both the display and MQTT command paths. Keep them off its stack. */
    char *buffer = (char *)malloc(EVENT_LOG_BATCH_BUFFER);
    if (!buffer) {
        f.close();
        if (spiffsMutex) {
            xSemaphoreGive(spiffsMutex);
        }
        return;
    }
    uint32_t sentCount = 0;
    uint32_t batchCount = 0;
    int batchIndex = 0;
    JsonDocument batchDoc;
    JsonArray batchArr = batchDoc["logs"].to<JsonArray>();
    batchDoc["total"] = totalCount;

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) {
            continue;
        }
        JsonDocument entryDoc;
        if (deserializeJson(entryDoc, line)) {
            continue;
        }
        batchArr.add(entryDoc.as<JsonObject>());
        batchCount++;
        sentCount++;
        if (batchCount >= EVENT_LOG_BATCH_SIZE) {
            batchDoc["batch_index"] = batchIndex++;
            batchDoc["batch_count"] = batchCount;
            serializeJson(batchDoc, buffer, EVENT_LOG_BATCH_BUFFER);
            mqttEnqueuePublish(mqtt_event_log_topic.c_str(), buffer, false);
            batchDoc.clear();
            batchArr = batchDoc["logs"].to<JsonArray>();
            batchDoc["total"] = totalCount;
            batchCount = 0;
        }
    }
    f.close();
    if (batchCount > 0) {
        batchDoc["batch_index"] = batchIndex;
        batchDoc["batch_count"] = batchCount;
        serializeJson(batchDoc, buffer, EVENT_LOG_BATCH_BUFFER);
        mqttEnqueuePublish(mqtt_event_log_topic.c_str(), buffer, false);
    }
    free(buffer);
    JsonDocument done;
    done["status"] = "complete";
    done["sent_count"] = sentCount;
    char doneBuf[128];
    serializeJson(done, doneBuf);
    mqttEnqueuePublish(mqtt_event_log_topic.c_str(), doneBuf, false);
    if (spiffsMutex) {
        xSemaphoreGive(spiffsMutex);
    }
}

void clearEventLogs(void) {
    if (spiffsMutex && xSemaphoreTake(spiffsMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    if (SPIFFS.exists(EVENT_LOG_FILE)) {
        SPIFFS.remove(EVENT_LOG_FILE);
    }
    if (spiffsMutex) {
        xSemaphoreGive(spiffsMutex);
    }
    mqttEnqueuePublish(mqtt_event_log_topic.c_str(), "{\"status\":\"cleared\"}", false);
}
