# sys-applet-esp32p4

System queries for an ESP32-P4 firmware: per-task CPU, heap, where each task's stack lives,
uptime, reset reason and version. The applet serves them as `get sys <key>` on a UDP console
and, if configured, on an m-plane over two named [bfc](https://github.com/therooftopprinz/BFC)
queues (for example a winject-l3 `QUEUE_FORWARDING` upstream, so the board can be asked over
the radio). Both run on a named `bfc::default_reactor<>`. Each firmware creates its own applet
and adds its own board-specific keys; this repo holds only what is the same on every P4.

Illustrative replies (values made up):

```
get sys cpu       ok window_ms=10000 IDLE1:1:0:60.0 worker:0:13:45.0 IDLE0:0:0:52.0 ...
get sys heap      ok internal free=20000 min=9000 big=10000; psram free=...
get sys stack     ok tasks=30 worker@PS:48300000/5000 main@L2:...
get sys uptime    ok uptime_s=8000.0
get sys reset     ok software
get sys version   ok project=my-firmware version=... built=... idf=v5.5.5 slot=ota_0
get sys keys      ok cpu heap stack uptime reset version
nodes             ok sys
ping              ok pong
```

Replies are `ok <value>` or `err <errno> <NAME>`, each ending in `\n` (the grammar of
vstreamer's m-plane). Keys:

| key | value |
| --- | --- |
| `cpu` | CPU share per task since the previous `get sys cpu` from any client (the first one: since boot), busiest first: `<name>:<core>:<prio>:<percent of one core>` (core `-` = unpinned). A window longer than the run-time counter's range (about 71 min with the default 32-bit counter) only restarts it: `window_ms=0`, ask again. Needs `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`. |
| `heap` | internal and PSRAM: free bytes, low-water mark since boot (`min`), largest free block (`big`). |
| `stack` | `<task>@<region>:<stack base>/<free bytes at the low-water mark>`. Region `L2` (internal), `PS` (PSRAM), `LP` (LP SRAM, slow for the CPU), `TCM` or `??`. A stack in `LP` means internal RAM ran out when the task was created. Needs `CONFIG_FREERTOS_USE_TRACE_FACILITY`. |
| `uptime` | seconds since boot. |
| `reset` | reason of the last reset (`poweron`, `software`, `panic`, `task_wdt`, ...). |
| `version` | app project, version, build time, IDF version, running OTA partition. |

There is no `set`, and no authentication: the queries only read the board's state. Over a
winject-l3 upstream a reply is one datagram, so one longer than L3's application limit (1445 bytes)
is dropped; `cpu` and `stack` are about 25 bytes per task.

## Use

The parent project declares bfc with FetchContent (this repo pins none, so a firmware keeps one
bfc for all its libraries) and adds the component:

```cmake
FetchContent_Declare(sys_applet
    GIT_REPOSITORY https://github.com/FlyingComputer2/sys-applet-esp32p4.git
    GIT_TAG <commit>)
FetchContent_Populate(sys_applet)
list(APPEND EXTRA_COMPONENT_DIRS ${sys_applet_SOURCE_DIR}/idf/sys_applet)
```

```cpp
#include "sys_applet/applet.hpp"

static sys_applet::Applet sys;
sys_applet::config cfg;
cfg.reactor = "apps";            // bfc::default_reactor_registry name
cfg.console = "0.0.0.0:5130";    // empty = none
cfg.mplane_out = "sys_mp_tx";    // optional m-plane queues (bfc queue registry)
cfg.mplane_in = "sys_mp_rx";
sys.add_query("eth", [] { return std::string("up"); });  // board-specific
if (!sys.configure(cfg) || !sys.start()) { /* missing reactor / queue, or bind failed */ }
// ... then run the reactor.
```

`start()` must run before the reactor thread does (bfc registration is not thread-safe); after
that every request is handled on the reactor thread. The applet lives for the program, so keep
it static. The functions in `sys_applet/queries.hpp` can be called directly too, for example
for a periodic serial line (`cpu_window` keeps its own window per caller).

The stack-region address ranges are the ESP32-P4's.
