#include "sys_applet/applet.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include <bfc/default_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>

#include "esp_log.h"
#include "sys_applet/queries.hpp"

namespace sys_applet
{
namespace
{

const char* TAG = "sys_applet";

using reactor_t = bfc::default_reactor<>;
using queue_t = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using queue_registry = bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

// At most this many datagrams per socket or queue per reactor pass, so one
// busy source cannot hold the reactor (it comes back for the rest).
constexpr size_t k_max_batch = 16;

std::string err(int e, const char* name)
{
    return "err " + std::to_string(-e) + " " + name + "\n";
}

std::string usage()
{
    return "err usage: get sys <key> | get sys keys | nodes | ping\n";
}

std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> w;
    for (size_t i = 0; i < line.size();)
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
        {
            ++i;
        }
        size_t end = i;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t')
        {
            ++end;
        }
        if (end > i)
        {
            w.push_back(line.substr(i, end - i));
        }
        i = end;
    }
    return w;
}

// Binds host:port; -1 on error.
int bind_udp(const std::string& host_port)
{
    const size_t colon = host_port.rfind(':');
    if (colon == std::string::npos)
    {
        return -1;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::atoi(host_port.c_str() + colon + 1)));
    if (addr.sin_port == 0 || inet_pton(AF_INET, host_port.substr(0, colon).c_str(), &addr.sin_addr) != 1)
    {
        return -1;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    if (bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

struct Applet::Impl
{
    config cfg;
    std::shared_ptr<reactor_t> reactor;
    std::shared_ptr<queue_t> to_l3, from_l3;
    std::vector<std::pair<std::string, query_fn>> queries;
    cpu_window cpu;
    bool configured = false;

    std::string handle(std::string_view line);
    void serve_console(int fd);
    void serve_mplane();
};

std::string Applet::Impl::handle(std::string_view line)
{
    const auto w = words(line.substr(0, line.find_first_of("\r\n")));
    if (w.size() == 1 && w[0] == "ping")
    {
        return "ok pong\n";
    }
    if (w.size() == 1 && w[0] == "nodes")
    {
        return "ok sys\n";
    }
    if (w.size() >= 3 && w[0] == "set")
    {
        return w[1] == "sys" ? err(ENOTSUP, "ENOTSUP") : err(ENOENT, "ENOENT");
    }
    if (w.size() != 3 || w[0] != "get")
    {
        return usage();
    }
    if (w[1] != "sys")
    {
        return err(ENOENT, "ENOENT");
    }
    if (w[2] == "keys")
    {
        std::string out = "ok";
        for (const auto& q : queries)
        {
            out += " " + q.first;
        }
        return out + "\n";
    }
    for (const auto& q : queries)
    {
        if (q.first == w[2])
        {
            const std::string value = q.second();
            // Built-ins return empty when unsupported or out of memory.
            return value.empty() ? err(ENODATA, "ENODATA") : "ok " + value + "\n";
        }
    }
    return err(ENOENT, "ENOENT");
}

// One request per datagram, the reply to its sender.
void Applet::Impl::serve_console(int fd)
{
    reactor->add_read_rdy(fd,
                          [this, fd]
                          {
                              for (size_t i = 0; i < k_max_batch; ++i)
                              {
                                  char buf[512];
                                  sockaddr_in from {};
                                  socklen_t from_len = sizeof(from);
                                  const ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                                                             reinterpret_cast<sockaddr*>(&from), &from_len);
                                  if (n <= 0)
                                  {
                                      return;
                                  }
                                  const std::string reply = handle(std::string_view(buf, size_t(n)));
                                  sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<const sockaddr*>(&from),
                                         from_len);
                              }
                          });
}

// Each request datagram from L3 handled, its reply pushed back; a push from L3
// wakes the reactor.
void Applet::Impl::serve_mplane()
{
    reactor->add_read_rdy(*from_l3,
                          [this]
                          {
                              for (size_t i = 0; i < k_max_batch; ++i)
                              {
                                  auto packet = from_l3->pop();
                                  if (!packet)
                                  {
                                      return;
                                  }
                                  const std::string reply = handle(std::string_view(
                                      reinterpret_cast<const char*>(packet->data()), packet->size()));
                                  to_l3->push(bfc::shared_sized_buffer::copy_from(
                                      reinterpret_cast<const uint8_t*>(reply.data()), reply.size()));
                              }
                          });
    from_l3->set_reactor(reactor);
}

Applet::Applet() : impl_(std::make_unique<Impl>())
{
    Impl* impl = impl_.get();
    impl->queries = {
        {"cpu", [impl] { return impl->cpu.line(); }},
        {"heap", heap_line},
        {"stack", stack_line},
        {"uptime", uptime_line},
        {"reset", reset_line},
        {"version", version_line},
    };
}

Applet::~Applet() = default;

void Applet::add_query(std::string key, query_fn fn)
{
    for (auto& q : impl_->queries)
    {
        if (q.first == key)
        {
            q.second = std::move(fn);
            return;
        }
    }
    impl_->queries.emplace_back(std::move(key), std::move(fn));
}

bool Applet::configure(const config& cfg)
{
    if (impl_->configured)
    {
        return false;
    }
    impl_->cfg = cfg;
    impl_->reactor = bfc::default_reactor_registry::get_instance().get(cfg.reactor);
    if (!impl_->reactor)
    {
        ESP_LOGE(TAG, "no reactor %s", cfg.reactor.c_str());
        return false;
    }
    if (cfg.mplane_to_l3.empty() != cfg.mplane_from_l3.empty())
    {
        ESP_LOGE(TAG, "m-plane needs both queue names");
        return false;
    }
    if (!cfg.mplane_to_l3.empty())
    {
        impl_->to_l3 = queue_registry::get_instance().get(cfg.mplane_to_l3);
        impl_->from_l3 = queue_registry::get_instance().get(cfg.mplane_from_l3);
        if (!impl_->to_l3 || !impl_->from_l3)
        {
            ESP_LOGE(TAG, "no queues %s / %s", cfg.mplane_to_l3.c_str(), cfg.mplane_from_l3.c_str());
            return false;
        }
    }
    impl_->configured = true;
    return true;
}

bool Applet::start()
{
    if (!impl_->configured)
    {
        return false;
    }
    const int fd = bind_udp(impl_->cfg.console);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "cannot bind console %s", impl_->cfg.console.c_str());
        return false;
    }
    impl_->serve_console(fd);
    if (impl_->from_l3)
    {
        impl_->serve_mplane();
    }
    printf("sys: console %s%s (reactor %s)\n", impl_->cfg.console.c_str(),
           impl_->from_l3 ? " and m-plane upstream" : "", impl_->cfg.reactor.c_str());
    return true;
}

}  // namespace sys_applet
