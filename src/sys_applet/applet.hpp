// sys_applet: the ESP32-P4's system queries as an applet. The console and the
// m-plane run on a named reactor (bfc::default_reactor_registry); it has no
// task of its own.
//
//   get sys <key>      "ok <value>" or "err <errno> <NAME>"
//   get sys keys       "ok <key> <key> ..."
//   nodes              "ok sys"
//   ping               "ok pong"
//
// Every reply ends in '\n' (the grammar of vstreamer's m-plane). Built-in
// keys: cpu, heap, stack, uptime, reset, version (queries.hpp); a firmware
// adds its board's own with add_query(). `get sys cpu` is the share since the
// previous cpu query from any client.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace sys_applet
{

struct config
{
    // The m-plane upstream: an L3 QUEUE_FORWARDING upstream on two named
    // queues in bfc's queue registry (its rx = mplane_to_l3, tx =
    // mplane_from_l3). Empty = no m-plane.
    std::string mplane_to_l3;
    std::string mplane_from_l3;
    // The reactor (bfc::default_reactor_registry) the console and the m-plane
    // run on; start() registers on it before its thread runs.
    std::string reactor;
    // UDP console, one command per datagram, reply to the sender.
    std::string console = "0.0.0.0:5130";
};

// A board-specific `get sys <key>`: returns the value, or empty for an error
// (ENODATA). Runs on the reactor thread.
using query_fn = std::function<std::string()>;

// The system applet: configure() finds the reactor and the m-plane queues;
// start() registers the console and the m-plane on the reactor. Lives for the
// life of the program (no stop).
class Applet
{
public:
    Applet();
    ~Applet();
    Applet(const Applet&) = delete;
    Applet& operator=(const Applet&) = delete;
    // Adds or replaces `get sys <key>`; before start().
    void add_query(std::string key, query_fn fn);
    // False when there is no such reactor or m-plane queue (once only).
    bool configure(const config& cfg);
    // False when not configured or the console cannot be bound.
    bool start();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sys_applet
