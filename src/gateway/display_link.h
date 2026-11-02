#ifndef GATEWAY_DISPLAY_LINK_H
#define GATEWAY_DISPLAY_LINK_H

#include "display_protocol.h"
#include <Arduino.h>

typedef struct {
    uint8_t type;
    uint16_t sequence;
    uint16_t length;
    uint32_t received_ms;
    uint8_t payload[DISPLAY_PROTO_MAX_PAYLOAD];
} DisplayCommandItem;

typedef struct {
    uint32_t txFrames;
    uint32_t rxFrames;
    uint32_t crcErrors;
    uint32_t framingErrors;
    uint32_t lastRxMs;
    uint32_t lastTxMs;
    bool online;
} DisplayLinkStats;

void displayBegin();
void displayRxTask(void *arg);
void displayProcessPendingCommands();
bool displayHasPendingCommands();
void displaySendSnapshot();
void displaySendHeartbeat();
void displayGetStats(DisplayLinkStats *stats);
bool displayEchoTest(uint32_t timeoutMs, uint32_t *rttMs);

#endif
