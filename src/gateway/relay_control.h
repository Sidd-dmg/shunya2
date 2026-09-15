#ifndef GATEWAY_RELAY_CONTROL_H
#define GATEWAY_RELAY_CONTROL_H

#include <stdint.h>
#include <stdbool.h>

void relayInitPins(void);
void requestRelay(uint8_t relayId, bool state, uint8_t triggerSource);
void requestAllRelays(bool state, uint8_t triggerSource);
void applyRelayLocked(uint8_t relayId, bool state, uint8_t triggerSource);

#endif
