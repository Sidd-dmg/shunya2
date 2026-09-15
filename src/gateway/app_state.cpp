#include "app_state.h"
#include <cstdio>
#include <cstring>

NodeInfo nodes[MAX_NODES];
uint8_t nodeCount = 0;
bool pairingMode = false;
unsigned long pairingModeStartTime = 0;
unsigned long buttonPressStartTime = 0;
bool buttonPressed = false;

bool relayStates[4] = {false, false, false, false};
uint8_t relayPins[4] = {RELAY1_PIN, RELAY2_PIN, RELAY3_PIN, RELAY4_PIN};
uint8_t operationMode = MODE_MANUAL;
AutomationRule autoRules[MAX_AUTO_RULES];
QueueConfig queueConfig;
uint8_t currentQueueStep = 0;
unsigned long queueStepStartTime = 0;
bool queueRunning = false;
bool queuePaused = false;

uint8_t wifi_flag = 0;
uint8_t mqtt_flag = 0;
uint8_t pair_mode = 0;
uint8_t ota_available = 0;
uint8_t control_states[7] = {0, 0, 0, 0, 0, 0, 0};
float temps[MAX_NODES] = {0};
float hums[MAX_NODES] = {0};
uint16_t sensorSleepSec = SENSOR_SLEEP_DEFAULT_S;

uint8_t hybridLogic = 0;
bool automationActive = false;
bool queueActive = false;
bool hybridRelayStates[4] = {false, false, false, false};
bool automationOwns[4] = {false, false, false, false};

bool timeSync = false;
unsigned long lastNTPSync = 0;
bool otaInProgress = false;

String deviceMAC = "";
String mqtt_client_id = "";
String mqtt_command_topic = "";
String mqtt_status_topic = "";
String mqtt_event_log_topic = "";
String provServiceName = "";

int8_t findNodeById(uint8_t nodeId) {
    for (int i = 0; i < nodeCount; i++) {
        if (nodes[i].nodeId == nodeId) {
            return (int8_t)i;
        }
    }
    return -1;
}

int8_t findNodeByIeee(const uint8_t *ieee) {
    for (int i = 0; i < nodeCount; i++) {
        if (memcmp(nodes[i].ieee, ieee, 8) == 0) {
            return (int8_t)i;
        }
    }
    return -1;
}

int8_t getNextAvailableNodeId(void) {
    bool usedIds[MAX_NODES + 1] = {false};
    for (int i = 0; i < nodeCount; i++) {
        if (nodes[i].nodeId >= 1 && nodes[i].nodeId <= MAX_NODES) {
            usedIds[nodes[i].nodeId] = true;
        }
    }
    for (int id = 1; id <= MAX_NODES; id++) {
        if (!usedIds[id]) {
            return (int8_t)id;
        }
    }
    return -1;
}

void formatIeee(const uint8_t *ieee, char *out, size_t outLen) {
    snprintf(out, outLen, "%02X%02X%02X%02X%02X%02X%02X%02X",
             ieee[0], ieee[1], ieee[2], ieee[3],
             ieee[4], ieee[5], ieee[6], ieee[7]);
}
