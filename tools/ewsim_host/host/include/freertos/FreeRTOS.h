// ewsim_host: FreeRTOS API on a deterministic cooperative scheduler
// (host/src/sched.cpp). One simulated task runs at a time; tasks switch only
// at blocking calls, so portENTER_CRITICAL is a no-op.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_attr.h"
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define pdFAIL 0
#define errQUEUE_EMPTY 0
#define errQUEUE_FULL 0
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define portTICK_RATE_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t) ((uint32_t)(t))
#define configMAX_PRIORITIES 25
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0x7FFFFFFF
#define configMINIMAL_STACK_SIZE 768
#define portNUM_PROCESSORS 2
typedef struct { int dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL(m) do { (void)(m); } while (0)
#define portENTER_CRITICAL_ISR(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL_ISR(m) do { (void)(m); } while (0)
#define portENTER_CRITICAL_SAFE(m) do { (void)(m); } while (0)
#define portEXIT_CRITICAL_SAFE(m) do { (void)(m); } while (0)
#define taskENTER_CRITICAL(m) do { (void)(m); } while (0)
#define taskEXIT_CRITICAL(m) do { (void)(m); } while (0)
#define portDISABLE_INTERRUPTS()
#define portENABLE_INTERRUPTS()
#define portYIELD_FROM_ISR(...) do {} while (0)
#define portEND_SWITCHING_ISR(x) do { (void)(x); } while (0)
#define configASSERT(x) do { if (!(x)) ewsimHostPanic("configASSERT failed: " #x, __FILE__, __LINE__); } while (0)
#ifdef __cplusplus
extern "C" {
#endif
void ewsimHostPanic(const char *what, const char *file, int line) __attribute__((noreturn));
BaseType_t xPortGetCoreID(void);
#ifdef __cplusplus
}
#endif
