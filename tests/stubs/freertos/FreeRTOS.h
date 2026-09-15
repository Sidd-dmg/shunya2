#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#endif
