// Controller: bridges hardware to model + event queue.
//   - sensor reads (RTC, IMU, battery)  -> mutate model
//   - input polling (button, IMU gestures) + touch ISR drain -> post events
//   - actions exposed for views to commit changes (e.g. write RTC)
#pragma once
#include <Arduino.h>
#include "model.h"

void controllerInit();           // pinModes + ADC config
void controllerStartTasks();     // launches the FreeRTOS tasks

// Sensor sampling functions. Only taskIO may call these — they touch the
// shared I2C bus or the battery ADC sequence and have no internal locking.
bool readRTC(uint8_t &h, uint8_t &m, uint8_t &s,
             uint8_t &weekday, uint8_t &day, uint8_t &month, uint16_t &year);
bool writeRTC(uint8_t h, uint8_t m, uint8_t s,
              uint8_t weekday, uint8_t day, uint8_t month, uint16_t year);
bool readAccel(int16_t &x, int16_t &y, int16_t &z);
bool readBattery(float &volts, uint8_t &pct);

// Thread-safe entry from any task: queue an RTC write to be performed by
// taskIO at the start of its next cycle. Caller supplies HMS and date (year
// as full 4-digit value, weekday 0..6 per RV-3028 convention).
void requestSetRTC(uint8_t h, uint8_t m, uint8_t s,
                   uint8_t weekday, uint8_t day, uint8_t month, uint16_t year);

// Enter deep sleep with wake sources configured per the model's wakeOn*
// flags. If wakeOnImu is set this also configures the MMA8451 motion
// interrupt before sleeping. Holds GPIO17 high during sleep so the LDO
// stays latched. Does not return.
void enterDeepSleep();
