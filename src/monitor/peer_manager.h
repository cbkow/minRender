#pragma once

#include "core/peer_info.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

namespace MR {

class PeerManager
{
public:
    void start(const std::filesystem::path& farmPath,
               const std::string& nodeId,
               const std::string& localEndpoint,   // "ip:port"
               const std::vector<std::string>& localTags = {});
    void stop();

    // Thread-safe snapshot for UI and /api/peers
    std::vector<PeerInfo> getPeerSnapshot() const;

    // Thread-safe leader query
    bool isLeader() const { return m_isLeader.load(); }
    std::string leaderId() const;

    // Thread-safe setters (called from main thread when state changes)
    void setRenderState(const std::string& state,
                        const std::string& jobId = {},
                        const std::string& chunk = {});
    void setNodeState(const std::string& state);  // "active" | "stopped"

    // Optimistic update: set a remote peer's node_state locally (instant UI feedback)
    void setPeerNodeState(const std::string& nodeId, const std::string& state);

    // Filesystem restart signal (checked from background thread, consumed from main thread)
    bool consumeRestartSignal();

    // UDP multicast fast path (called from main thread via MonitorApp)
    void processUdpHeartbeat(const std::string& nodeId, const std::string& ip,
                             uint16_t port, const std::string& nodeState,
                             const std::string& renderState, const std::string& jobId,
                             const std::string& chunk, int priority,
                             const std::string& agentHealth = "ok",
                             const std::string& alertReason = "",
                             bool readyForWork = true);
    void processUdpGoodbye(const std::string& nodeId);

private:
    void threadFunc();

    // Write own endpoint.json to {farmPath}/nodes/{nodeId}/
    void writeEndpoint();

    // Re-create own endpoint.json if it vanished (farm cleanup, forgetPeer)
    void ensureEndpointPresent();

    // Remove own endpoint.json on shutdown
    void removeEndpoint();

    // Scan {farmPath}/nodes/*/endpoint.json for new peers
    void discoverPeers();

    // What the directory listing says about a node's endpoint.json:
    // modification time + size, or nullopt if absent. Directory
    // enumeration is the one metadata source the Windows SMB client does
    // not serve from a cached file handle (see discoverPeers).
    struct EndpointStamp
    {
        int64_t   mtime = 0;
        uintmax_t size  = 0;
        bool operator==(const EndpointStamp& o) const { return mtime == o.mtime && size == o.size; }
        bool operator!=(const EndpointStamp& o) const { return !(*this == o); }
    };
    static std::optional<EndpointStamp> findEndpointEntry(const std::filesystem::path& nodeDir);

    // Per-node read bookkeeping for the mtime-gated endpoint.json reads
    struct EndpointReadState
    {
        bool          haveRead = false;
        EndpointStamp stamp;              // listing stamp at the last good read
        int64_t       timestampMs = 0;    // writer's timestamp_ms at the last good read
        int64_t       retryAfterMs = 0;   // back-off after a failed or stale read
        int           staleRetries = 0;   // consecutive stale-looking reads
        int64_t       lastWarnMs = 0;     // rate-limit for read warnings
    };
    std::map<std::string, EndpointReadState> m_endpointReads;  // peer thread only
    int64_t m_lastEndpointWriteMs = 0;                          // peer thread only

    // HTTP GET /api/status to each known peer
    void pollPeers();

    // Recompute leader from alive peers + self
    void recomputeLeader();

    // Check for {farmPath}/nodes/{nodeId}/restart signal file
    void checkRestartSignal();

    std::filesystem::path m_farmPath;
    std::string m_nodeId;
    std::string m_localEndpoint;
    std::vector<std::string> m_localTags;

    mutable std::mutex m_mutex;
    std::map<std::string, PeerInfo> m_peers;  // nodeId → PeerInfo (excludes self)
    std::atomic<bool> m_isLeader{false};
    std::string m_leaderId;  // guarded by m_mutex

    // Local render state (set from main thread, read from HTTP handler thread)
    mutable std::mutex m_stateMutex;
    std::string m_renderState = "idle";
    std::string m_activeJob;
    std::string m_activeChunk;
    std::string m_nodeState = "active";

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_restartSignaled{false};
};

} // namespace MR
