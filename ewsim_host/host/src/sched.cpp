// ewsim_host: deterministic cooperative FreeRTOS on host threads.
//
// Every simulated task is a pthread, but exactly one runs at a time (a baton
// passed between the task threads and the scheduler loop in the main thread).
// A task gives the baton back only when it blocks: vTaskDelay/delay, a queue
// or semaphore wait, vTaskSuspend, or the busy-wait guard in the clock
// (sim_clock_tick). The scheduler then runs the highest-priority ready task
// (round robin within a priority); when nothing is ready it jumps the
// simulated clock to the next deadline. Nothing depends on wall time, so a
// run is reproducible bit for bit and can go faster or slower than real time.
#include "host_sim.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <pthread.h>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>

enum class TS { Ready, Blocked, Suspended, Done };

struct HTask {
  int id;
  std::string name;
  int prio;
  int core;
  TaskFunction_t fn;
  void *arg;
  pthread_t th;
  std::condition_variable cv;
  TS state = TS::Ready;
  uint64_t wakeAt = UINT64_MAX;   // deadline while Blocked
  void *waitObj = nullptr;        // object waited on (queue, notify), or null for a plain delay
  bool timedOut = false;
  uint64_t seq = 0;               // round-robin order
  uint32_t timeCalls = 0;         // clock reads since the last yield (busy-wait guard)
  uint32_t notifyValue = 0;
  bool notifyPending = false;
  bool suspendAfterBlock = false;
};

static std::mutex G;
static std::condition_variable schedCv;
static std::vector<HTask *> tasks;
static HTask *running = nullptr;          // task holding the baton (null = scheduler)
static thread_local HTask *self = nullptr;
static uint64_t nowUs = 0;
static uint64_t seqCounter = 0;
static int nextId = 1;
static bool stopping = false;

uint64_t sim_now_us() { return nowUs; }
const char *sim_current_task_name() { return self ? self->name.c_str() : "host"; }

void sim_advance_us(uint64_t us) {      // time spent by the running task (bus transfers, busy waits)
  nowUs += us;
}

// ---------------------------------------------------------------- baton
static void giveBatonAndWait(std::unique_lock<std::mutex> &lk) {
  HTask *me = self;
  me->timeCalls = 0;
  running = nullptr;
  schedCv.notify_one();
  me->cv.wait(lk, [&] { return running == me; });
}

// Block the current task until woken (by an object signal) or the deadline.
// Returns false on timeout.
static bool blockOn(void *obj, uint64_t deadline) {
  std::unique_lock<std::mutex> lk(G);
  HTask *me = self;
  if (!me) ewsimHostPanic("blocking call outside a simulated task", __FILE__, __LINE__);
  me->state = TS::Blocked;
  me->waitObj = obj;
  me->wakeAt = deadline;
  me->timedOut = false;
  giveBatonAndWait(lk);
  return !me->timedOut;
}

static void wakeWaiters(void *obj) {     // caller holds no lock; state is only touched under G
  std::lock_guard<std::mutex> lk(G);
  for (HTask *t : tasks)
    if (t->state == TS::Blocked && t->waitObj == obj && obj) {
      t->state = TS::Ready;
      t->waitObj = nullptr;
      t->wakeAt = UINT64_MAX;
    }
}

static uint64_t deadlineFor(TickType_t ticks) {
  if (ticks == portMAX_DELAY) return UINT64_MAX;
  return nowUs + (uint64_t)ticks * 1000ULL;
}

static void *taskEntry(void *p) {
  HTask *t = (HTask *)p;
  self = t;
  {
    std::unique_lock<std::mutex> lk(G);
    t->cv.wait(lk, [&] { return running == t; });
  }
  t->fn(t->arg);
  // A FreeRTOS task must not return; treat it like vTaskDelete(NULL).
  vTaskDelete(nullptr);
  return nullptr;
}

static HTask *createTask(TaskFunction_t fn, const char *name, void *arg, int prio, int core) {
  HTask *t = new HTask();
  t->id = nextId++;
  t->name = name ? name : "?";
  t->prio = prio;
  t->core = core < 0 || core > 1 ? 0 : core;
  t->fn = fn;
  t->arg = arg;
  {
    std::lock_guard<std::mutex> lk(G);
    t->seq = ++seqCounter;
    tasks.push_back(t);
  }
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 8u << 20);   // generous: firmware stacks are sized for the ESP32
  if (pthread_create(&t->th, &a, taskEntry, t) != 0) ewsimHostPanic("pthread_create failed", __FILE__, __LINE__);
  pthread_attr_destroy(&a);
  return t;
}

