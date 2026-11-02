#ifndef GATEWAY_APP_STATE_H
#define GATEWAY_APP_STATE_H

#include <Arduino.h>
#include "types.h"

extern NodeInfo nodes[MAX_NODES];
extern uint8_t nodeCount;
extern bool pairingMode;
extern unsigned long pairingModeStartTime;
extern unsigned long buttonPressStartTime;
extern bool buttonPressed;

extern bool relayStates[RELAY_COUNT];
extern uint8_t relayPins[RELAY_COUNT];
extern uint8_t operationMode;
extern AutomationRule autoRules[MAX_AUTO_RULES];
extern QueueConfig queueConfig;
extern uint8_t currentQueueStep;
extern unsigned long queueStepStartTime;
extern bool queueRunning;
extern bool queuePaused;

extern uint8_t wifi_flag;
extern uint8_t mqtt_flag;
extern uint8_t pair_mode;
extern uint8_t ota_available;
/* [0..3] mirror relays 1-4. [4] is the mode, [5] pairing, [6] provisioning.
 * Relays 5-7 live only in relayStates because those slots are already used. */
extern uint8_t control_states[7];
extern float temps[MAX_NODES];
extern float hums[MAX_NODES];
extern uint16_t sensorSleepSec;

extern uint8_t hybridLogic;
extern bool automationActive;
extern bool queueActive;
extern bool hybridRelayStates[RELAY_COUNT];
extern bool automationOwns[RELAY_COUNT];

extern bool timeSync;
extern unsigned long lastNTPSync;
extern bool otaInProgress;

extern String deviceMAC;
extern String mqtt_client_id;
extern String mqtt_command_topic;
extern String mqtt_status_topic;
extern String mqtt_event_log_topic;
extern String provServiceName;

int8_t findNodeById(uint8_t nodeId);
int8_t findNodeByIeee(const uint8_t *ieee);
int8_t getNextAvailableNodeId(void);
void formatIeee(const uint8_t *ieee, char *out, size_t outLen);

#endif
