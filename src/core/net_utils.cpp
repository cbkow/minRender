#include "core/net_utils.h"
#include <min_paths.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winnetwk.h>
#include <vector>
#else
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/param.h>
#include <sys/mount.h>
#endif
#endif

#include <cstring>
#include <string>

namespace MR {

std::string getLocalIpAddress()
{
#ifdef _WIN32
    ULONG bufSize = 15000;
    std::vector<BYTE> buf(bufSize);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());

    ULONG ret = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &bufSize);

    if (ret == ERROR_BUFFER_OVERFLOW)
    {
        buf.resize(bufSize);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        ret = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &bufSize);
    }

    if (ret != NO_ERROR)
        return "127.0.0.1";

    for (auto* adapter = addrs; adapter; adapter = adapter->Next)
    {
        // Skip loopback, tunnel, and down adapters
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        for (auto* ua = adapter->FirstUnicastAddress; ua; ua = ua->Next)
        {
            if (ua->Address.lpSockaddr->sa_family != AF_INET)
                continue;

            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ipStr[INET_ADDRSTRLEN] = {};
            InetNtopA(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

            // Skip loopback
            if (std::strcmp(ipStr, "127.0.0.1") == 0)
                continue;

            return ipStr;
        }
    }

    return "127.0.0.1";

#else
    struct ifaddrs* ifList = nullptr;
    if (getifaddrs(&ifList) != 0)
        return "127.0.0.1";

    std::string result = "127.0.0.1";
    for (auto* ifa = ifList; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;

        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));

        if (std::strcmp(ipStr, "127.0.0.1") == 0)
            continue;

        result = ipStr;
        break;
    }

    freeifaddrs(ifList);
    return result;
#endif
}

std::string getLocalIpAddressTowards(const std::string& host)
{
    if (host.empty())
        return {};

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return {};
    struct WsaGuard { ~WsaGuard() { WSACleanup(); } } wsaGuard;
    using SockT = SOCKET;
    const SockT invalid = INVALID_SOCKET;
#else
    using SockT = int;
    const SockT invalid = -1;
#endif

    // Resolve (handles both dotted IPs and names). Any UDP port works —
    // connect() on a datagram socket only picks the route and source
    // address, nothing goes on the wire.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), "445", &hints, &res) != 0 || !res)
        return {};

    std::string result;
    for (auto* ai = res; ai && result.empty(); ai = ai->ai_next)
    {
        SockT s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == invalid)
            continue;
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0)
        {
            sockaddr_in local{};
#ifdef _WIN32
            int len = sizeof(local);
#else
            socklen_t len = sizeof(local);
#endif
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0)
            {
                char ipStr[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &local.sin_addr, ipStr, sizeof(ipStr)) &&
                    std::strcmp(ipStr, "0.0.0.0") != 0 &&
                    std::strcmp(ipStr, "127.0.0.1") != 0)
                {
                    result = ipStr;
                }
            }
        }
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }
    freeaddrinfo(res);
    return result;
}

std::string shareHostForPath(const std::string& path)
{
    // min-paths `classify::share_host_for_path` (MinRender's rules: UNC host with `user@` stripped, a mapped drive
    // through WNetGetConnectionW, a macOS mount's statfs source — `//user@host/share` or `host:/export`).
    char* host = mp_share_host_for_path(path.c_str());
    std::string out = host ? host : "";
    mp_free(host);
    return out;
}

std::pair<std::string, int> parseEndpoint(const std::string& endpoint)
{
    auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == endpoint.size() - 1)
        return {"", 0};

    std::string host = endpoint.substr(0, colon);
    int port = 0;
    try { port = std::stoi(endpoint.substr(colon + 1)); }
    catch (...) { return {"", 0}; }

    if (port <= 0 || port > 65535)
        return {"", 0};

    return {host, port};
}

} // namespace MR
