#ifndef GATEWAY_PERSISTENCE_H
#define GATEWAY_PERSISTENCE_H

#include <stdint.h>

void persistenceBegin(void);
void loadNodesFromEEPROM(void);
void saveNodesToEEPROM(void);
void loadConfigFromEEPROM(void);
void saveModeConfig(void);
void saveManualModeConfig(void);
void loadManualModeConfig(void);
void saveAutomationConfig(void);
void loadAutomationConfig(void);
void saveQueueConfig(void);
void loadQueueConfig(void);
void saveHybridConfig(void);
void loadHybridConfig(void);
void saveSensorSleep(void);
void loadSensorSleep(void);
void applySensorSleep(uint16_t seconds);
void initializeAutomationRules(void);
void initializeQueueConfig(void);
void eepromFlushAll(void);

#endif
