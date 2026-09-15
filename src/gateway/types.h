#ifndef GATEWAY_TYPES_H
#define GATEWAY_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

typedef struct {
    uint8_t nodeId;
    uint8_t ieee[8];
    uint16_t shortAddr;
    uint8_t lqi;
    uint8_t batteryPercent;
    float lastTemperature;
    float lastHumidity;
    unsigned long lastSeen;
    bool active;
} NodeInfo;

typedef struct {
    uint8_t nodeId;
    uint8_t relayId;
    float tempMin;
    float tempMax;
    float humMin;
    float humMax;
    bool enabled;
    bool lastState;
    bool useTempControl;
    bool useHumControl;
    bool logicOR;
    unsigned long lastStateChange;
} AutomationRule;

typedef struct {
    uint8_t relayMask;
    uint32_t duration;
} QueueStep;

typedef struct {
    QueueStep steps[MAX_QUEUE_STEPS];
    uint8_t stepCount;
    bool loop;
    bool autoStart;
} QueueConfig;

typedef struct {
    uint8_t buttonId;
    uint8_t requestedState;
} ControlRequest;

typedef struct {
    char topic[128];
    char payload[MQTT_PUB_PAYLOAD_MAX];
    bool retain;
} MqttPublishItem;

typedef struct {
    uint8_t relayId;
    bool state;
    uint8_t triggerSource;
} RelayCommand;

typedef struct {
    char payload[MQTT_CMD_PAYLOAD_MAX];
    unsigned int length;
} MqttCmdItem;

typedef enum {
    SENSOR_EVT_DATA = 0,
    SENSOR_EVT_JOINED,
    SENSOR_EVT_LEFT,
    SENSOR_EVT_LIST
} SensorEvtType;

typedef struct {
    SensorEvtType type;
    uint8_t nodeId;
    uint8_t ieee[8];
    uint16_t shortAddr;
    float temperature;
    float humidity;
    uint8_t lqi;
    uint8_t batteryPercent;
} SensorQueueItem;

typedef struct {
    uint8_t cmd;
    uint8_t payload[48];
    uint8_t length;
} HubTxItem;

#endif
