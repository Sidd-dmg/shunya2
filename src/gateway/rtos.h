#ifndef GATEWAY_RTOS_H
#define GATEWAY_RTOS_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <stdbool.h>

extern SemaphoreHandle_t nodesMutex;
extern SemaphoreHandle_t relayMutex;
extern SemaphoreHandle_t mqttMutex;
extern SemaphoreHandle_t spiffsMutex;
extern SemaphoreHandle_t eepromMutex;
extern SemaphoreHandle_t displayUartMutex;
extern SemaphoreHandle_t hubUartMutex;
extern SemaphoreHandle_t stateMutex;
extern SemaphoreHandle_t otaTriggerSemaphore;
extern SemaphoreHandle_t otaCheckSemaphore;

extern QueueHandle_t sensorQueue;
extern QueueHandle_t controlQueue;
extern QueueHandle_t mqttPublishQueue;
extern QueueHandle_t relayQueue;
extern QueueHandle_t cmdQueue;
extern QueueHandle_t hubTxQueue;
extern QueueHandle_t displayCmdQueue;

bool rtosCreatePrimitives(void);

#endif
