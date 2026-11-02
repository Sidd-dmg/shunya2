#include "ui_backend.h"
#include "display_link.h"
#include "display_state.h"
#include <Arduino.h>

extern "C" bool uiBackendGetSnapshot(display_snapshot_t *snapshot) {
    return snapshot && displayStateGetSnapshot(*snapshot);
}

extern "C" bool uiBackendLinkConnected(void) {
    return displayLinkConnected();
}

extern "C" bool uiBackendTakeAck(display_ack_t *ack) {
    return ack && displayStateTakeAck(*ack);
}
extern "C" bool uiBackendTakeOtaStatus(display_hmi_ota_status_t *status) {
    return status && displayStateTakeOtaStatus(*status);
}

extern "C" bool uiBackendTakeWifiProv(display_wifi_prov_t *info) {
    return info && displayStateTakeWifiProv(*info);
}

extern "C" uint16_t uiBackendSendCommand(uint8_t type, const void *payload, uint16_t length) {
    return displaySendCommand(type, payload, length);
}

extern "C" uint32_t uiBackendMillis(void) {
    return millis();
}
