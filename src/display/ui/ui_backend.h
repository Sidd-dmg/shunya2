#ifndef SHUNYA_DISPLAY_UI_BACKEND_H
#define SHUNYA_DISPLAY_UI_BACKEND_H

#include "display_protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool uiBackendGetSnapshot(display_snapshot_t *snapshot);
bool uiBackendLinkConnected(void);
bool uiBackendTakeAck(display_ack_t *ack);
bool uiBackendTakeOtaStatus(display_hmi_ota_status_t *status);
bool uiBackendTakeWifiProv(display_wifi_prov_t *info);
uint16_t uiBackendSendCommand(uint8_t type, const void *payload, uint16_t length);
uint32_t uiBackendMillis(void);

#ifdef __cplusplus
}
#endif

#endif
