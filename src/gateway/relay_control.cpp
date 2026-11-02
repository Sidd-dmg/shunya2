#include "relay_control.h"
#include "app_state.h"
#include "rtos.h"
#include "event_log.h"
#include "diagnostics.h"
#include <Arduino.h>
#include <atomic>

namespace {
std::atomic<uint32_t> s_relayCommandsQueued{0};
std::atomic<uint32_t> s_relayCommandsApplied{0};
}

void relayInitPins(void) {
    for (int i = 0; i < RELAY_COUNT; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], LOW);
        relayStates[i] = false;
    }
}

void requestRelay(uint8_t relayId, bool state, uint8_t triggerSource) {
    if (relayId >= RELAY_COUNT || relayQueue == NULL) {
        diagnosticsLog("relay/request", "FAIL invalid relay=%u queue=%p",
                       relayId, relayQueue);
        return;
    }
    RelayCommand cmd;
    cmd.relayId = relayId;
    cmd.state = state;
    cmd.triggerSource = triggerSource;
    if (xQueueSend(relayQueue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE) {
        diagnosticsLog("relay/request", "FAIL queue full relay=%u state=%u",
                       relayId + 1, state);
    } else {
        s_relayCommandsQueued.fetch_add(1, std::memory_order_release);
        diagnosticsLog("relay/request", "queued relay=%u state=%u source=%u",
                       relayId + 1, state, triggerSource);
    }
}

void requestAllRelays(bool state, uint8_t triggerSource) {
    RelayCommand cmd;
    cmd.relayId = 0xFF;
    cmd.state = state;
    cmd.triggerSource = triggerSource;
    if (relayQueue) {
        if (xQueueSend(relayQueue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE) {
            diagnosticsLog("relay/request", "FAIL all-relay queue full");
        } else {
            s_relayCommandsQueued.fetch_add(1, std::memory_order_release);
            diagnosticsLog("relay/request", "queued all state=%u source=%u",
                           state, triggerSource);
        }
    }
}

void relayCommandApplied(void) {
    s_relayCommandsApplied.fetch_add(1, std::memory_order_release);
}

bool relayWaitForIdle(uint32_t timeoutMs) {
    const uint32_t target =
        s_relayCommandsQueued.load(std::memory_order_acquire);
    const uint32_t started = millis();
    while (s_relayCommandsApplied.load(std::memory_order_acquire) < target) {
        if (millis() - started >= timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

void applyRelayLocked(uint8_t relayId, bool state, uint8_t triggerSource) {
    if (relayId >= RELAY_COUNT) {
        return;
    }
    relayStates[relayId] = state;
    digitalWrite(relayPins[relayId], state ? HIGH : LOW);
    if (relayId < 4) control_states[relayId] = state ? 1 : 0;
    Serial.printf("Relay %d -> %s\n", relayId + 1, state ? "ON" : "OFF");
    diagnosticsLog("relay/apply", "relay=%u state=%s source=%u",
                   relayId + 1, state ? "ON" : "OFF", triggerSource);
    logRelayEvent(relayId, state, triggerSource);
}