void sim_create_task(void (*fn)(void *), const char *name, void *arg, int prio, int core) {
  createTask(fn, name, arg, prio, core);
}

// ---------------------------------------------------------------- scheduler loop
// Runs in the main thread until sim_stop() (called by the script runner).
// Returns 0, or 2 on deadlock (no task can ever run again).
int sim_run() {
  std::unique_lock<std::mutex> lk(G);
  for (;;) {
    if (stopping) return 0;
    // promote expired waits
    for (HTask *t : tasks)
      if (t->state == TS::Blocked && t->wakeAt <= nowUs) {
        t->state = TS::Ready;
        t->timedOut = t->waitObj != nullptr;
        t->waitObj = nullptr;
        t->wakeAt = UINT64_MAX;
      }
    HTask *pick = nullptr;
    for (HTask *t : tasks)
      if (t->state == TS::Ready && (!pick || t->prio > pick->prio || (t->prio == pick->prio && t->seq < pick->seq)))
        pick = t;
    if (!pick) {
      uint64_t next = UINT64_MAX;
      for (HTask *t : tasks)
        if (t->state == TS::Blocked && t->wakeAt < next) next = t->wakeAt;
      if (next == UINT64_MAX) {
        fprintf(stderr, "ewsim: DEADLOCK at t=%.3fs: every task is blocked forever\n", nowUs / 1e6);
        for (HTask *t : tasks)
          fprintf(stderr, "  task %-10s prio %2d state %d\n", t->name.c_str(), t->prio, (int)t->state);
        return 2;
      }
      if (next > nowUs) nowUs = next;
      continue;
    }
    pick->seq = ++seqCounter;
    running = pick;
    pick->cv.notify_one();
    schedCv.wait(lk, [&] { return running == nullptr; });
  }
}

void sim_stop() {               // called from a task (the script runner)
  std::unique_lock<std::mutex> lk(G);
  stopping = true;
  // keep the baton parked: the scheduler loop returns and main() exits
  running = nullptr;
  schedCv.notify_one();
  self->cv.wait(lk, [] { return false; });
}

// Busy-wait guard: code like `while (millis() - t0 < 300) {}` would spin
// forever on a frozen clock. Every clock read advances time by 1 us, and a
// task that reads the clock 2000 times without blocking is made to yield for
// 1 ms (as a preemptive RTOS tick would let others run).
void sim_clock_tick() {
  HTask *me = self;
  if (!me) return;
  nowUs += 1;
  if (++me->timeCalls >= 2000) vTaskDelay(1);
}

// ---------------------------------------------------------------- task API
extern "C" {

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core) {
  HTask *t = createTask(fn, name, arg, (int)prio, (int)core);
  if (out) *out = t;
  return pdPASS;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out) {
  return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0);
}

void vTaskDelete(TaskHandle_t t) {
  HTask *tt = t ? t : self;
  std::unique_lock<std::mutex> lk(G);
  tt->state = TS::Done;
  if (tt == self) {
    running = nullptr;
    schedCv.notify_one();
    lk.unlock();
    pthread_exit(nullptr);
  }
}

void vTaskDelay(TickType_t ticks) {
  if (!self) ewsimHostPanic("vTaskDelay outside a simulated task", __FILE__, __LINE__);
  blockOn(nullptr, nowUs + (uint64_t)ticks * 1000ULL);
}

void vTaskDelayUntil(TickType_t *prev, TickType_t inc) {
  uint64_t target = (uint64_t)(*prev + inc) * 1000ULL;
  *prev += inc;
  blockOn(nullptr, target > nowUs ? target : nowUs);
}
BaseType_t xTaskDelayUntil(TickType_t *prev, TickType_t inc) { vTaskDelayUntil(prev, inc); return pdTRUE; }

void taskYIELD(void) { vTaskDelay(0); }

