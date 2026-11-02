#ifndef SHUNYA_DISPLAY_STATE_H
#define SHUNYA_DISPLAY_STATE_H

#include "display_protocol.h"
#include <Arduino.h>

void displayStateBegin();
void displayStateSetSnapshot(const display_snapshot_t &snapshot);
bool displayStateGetSnapshot(display_snapshot_t &snapshot);
void displayStateSetLink(bool connected);
bool displayStateLinkConnected();
uint32_t displayStateLastRxMs();
void displayStateSetAck(const display_ack_t &ack);
bool displayStateTakeAck(display_ack_t &ack);
void displayStateSetOtaStatus(const display_hmi_ota_status_t &status);
bool displayStateTakeOtaStatus(display_hmi_ota_status_t &status);
void displayStateSetWifiProv(const display_wifi_prov_t &info);
bool displayStateTakeWifiProv(display_wifi_prov_t &info);

#endif
