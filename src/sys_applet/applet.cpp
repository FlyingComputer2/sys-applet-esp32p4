#include "sys_applet/applet.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "esp_log.h"

namespace sys_applet
{
namespace
{

const char* TAG = "sys_applet";

// At most this many datagrams per socket or queue per reactor pass, so one
// busy source cannot hold the reactor (it comes back for the rest).
constexpr size_t k_max_batch = 16;

using queue_registry = bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;

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

Applet::Applet()
{
    queries_ = {
        {"cpu", [this] { return cpu_.line(); }},
        {"heap", heap_line},
        {"stack", stack_line},
        {"uptime", uptime_line},
        {"reset", reset_line},
        {"version", version_line},
    };
}

void Applet::add_query(std::string key, query_fn fn)
{
    for (auto& q : queries_)
    {
        if (q.first == key)
        {
            q.second = std::move(fn);
            return;
        }
    }
    queries_.emplace_back(std::move(key), std::move(fn));
}

std::string Applet::handle(std::string_view line)
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
        for (const auto& q : queries_)
        {
            out += " " + q.first;
        }
        return out + "\n";
    }
    for (const auto& q : queries_)
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

bool Applet::configure(const config& cfg)
{
    cfg_ = cfg;
    reactor_ = bfc::default_reactor_registry::get_instance().get(cfg.reactor);
    if (!reactor_)
    {
        ESP_LOGE(TAG, "no reactor %s", cfg.reactor.c_str());
        return false;
    }
    if (cfg.mplane_in.empty() != cfg.mplane_out.empty())
    {
        ESP_LOGE(TAG, "m-plane needs both queue names");
        return false;
    }
    if (!cfg.mplane_in.empty())
    {
        out_ = queue_registry::get_instance().get(cfg.mplane_out);
        in_ = queue_registry::get_instance().get(cfg.mplane_in);
        if (!out_ || !in_)
        {
            ESP_LOGE(TAG, "no queues %s / %s", cfg.mplane_out.c_str(), cfg.mplane_in.c_str());
            return false;
        }
    }
    return true;
}

bool Applet::start()
{
    if (!reactor_)
    {
        return false;
    }
    if (!cfg_.console.empty())
    {
        const int fd = bind_udp(cfg_.console);
        if (fd < 0)
        {
            ESP_LOGE(TAG, "cannot bind console %s", cfg_.console.c_str());
            return false;
        }
        reactor_->add_read_rdy(fd,
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
                                       sendto(fd, reply.data(), reply.size(), 0,
                                              reinterpret_cast<const sockaddr*>(&from), from_len);
                                   }
                               });
    }
    if (in_)
    {
        reactor_->add_read_rdy(*in_,
                               [this]
                               {
                                   for (size_t i = 0; i < k_max_batch; ++i)
                                   {
                                       auto packet = in_->pop();
                                       if (!packet)
                                       {
                                           return;
                                       }
                                       const std::string reply = handle(std::string_view(
                                           reinterpret_cast<const char*>(packet->data()), packet->size()));
                                       out_->push(bfc::shared_sized_buffer::copy_from(
                                           reinterpret_cast<const uint8_t*>(reply.data()), reply.size()));
                                   }
                               });
        in_->set_reactor(reactor_);
    }
    printf("sys: console %s%s (reactor %s)\n", cfg_.console.empty() ? "none" : cfg_.console.c_str(),
           in_ ? " and m-plane queues" : "", cfg_.reactor.c_str());
    return true;
}

}  // namespace sys_applet
