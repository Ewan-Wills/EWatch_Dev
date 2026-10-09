// ewsim_host: internal host API shared by the shims and the runner.
#pragma once
#include <stdint.h>
#include <stddef.h>

uint64_t sim_now_us();
void sim_advance_us(uint64_t us);      // the running task spent this long (bus transfer, busy wait)
void sim_clock_tick();                 // called on every clock read (busy-wait guard)
const char *sim_current_task_name();
void sim_create_task(void (*fn)(void *), const char *name, void *arg, int prio, int core);
int  sim_run();                        // main thread: run the scheduler until sim_stop()
void sim_stop();                       // from a task: end the run

// console (Serial)
void host_serial_inject(const char *line);       // bytes the host PC sends
size_t host_serial_tx_size();
const char *host_serial_tx_data();               // everything the watch has printed
void host_serial_set_echo(bool on);              // mirror watch output to stderr

// hardware state visible to the runner
uint8_t host_backlight();                        // LEDC duty on the backlight channel
uint32_t host_cpu_mhz();
void host_set_rtc(int year, int mon, int day, int hour, int min, int sec);
void host_seed_random(uint64_t seed);

// notes / failures raised by stubs for hardware the simulator can't run
void ewsimHostUnsupported(const char *what);     // fails the run unless --allow-unsupported
void ewsimHostNote(const char *what);            // printed once in the summary
int  host_unsupported_count();
const char *host_unsupported_list();
const char *host_notes();

extern "C" void ewsimHostPanic(const char *what, const char *file, int line) __attribute__((noreturn));
