#pragma once
#include "FreeRTOS.h"
typedef struct HTask *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
typedef enum { eNoAction = 0, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite } eNotifyAction;
#ifdef __cplusplus
extern "C" {
#endif
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t t);
void vTaskDelay(TickType_t ticks);
void vTaskDelayUntil(TickType_t *prev, TickType_t inc);
BaseType_t xTaskDelayUntil(TickType_t *prev, TickType_t inc);
void vTaskSuspend(TaskHandle_t t);
void vTaskResume(TaskHandle_t t);
eTaskState eTaskGetState(TaskHandle_t t);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
UBaseType_t uxTaskPriorityGet(TaskHandle_t t);
void vTaskPrioritySet(TaskHandle_t t, UBaseType_t prio);
char *pcTaskGetName(TaskHandle_t t);
void taskYIELD(void);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyFromISR(TaskHandle_t t, uint32_t value, eNotifyAction action, BaseType_t *woken);
BaseType_t xTaskNotifyWait(uint32_t clearEntry, uint32_t clearExit, uint32_t *value, TickType_t ticks);
void vTaskSuspendAll(void);
BaseType_t xTaskResumeAll(void);
#ifdef __cplusplus
}
#endif
#define pcTaskGetTaskName pcTaskGetName
