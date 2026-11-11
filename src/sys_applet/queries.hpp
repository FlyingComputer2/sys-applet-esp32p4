// The ESP32-P4's chip-level system queries, as one-line text values. The
// applet (applet.hpp) serves them as `get sys <key>`; a firmware can also call
// them directly, for example for a periodic serial line.
#pragma once

#include <cstdint>
#include <limits>
#include <map>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace sys_applet
{

// CPU share per FreeRTOS task since this window's previous line() (the first
// call covers the time since boot), busiest first:
//   "window_ms=<ms> <name>:<core>:<prio>:<percent> ..."
// core '-' = unpinned; percent is of one core. Tasks are told apart by
// handle, since names repeat (every pthread is "pthread"). A window longer
// than the run-time counter's range (about 71 min with the default 32-bit
// counter) cannot be measured: that call only restarts the window and returns
// "window_ms=0". Needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; empty without
// it. Not thread-safe: keep one window per caller.
class cpu_window
{
public:
    std::string line();

private:
    std::map<TaskHandle_t, configRUN_TIME_COUNTER_TYPE> last_;
    int64_t last_us_ = 0;
};

// "internal free=<B> min=<B> big=<B>; psram free=<B> min=<B> big=<B>"
// (min = low-water mark since boot, big = largest free block).
std::string heap_line();

// Where each task's stack is and its headroom:
//   "tasks=<n> <name>@<region>:<base>/<free bytes at the low-water mark> ..."
// Region L2 (internal), PS (PSRAM), LP (LP SRAM, slow for the CPU), TCM or ??.
// A stack in LP means internal RAM ran out when the task was created. Empty
// if the snapshot cannot be allocated (it is taken in PSRAM).
std::string stack_line();

// "uptime_s=<seconds since boot>"
std::string uptime_line();

// The last reset's reason: poweron, external, software, panic, int_wdt,
// task_wdt, wdt, deepsleep, brownout, sdio, usb, jtag or unknown.
std::string reset_line();

// "project=<name> version=<v> built=<date> <time> idf=<v> slot=<partition>"
std::string version_line();

}  // namespace sys_applet
