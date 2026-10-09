#pragma once
#include "FreeRTOS.h"
typedef struct HQueue *QueueHandle_t;
#ifdef __cplusplus
extern "C" {
#endif
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t itemSize);
void vQueueDelete(QueueHandle_t q);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t q);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *woken);
BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *item, BaseType_t *woken);
#ifdef __cplusplus
}
#endif
#define xQueueSendToBack xQueueSend
#define xQueueSendToBackFromISR xQueueSendFromISR
