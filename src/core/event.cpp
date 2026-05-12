#include "event.h"

QueueHandle_t eventQueue = nullptr;

void eventQueueInit() {
  eventQueue = xQueueCreate(16, sizeof(Event));
}

void postEvent(const Event &e) {
  if (!eventQueue) return;
  xQueueSend(eventQueue, &e, 0);   // drop if full — UI events are time-sensitive
}
