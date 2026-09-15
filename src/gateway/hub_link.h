#ifndef GATEWAY_HUB_LINK_H
#define GATEWAY_HUB_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t txFrames;
    uint32_t rxFrames;
    uint32_t checksumErrors;
    uint32_t lastAckMs;
    uint32_t lastStatusMs;
    uint32_t lastEchoMs;
    uint8_t lastAckCmd;
    uint8_t lastNackCmd;
    uint16_t lastPanId;
    uint8_t lastChannel;
    uint8_t lastNodeCount;
    uint32_t lastUptimeMs;
} HubLinkStats;

void hubBegin(void);
void hubRequestPairOpen(uint16_t durationSec);
void hubRequestPairClose(void);
void hubRequestForgetNode(uint8_t nodeId, const uint8_t *ieee);
void hubRequestForgetAll(void);
void hubRequestListNodes(void);
void hubRequestStatus(void);
void hubRequestPing(void);
void hubRequestEcho(const uint8_t *payload, uint8_t length);
void hubRequestSetSleep(uint16_t seconds);
void hubGetStats(HubLinkStats *out);
bool hubPingTest(uint32_t timeoutMs, uint32_t *rttMs);
bool hubStatusTest(uint32_t timeoutMs, HubLinkStats *out);
bool hubEchoTest(const uint8_t *payload, uint8_t length, uint32_t timeoutMs, uint32_t *rttMs);
void hubRxTask(void *pvParameters);
void hubTxTask(void *pvParameters);

#endif
