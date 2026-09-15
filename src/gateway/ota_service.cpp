#include "ota_service.h"
#include "app_state.h"
#include "rtos.h"
#include "config.h"
#include "relay_control.h"
#include "mqtt_service.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <ArduinoJson.h>
#include <Arduino.h>

static WiFiClientSecure secureClient;

void requestOtaUpdate(void) {
    if (otaTriggerSemaphore) {
        xSemaphoreGive(otaTriggerSemaphore);
    }
}

void requestOtaCheck(void) {
    if (otaCheckSemaphore) {
        xSemaphoreGive(otaCheckSemaphore);
    }
}

static bool checkNewVersion(String &newVersion) {
    HTTPClient http;
    secureClient.setInsecure();
    http.begin(secureClient, VERSION_URL);
    http.setTimeout(10000);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        newVersion = http.getString();
        newVersion.trim();
        http.end();
        return newVersion != CURRENT_VERSION && newVersion.length() > 0;
    }
    http.end();
    return false;
}

static void checkForOTAUpdate(void) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    String newVersion;
    if (checkNewVersion(newVersion)) {
        ota_available = 1;
        JsonDocument doc;
        doc["ota_available"] = true;
        doc["current_version"] = CURRENT_VERSION;
        doc["new_version"] = newVersion;
        char buffer[256];
        serializeJson(doc, buffer);
        mqttEnqueuePublish((deviceMAC + "/ota/status").c_str(), buffer, true);
    } else {
        ota_available = 0;
    }
}

static void performOTAUpdate(void) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    otaInProgress = true;
    requestAllRelays(false, 0);
    HTTPClient http;
    secureClient.setInsecure();
    http.begin(secureClient, OTA_URL);
    http.setTimeout(30000);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        http.end();
        otaInProgress = false;
        return;
    }
    int contentLength = http.getSize();
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        http.end();
        otaInProgress = false;
        Serial.println("No OTA partition found!");
        return;
    }
    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(update_partition, contentLength, &ota_handle) != ESP_OK) {
        http.end();
        otaInProgress = false;
        return;
    }
    WiFiClient *stream = http.getStreamPtr();
    size_t written = 0;
    uint8_t buff[512] = {0};
    while (http.connected() && written < (size_t)contentLength) {
        size_t available = stream->available();
        if (available) {
            size_t toRead = available < sizeof(buff) ? available : sizeof(buff);
            int bytesRead = stream->readBytes(buff, toRead);
            if (esp_ota_write(ota_handle, buff, bytesRead) != ESP_OK) {
                esp_ota_abort(ota_handle);
                http.end();
                otaInProgress = false;
                return;
            }
            written += bytesRead;
        }
        vTaskDelay(1);
    }
    http.end();
    if (written != (size_t)contentLength || esp_ota_end(ota_handle) != ESP_OK) {
        otaInProgress = false;
        return;
    }
    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        otaInProgress = false;
        return;
    }
    mqttEnqueuePublish((deviceMAC + "/ota/status").c_str(),
                       "{\"ota_status\":\"success\"}", true);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
}

void markFirmwareValid(void) {
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            mqttEnqueuePublish((deviceMAC + "/ota/status").c_str(),
                               "{\"ota_status\":\"verified\"}", true);
        }
    }
}

void otaTask(void *pvParameters) {
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (WiFi.status() == WL_CONNECTED) {
        checkForOTAUpdate();
    }
    unsigned long lastOTACheck = millis();
    for (;;) {
        if (xSemaphoreTake(otaTriggerSemaphore, pdMS_TO_TICKS(200)) == pdTRUE) {
            performOTAUpdate();
        }
        if (xSemaphoreTake(otaCheckSemaphore, 0) == pdTRUE) {
            checkForOTAUpdate();
            lastOTACheck = millis();
        }
        if (millis() - lastOTACheck >= OTA_CHECK_INTERVAL_MS) {
            if (WiFi.status() == WL_CONNECTED) {
                checkForOTAUpdate();
            }
            lastOTACheck = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
