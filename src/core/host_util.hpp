#ifndef VSTREAMER_CORE_HOST_UTIL_HPP
#define VSTREAMER_CORE_HOST_UTIL_HPP

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace vstreamer
{

inline int parse_host_port(std::string_view spec, char *host, size_t host_cap, int *port)
{
    if (nullptr == host || 0 == host_cap || nullptr == port || spec.empty())
    {
        return -EINVAL;
    }

    char buf[256];
    if (spec.size() >= sizeof(buf))
    {
        return -EINVAL;
    }
    std::memcpy(buf, spec.data(), spec.size());
    buf[spec.size()] = '\0';

    char *colon = std::strchr(buf, ':');
    if (nullptr == colon || colon == buf || colon[1] == '\0')
    {
        return -EINVAL;
    }
    *colon = '\0';

    char *end = nullptr;
    long p = std::strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || p <= 0 || p > 65535)
    {
        return -EINVAL;
    }

    if (std::strlen(buf) >= host_cap)
    {
        return -EINVAL;
    }
    std::memcpy(host, buf, std::strlen(buf) + 1);
    *port = static_cast<int>(p);
    return 0;
}

/* IPv4 UDP destination; host may be a name or numeric address. */
inline int resolve_ipv4_destination(const char *host, int port, sockaddr_in *out)
{
    if (nullptr == host || nullptr == out || port <= 0 || port > 65535)
    {
        return -EINVAL;
    }

    char port_str[16];
    if (std::snprintf(port_str, sizeof(port_str), "%d", port) < 0)
    {
        return -EINVAL;
    }

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    addrinfo *res = nullptr;
    const int gai = getaddrinfo(host, port_str, &hints, &res);
    if (0 != gai || nullptr == res)
    {
        return -EINVAL;
    }

    if (res->ai_addrlen < static_cast<socklen_t>(sizeof(sockaddr_in)))
    {
        freeaddrinfo(res);
        return -EINVAL;
    }

    std::memcpy(out, res->ai_addr, sizeof(sockaddr_in));
    out->sin_port = htons(static_cast<uint16_t>(port));
    freeaddrinfo(res);
    return 0;
}

/* IPv4 address for bind(); numeric host only unless resolve_name is true. */
inline int resolve_ipv4_bind_addr(const char *host, bool resolve_name, in_addr *out)
{
    if (nullptr == host || nullptr == out)
    {
        return -EINVAL;
    }

    if (0 == std::strcmp(host, "0") || 0 == std::strcmp(host, "0.0.0.0") ||
        0 == host[0])
    {
        out->s_addr = INADDR_ANY;
        return 0;
    }

    if (inet_pton(AF_INET, host, out) == 1)
    {
        return 0;
    }

    if (!resolve_name)
    {
        return -EINVAL;
    }

    sockaddr_in sa {};
    if (resolve_ipv4_destination(host, 1, &sa) < 0)
    {
        return -EINVAL;
    }
    *out = sa.sin_addr;
    return 0;
}

}  // namespace vstreamer

#endif  // VSTREAMER_CORE_HOST_UTIL_HPP
