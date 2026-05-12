#include "model.h"

Model            model;
SemaphoreHandle_t modelMutex = nullptr;

void modelInit() {
  modelMutex = xSemaphoreCreateMutex();
}
