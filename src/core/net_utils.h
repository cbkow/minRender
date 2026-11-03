#pragma once

#include <string>
#include <utility>
#include <cstdint>

namespace MR {

// Returns the first non-loopback IPv4 address, or "127.0.0.1" if none found.
// Windows: GetAdaptersAddresses (Iphlpapi.lib)
// Other:   getifaddrs()
// Fallback only — it takes adapters in enumeration order, so a tunnel
// (Tailscale, WireGuard) can win over the LAN. Prefer
// getLocalIpAddressTowards() when a peer-reachable host is known.
std::string getLocalIpAddress();

// The local IPv4 address the OS would use to reach `host` (IP or name),
// found by connecting a UDP socket to it and reading the bound source
// address — no packet is sent. This is the address peers that share that
// host (the farm share) can reach us on: LAN in the office, the VPN
// tunnel at home. Returns "" if the host cannot be resolved or routed.
std::string getLocalIpAddressTowards(const std::string& host);

// The server a sync-root path lives on: the host of a UNC path
// (\\host\share or //host/share), the SMB server behind a macOS mount
// point or a Windows mapped drive. Returns "" for a local path.
std::string shareHostForPath(const std::string& path);

// Parse "host:port" → {host, port}. Returns {"", 0} on failure.
std::pair<std::string, int> parseEndpoint(const std::string& endpoint);

} // namespace MR
