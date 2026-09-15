#ifndef GATEWAY_EVENT_LOG_H
#define GATEWAY_EVENT_LOG_H

#include <stdint.h>
#include <stdbool.h>

void initSPIFFS(void);
void logRelayEvent(uint8_t relayId, bool state, uint8_t triggerSource);
void sendEventLogs(void);
void clearEventLogs(void);
uint32_t getEventLogCount(void);

#endif