void vTaskSuspend(TaskHandle_t t) {
  HTask *tt = t ? t : self;
  std::unique_lock<std::mutex> lk(G);
  if (tt == self) {
    tt->state = TS::Suspended;
    giveBatonAndWait(lk);
  } else if (tt->state != TS::Done) {
    tt->state = TS::Suspended;
    tt->waitObj = nullptr;
    tt->wakeAt = UINT64_MAX;
  }
}

void vTaskResume(TaskHandle_t t) {
  std::lock_guard<std::mutex> lk(G);
  if (t && t->state == TS::Suspended) t->state = TS::Ready;
}

eTaskState eTaskGetState(TaskHandle_t t) {
  std::lock_guard<std::mutex> lk(G);
  if (!t) return eInvalid;
  if (t == self) return eRunning;
  switch (t->state) {
    case TS::Ready: return eReady;
    case TS::Blocked: return eBlocked;
    case TS::Suspended: return eSuspended;
    default: return eDeleted;
  }
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return self; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(nowUs / 1000ULL); }
TickType_t xTaskGetTickCountFromISR(void) { return xTaskGetTickCount(); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 2048; }
UBaseType_t uxTaskPriorityGet(TaskHandle_t t) { return (t ? t : self)->prio; }
void vTaskPrioritySet(TaskHandle_t t, UBaseType_t p) { (t ? t : self)->prio = (int)p; }
char *pcTaskGetName(TaskHandle_t t) { return (char *)(t ? t : self)->name.c_str(); }
BaseType_t xPortGetCoreID(void) { return self ? self->core : 0; }
void vTaskSuspendAll(void) {}
BaseType_t xTaskResumeAll(void) { return pdFALSE; }

static char kNotify;   // waitObj tag for task notifications (per task: &task->notifyValue)

BaseType_t xTaskNotify(TaskHandle_t t, uint32_t value, eNotifyAction action) {
  {
    std::lock_guard<std::mutex> lk(G);
    switch (action) {
      case eSetBits: t->notifyValue |= value; break;
      case eIncrement: t->notifyValue++; break;
      case eSetValueWithOverwrite:
      case eSetValueWithoutOverwrite: t->notifyValue = value; break;
      default: break;
    }
    t->notifyPending = true;
  }
  wakeWaiters(&t->notifyValue);
  (void)kNotify;
  return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) { return xTaskNotify(t, 0, eIncrement); }
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *w) { if (w) *w = 0; xTaskNotifyGive(t); }
BaseType_t xTaskNotifyFromISR(TaskHandle_t t, uint32_t v, eNotifyAction a, BaseType_t *w) { if (w) *w = 0; return xTaskNotify(t, v, a); }

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
  uint64_t dl = deadlineFor(ticks);
  for (;;) {
    if (self->notifyValue) {
      uint32_t v = self->notifyValue;
      self->notifyValue = clear ? 0 : v - 1;
      return v;
    }
    if (ticks == 0 || !blockOn(&self->notifyValue, dl)) return 0;
  }
}

BaseType_t xTaskNotifyWait(uint32_t clearEntry, uint32_t clearExit, uint32_t *value, TickType_t ticks) {
  uint64_t dl = deadlineFor(ticks);
  if (!self->notifyPending) self->notifyValue &= ~clearEntry;
  for (;;) {
    if (self->notifyPending) {
      self->notifyPending = false;
      if (value) *value = self->notifyValue;
      self->notifyValue &= ~clearExit;
      return pdTRUE;
    }
    if (ticks == 0 || !blockOn(&self->notifyValue, dl)) return pdFALSE;
  }
}

}  // extern "C"

// ---------------------------------------------------------------- queues + semaphores
enum class QK { Queue, Binary, Counting, Mutex, Recursive };
struct HQueue {
  QK kind;
  UBaseType_t len, itemSize;
  std::deque<std::vector<uint8_t>> items;
  UBaseType_t count = 0, max = 1;     // semaphores
  HTask *holder = nullptr;            // mutexes
  int depth = 0;
};

extern "C" {

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t itemSize) {
  HQueue *q = new HQueue();
  q->kind = QK::Queue; q->len = len; q->itemSize = itemSize;
  return q;
}
void vQueueDelete(QueueHandle_t q) { (void)q; /* leaked on purpose: a task may still reference it */ }

