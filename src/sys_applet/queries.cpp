#include "sys_applet/queries.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

namespace sys_applet
{

std::string cpu_window::line()
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
    configRUN_TIME_COUNTER_TYPE total = 0;
    tasks.resize(uxTaskGetSystemState(tasks.data(), tasks.size(), &total));
    // The run-time counter is esp_timer (microseconds).
    const int64_t now_us = esp_timer_get_time();
    const double elapsed_us = double(now_us - last_us_);
    struct row
    {
        const char* name;
        BaseType_t core;
        unsigned prio;
        double pct;
    };
    std::vector<row> rows;
    // Longer than the counter's range: the deltas may have wrapped.
    const bool too_long = elapsed_us > double(std::numeric_limits<configRUN_TIME_COUNTER_TYPE>::max());
    std::map<TaskHandle_t, configRUN_TIME_COUNTER_TYPE> next;
    for (const auto& t : tasks)
    {
        const configRUN_TIME_COUNTER_TYPE counter = t.ulRunTimeCounter;
        const auto it = last_.find(t.xHandle);
        const configRUN_TIME_COUNTER_TYPE delta = it == last_.end() ? counter : counter - it->second;
        next[t.xHandle] = counter;
        rows.push_back({t.pcTaskName, xTaskGetCoreID(t.xHandle), unsigned(t.uxCurrentPriority),
                        elapsed_us > 0 ? 100.0 * delta / elapsed_us : 0.0});
    }
    last_.swap(next);
    last_us_ = now_us;
    if (too_long)
    {
        return "window_ms=0";
    }
    std::sort(rows.begin(), rows.end(), [](const row& a, const row& b) { return a.pct > b.pct; });
    std::string out = "window_ms=" + std::to_string(int64_t(elapsed_us / 1000));
    char buf[64];
    for (const auto& r : rows)
    {
        if (r.core == tskNO_AFFINITY)
        {
            std::snprintf(buf, sizeof(buf), " %s:-:%u:%.1f", r.name, r.prio, r.pct);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), " %s:%d:%u:%.1f", r.name, int(r.core), r.prio, r.pct);
        }
        out += buf;
    }
    return out;
#else
    return "";
#endif
}

std::string heap_line()
{
    char b[200];
    std::snprintf(b, sizeof(b), "internal free=%u min=%u big=%u; psram free=%u min=%u big=%u",
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                  unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                  unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                  unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    return b;
}

std::string stack_line()
{
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    const UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    // In PSRAM: this is asked about when internal RAM is short.
    auto* st = static_cast<TaskStatus_t*>(heap_caps_malloc(sizeof(TaskStatus_t) * n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (st == nullptr)
    {
        return "";
    }
    const UBaseType_t got = uxTaskGetSystemState(st, n, nullptr);
    std::string out = "tasks=" + std::to_string(got);
    char b[96];
    for (UBaseType_t i = 0; i < got; i++)
    {
        const auto base = uint32_t(reinterpret_cast<uintptr_t>(st[i].pxStackBase));
        const char* where = "??";
        if (base >= 0x50108000u && base < 0x50110000u)
        {
            where = "LP";
        }
        else if (base >= 0x4ff00000u && base < 0x4ffc0000u)
        {
            where = "L2";
        }
        else if (base >= 0x48000000u && base < 0x4c000000u)
        {
            where = "PS";
        }
        else if (base >= 0x30100000u && base < 0x30102000u)
        {
            where = "TCM";
        }
        std::snprintf(b, sizeof(b), " %s@%s:%x/%u", st[i].pcTaskName, where, unsigned(base),
                      unsigned(st[i].usStackHighWaterMark));
        out += b;
    }
    heap_caps_free(st);
    return out;
#else
    return "";
#endif
}

std::string uptime_line()
{
    char b[48];
    std::snprintf(b, sizeof(b), "uptime_s=%.1f", esp_timer_get_time() / 1e6);
    return b;
}

std::string reset_line()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        return "poweron";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_DEEPSLEEP:
        return "deepsleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    default:
        return "unknown";
    }
}

std::string version_line()
{
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_partition_t* slot = esp_ota_get_running_partition();
    return std::string("project=") + app->project_name + " version=" + app->version + " built=" + app->date + " " +
           app->time + " idf=" + app->idf_ver + " slot=" + (slot ? slot->label : "?");
}

}  // namespace sys_applet
