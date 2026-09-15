#include "rtos.h"
#include "types.h"
#include "display_link.h"

SemaphoreHandle_t nodesMutex = NULL;
SemaphoreHandle_t relayMutex = NULL;
SemaphoreHandle_t mqttMutex = NULL;
SemaphoreHandle_t spiffsMutex = NULL;
SemaphoreHandle_t eepromMutex = NULL;
SemaphoreHandle_t displayUartMutex = NULL;
SemaphoreHandle_t hubUartMutex = NULL;
SemaphoreHandle_t stateMutex = NULL;
SemaphoreHandle_t otaTriggerSemaphore = NULL;
SemaphoreHandle_t otaCheckSemaphore = NULL;

QueueHandle_t sensorQueue = NULL;
QueueHandle_t controlQueue = NULL;
QueueHandle_t mqttPublishQueue = NULL;
QueueHandle_t relayQueue = NULL;
QueueHandle_t cmdQueue = NULL;
QueueHandle_t hubTxQueue = NULL;
QueueHandle_t displayCmdQueue = NULL;

bool rtosCreatePrimitives(void) {
    nodesMutex = xSemaphoreCreateMutex();
    relayMutex = xSemaphoreCreateMutex();
    mqttMutex = xSemaphoreCreateMutex();
    spiffsMutex = xSemaphoreCreateMutex();
    eepromMutex = xSemaphoreCreateMutex();
    displayUartMutex = xSemaphoreCreateMutex();
    hubUartMutex = xSemaphoreCreateMutex();
    stateMutex = xSemaphoreCreateMutex();
    otaTriggerSemaphore = xSemaphoreCreateBinary();
    otaCheckSemaphore = xSemaphoreCreateBinary();

    sensorQueue = xQueueCreate(20, sizeof(SensorQueueItem));
    controlQueue = xQueueCreate(10, sizeof(ControlRequest));
    mqttPublishQueue = xQueueCreate(30, sizeof(MqttPublishItem));
    relayQueue = xQueueCreate(16, sizeof(RelayCommand));
    cmdQueue = xQueueCreate(8, sizeof(MqttCmdItem));
    hubTxQueue = xQueueCreate(16, sizeof(HubTxItem));
    /* HELLO/snapshot traffic and touch commands share this queue. Eight slots
     * absorb reconnect bursts without delaying or rejecting user actions. */
    displayCmdQueue = xQueueCreate(8, sizeof(DisplayCommandItem));

    return nodesMutex && relayMutex && mqttMutex && spiffsMutex && eepromMutex &&
           displayUartMutex && hubUartMutex && stateMutex && otaTriggerSemaphore &&
           otaCheckSemaphore && sensorQueue && controlQueue && mqttPublishQueue &&
           relayQueue && cmdQueue && hubTxQueue && displayCmdQueue;
}
