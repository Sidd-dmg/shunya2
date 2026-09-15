#ifndef TEST_FREERTOS_SEMPHR_H
#define TEST_FREERTOS_SEMPHR_H
#include "FreeRTOS.h"
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
#endif
