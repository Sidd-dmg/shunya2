#include "relay_control.h"
#include "app_state.h"
#include "rtos.h"
#include "event_log.h"
#include "diagnostics.h"
#include <Arduino.h>

void relayInitPins(void) {
    for (int i = 0; i < 4; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], LOW);
        relayStates[i] = false;
    }
}

void requestRelay(uint8_t relayId, bool state, uint8_t triggerSource) {
    if (relayId >= 4 || relayQueue == NULL) {
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
            diagnosticsLog("relay/request", "queued all state=%u source=%u",
                           state, triggerSource);
        }
    }
}

void applyRelayLocked(uint8_t relayId, bool state, uint8_t triggerSource) {
    if (relayId >= 4) {
        return;
    }
    relayStates[relayId] = state;
    digitalWrite(relayPins[relayId], state ? HIGH : LOW);
    control_states[relayId] = state ? 1 : 0;
    Serial.printf("Relay %d -> %s\n", relayId + 1, state ? "ON" : "OFF");
    diagnosticsLog("relay/apply", "relay=%u state=%s source=%u",
                   relayId + 1, state ? "ON" : "OFF", triggerSource);
    logRelayEvent(relayId, state, triggerSource);
}
