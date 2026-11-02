#ifndef GATEWAY_MODE_ENGINE_H
#define GATEWAY_MODE_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

void runManualMode(void);
void runAutomationMode(void);
void runQueueMode(void);
void runHybridMode(void);
void transitionToMode(uint8_t newMode, bool graceful);
uint8_t automationReleaseMask(void);
void applyAutomationRelease(uint8_t mask);
void pauseQueue(void);
void resumeQueue(void);
void processControlRequest(uint8_t buttonId, uint8_t requestedState);
void modeTask(void *pvParameters);
void sensorTask(void *pvParameters);
void relayTask(void *pvParameters);

#endif
