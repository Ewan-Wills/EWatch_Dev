#pragma once
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
typedef QueueHandle_t SemaphoreHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *woken);
BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t s, BaseType_t *woken);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s);
void vSemaphoreDelete(SemaphoreHandle_t s);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t s);
#ifdef __cplusplus
}
#endif
#define xQueueSemaphoreTake xSemaphoreTake
