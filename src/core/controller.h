// Controller: bridges hardware to model + event queue.
//   - sensor reads (IMU)                 -> mutate model
//   - input polling (button) + touch ISR -> post events
//   - owns the I/O + render FreeRTOS tasks
#pragma once
#include <Arduino.h>
#include "model.h"

void controllerInit();           // pinModes + ADC config
void controllerStartTasks();     // launches the FreeRTOS tasks

// Accelerometer read. Only taskIO may call this — it touches the shared I2C
// bus and has no internal locking. Raw 14-bit signed counts (±2g).
bool readAccel(int16_t &x, int16_t &y, int16_t &z);

// Enter deep sleep with wake sources configured per the model's wakeOn*
// flags. If wakeOnImu is set this also configures the MMA8451 motion
// interrupt before sleeping. Holds GPIO17 high during sleep so the LDO
// stays latched. Does not return.
void enterDeepSleep();