static BaseType_t qSend(QueueHandle_t q, const void *item, TickType_t ticks, bool front, bool overwrite) {
  if (!q) return pdFAIL;
  uint64_t dl = deadlineFor(ticks);
  for (;;) {
    if (q->items.size() < q->len || overwrite) {
      if (overwrite && !q->items.empty()) q->items.pop_back();
      std::vector<uint8_t> v((const uint8_t *)item, (const uint8_t *)item + q->itemSize);
      if (front) q->items.push_front(std::move(v)); else q->items.push_back(std::move(v));
      wakeWaiters(q);
      return pdPASS;
    }
    if (ticks == 0 || !self || !blockOn(q, dl)) return errQUEUE_FULL;
  }
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { return qSend(q, item, t, false, false); }
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t t) { return qSend(q, item, t, true, false); }
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item) { return qSend(q, item, 0, false, true); }
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *w) { if (w) *w = 0; return qSend(q, item, 0, false, false); }

static BaseType_t qRecv(QueueHandle_t q, void *item, TickType_t ticks, bool peek) {
  if (!q) return pdFAIL;
  uint64_t dl = deadlineFor(ticks);
  for (;;) {
    if (!q->items.empty()) {
      memcpy(item, q->items.front().data(), q->itemSize);
      if (!peek) { q->items.pop_front(); wakeWaiters(q); }
      return pdPASS;
    }
    if (ticks == 0 || !self || !blockOn(q, dl)) return errQUEUE_EMPTY;
  }
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t) { return qRecv(q, item, t, false); }
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t t) { return qRecv(q, item, t, true); }
BaseType_t xQueueReceiveFromISR(QueueHandle_t q, void *item, BaseType_t *w) { if (w) *w = 0; return qRecv(q, item, 0, false); }
BaseType_t xQueueReset(QueueHandle_t q) { if (q) { q->items.clear(); wakeWaiters(q); } return pdPASS; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return q ? (q->kind == QK::Queue ? q->items.size() : q->count) : 0; }
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) { return q ? q->len - q->items.size() : 0; }

static HQueue *mkSem(QK k, UBaseType_t max, UBaseType_t init) {
  HQueue *q = new HQueue();
  q->kind = k; q->max = max; q->count = init; q->len = 0; q->itemSize = 0;
  return q;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return mkSem(QK::Mutex, 1, 1); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return mkSem(QK::Recursive, 1, 1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return mkSem(QK::Binary, 1, 0); }
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t init) { return mkSem(QK::Counting, max, init); }
void vSemaphoreDelete(SemaphoreHandle_t) {}
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s) { return s ? s->count : 0; }
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t s) { return s ? s->holder : nullptr; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  if (!s) ewsimHostPanic("xSemaphoreTake on a null semaphore (the ESP32 asserts here)", __FILE__, __LINE__);
  if (s->kind == QK::Queue) ewsimHostPanic("xSemaphoreTake on a queue", __FILE__, __LINE__);
  if (s->kind == QK::Recursive && s->holder == self && self) { s->depth++; return pdTRUE; }
  uint64_t dl = deadlineFor(ticks);
  for (;;) {
    if (s->count > 0) {
      s->count--;
      if (s->kind == QK::Mutex || s->kind == QK::Recursive) { s->holder = self; s->depth = 1; }
      return pdTRUE;
    }
    if (s->kind == QK::Mutex && s->holder == self && self && ticks == portMAX_DELAY)
      ewsimHostPanic("task re-took a non-recursive mutex it holds (would deadlock on the watch)", __FILE__, __LINE__);
    if (ticks == 0 || !self || !blockOn(s, dl)) return pdFALSE;
  }
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t t) { return xSemaphoreTake(s, t); }

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  if (!s) return pdFALSE;
  if (s->kind == QK::Recursive && s->depth > 1) { s->depth--; return pdTRUE; }
  if (s->count >= s->max) return pdFALSE;
  s->count++;
  if (s->kind == QK::Mutex || s->kind == QK::Recursive) { s->holder = nullptr; s->depth = 0; }
  wakeWaiters(s);
  return pdTRUE;
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return xSemaphoreGive(s); }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t *w) { if (w) *w = 0; return xSemaphoreGive(s); }
BaseType_t xSemaphoreTakeFromISR(SemaphoreHandle_t s, BaseType_t *w) { if (w) *w = 0; return xSemaphoreTake(s, 0); }

}  // extern "C"
