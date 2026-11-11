// The P4 system applet: `get sys <key>` on a UDP console and, optionally, an
// m-plane on two named bfc queues (an L3 QUEUE_FORWARDING upstream: request
// datagram in, reply out), both on a named bfc::default_reactor<>.
//
//   get sys <key>      "ok <value>" or "err <errno> <NAME>"
//   get sys keys       "ok <key> <key> ..."
//   nodes              "ok sys"
//   ping               "ok pong"
//
// Every reply ends in '\n' (the grammar of vstreamer's m-plane). Built-in
// keys: cpu, heap, stack, uptime, reset, version (queries.hpp). A firmware
// adds its own board-specific keys with add_query().
//
// configure() finds the reactor and queues by name; start() registers the
// callbacks: call it before the reactor thread runs (registration is not
// thread-safe). Everything then runs on the reactor thread. The applet lives
// for the program (no stop): keep it static. `get sys cpu` is the share since
// the previous cpu query from any client.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bfc/default_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>

#include "sys_applet/queries.hpp"

namespace sys_applet
{

using reactor_t = bfc::default_reactor<>;
using queue_t = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;

struct config
{
    // Name in bfc::default_reactor_registry.
    std::string reactor;
    // UDP console, one request per datagram, the reply to its sender; empty =
    // none.
    std::string console = "0.0.0.0:5130";
    // The m-plane queues (bfc's queue registry): the applet pushes replies
    // into mplane_out and serves requests from mplane_in. Empty = none.
    std::string mplane_out;
    std::string mplane_in;
};

class Applet
{
public:
    using query_fn = std::function<std::string()>;

    Applet();
    // Its callbacks hold `this`.
    Applet(const Applet&) = delete;
    Applet& operator=(const Applet&) = delete;

    // Adds or replaces `get sys <key>`. Call before start().
    void add_query(std::string key, query_fn fn);

    // False if the reactor or a named queue is missing.
    bool configure(const config& cfg);
    // False if not configured or the console cannot be bound.
    bool start();

    // One request line to its reply (what the console and m-plane call).
    std::string handle(std::string_view line);

private:
    config cfg_;
    std::shared_ptr<reactor_t> reactor_;
    std::shared_ptr<queue_t> out_, in_;
    std::vector<std::pair<std::string, query_fn>> queries_;
    cpu_window cpu_;
};

}  // namespace sys_applet
