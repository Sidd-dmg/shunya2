#include "display_state.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>

static SemaphoreHandle_t s_mutex;
static display_snapshot_t s_snapshot;
static display_ack_t s_ack;
static display_hmi_ota_status_t s_ota_status;
static bool s_has_snapshot;
static bool s_link;
static bool s_has_ack;
static bool s_has_ota_status;
static uint32_t s_last_rx;

void displayStateBegin() {
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_snapshot, 0, sizeof(s_snapshot));
}

void displayStateSetSnapshot(const display_snapshot_t &snapshot) {
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_snapshot = snapshot;
        s_has_snapshot = true;
        s_link = true;
        s_last_rx = millis();
        xSemaphoreGive(s_mutex);
    }
}

bool displayStateGetSnapshot(display_snapshot_t &snapshot) {
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool available = s_has_snapshot;
    if (available) {
        snapshot = s_snapshot;
        s_has_snapshot = false;
    }
    xSemaphoreGive(s_mutex);
    return available;
}

void displayStateSetLink(bool connected) {
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_link = connected;
        if (connected) {
            s_last_rx = millis();
        }
        xSemaphoreGive(s_mutex);
    }
}

bool displayStateLinkConnected() {
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool connected = s_link && (millis() - s_last_rx < 8000UL);
    xSemaphoreGive(s_mutex);
    return connected;
}

uint32_t displayStateLastRxMs() {
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    const uint32_t value = s_last_rx;
    xSemaphoreGive(s_mutex);
    return value;
}

void displayStateSetAck(const display_ack_t &ack) {
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_ack = ack;
        s_has_ack = true;
        s_link = true;
        s_last_rx = millis();
        xSemaphoreGive(s_mutex);
    }
}

bool displayStateTakeAck(display_ack_t &ack) {
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool available = s_has_ack;
    if (available) {
        ack = s_ack;
        s_has_ack = false;
    }
    xSemaphoreGive(s_mutex);
    return available;
}

void displayStateSetOtaStatus(const display_hmi_ota_status_t &status) {
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_ota_status = status;
        s_has_ota_status = true;
        xSemaphoreGive(s_mutex);
    }
}

bool displayStateTakeOtaStatus(display_hmi_ota_status_t &status) {
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    const bool available = s_has_ota_status;
    if (available) {
        status = s_ota_status;
        s_has_ota_status = false;
    }
    xSemaphoreGive(s_mutex);
    return available;
}
