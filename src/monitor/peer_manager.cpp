#include "monitor/peer_manager.h"
#include "core/monitor_log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace MR {

static int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void PeerManager::start(const std::filesystem::path& farmPath,
                        const std::string& nodeId,
                        const std::string& localEndpoint,
                        const std::vector<std::string>& localTags)
{
    if (m_running.load())
        return;

    m_farmPath = farmPath;
    m_nodeId = nodeId;
    m_localEndpoint = localEndpoint;
    m_localTags = localTags;
    m_endpointReads.clear();
    m_lastEndpointWriteMs = 0;

    // Write initial endpoint immediately
    writeEndpoint();

    m_running.store(true);
    m_thread = std::thread(&PeerManager::threadFunc, this);
}

void PeerManager::stop()
{
    if (!m_running.load())
        return;

    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();

    removeEndpoint();
}

std::vector<PeerInfo> PeerManager::getPeerSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PeerInfo> result;
    result.reserve(m_peers.size());
    for (const auto& [id, info] : m_peers)
        result.push_back(info);
    return result;
}

std::string PeerManager::leaderId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_leaderId;
}

void PeerManager::setRenderState(const std::string& state,
                                 const std::string& jobId,
                                 const std::string& chunk)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_renderState = state;
    m_activeJob = jobId;
    m_activeChunk = chunk;
}

void PeerManager::setNodeState(const std::string& state)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_nodeState = state;
}

void PeerManager::setPeerNodeState(const std::string& nodeId, const std::string& state)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_peers.find(nodeId);
    if (it != m_peers.end())
        it->second.node_state = state;
}

void PeerManager::processUdpHeartbeat(const std::string& nodeId, const std::string& ip,
                                       uint16_t port, const std::string& nodeState,
                                       const std::string& renderState, const std::string& jobId,
                                       const std::string& chunk, int priority,
                                       const std::string& agentHealth,
                                       const std::string& alertReason,
                                       bool readyForWork)
{
    // ip/port are still parsed off the wire (and kept in the signature to
    // mirror the heartbeat schema) but are intentionally not consumed —
    // see the endpoint-authority note below.
    (void)ip;
    (void)port;

    auto now = nowMs();
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_peers.find(nodeId);
    if (it == m_peers.end())
    {
        // Unknown node. Deliberately NOT discovered here: multicast is
        // unauthenticated, so anything on the LAN can emit a heartbeat. A
        // UDP-created peer carried an attacker-chosen endpoint, and the
        // leader then POSTed /api/dispatch/assign to it *with the farm
        // secret in the Authorization header* — one spoofed packet was
        // enough to exfiltrate the shared secret.
        //
        // discoverPeers() is the only path that creates peers, from
        // nodes/<id>/endpoint.json on the shared farm filesystem. That
        // directory sits beside farm.json, so writing it already requires
        // the secret. It runs on the same ~3s tick as this, so legitimate
        // discovery is no slower in practice.
        return;
    }

    // Known peer: multicast carries fast liveness/state only. The endpoint
    // is NOT updated from UDP for the reason above — an IP change has to
    // come through endpoint.json.
    // (don't touch last_seen_ms — that tracks HTTP poll success for adaptive polling)
    it->second.node_state = nodeState;
    it->second.render_state = renderState;
    it->second.active_job = jobId;
    it->second.active_chunk = chunk;
    it->second.priority = priority;
    it->second.agent_health = agentHealth;
    it->second.alert_reason = alertReason;
    it->second.ready_for_work = readyForWork;
    it->second.is_alive = true;
    it->second.failed_polls = 0;
    it->second.has_udp_contact = true;
    it->second.last_udp_contact_ms = now;
}

void PeerManager::processUdpGoodbye(const std::string& nodeId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_peers.find(nodeId);
    if (it != m_peers.end())
    {
        it->second.is_alive = false;
        it->second.has_udp_contact = false;
        MonitorLog::instance().info("peer", "Peer goodbye via UDP: " + nodeId);
    }
}

// --- Background thread ---

void PeerManager::threadFunc()
{
    MonitorLog::instance().info("peer", "PeerManager started (endpoint: " + m_localEndpoint + ")");

    while (m_running.load())
    {
        ensureEndpointPresent();
        checkRestartSignal();
        discoverPeers();
        pollPeers();
        recomputeLeader();

        // Sleep ~3 seconds, checking for stop every 100ms
        for (int i = 0; i < 30 && m_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    MonitorLog::instance().info("peer", "PeerManager stopped");
}

std::optional<PeerManager::EndpointStamp>
PeerManager::findEndpointEntry(const std::filesystem::path& nodeDir)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(nodeDir, ec))
    {
        if (entry.path().filename() != "endpoint.json")
            continue;
        // Read the stamp off the directory_entry, not the path: on MSVC the
        // entry carries the FindFirstFile data, which is what keeps this
        // fresh when a by-path query would be answered from the cache.
        EndpointStamp st;
        auto ft = entry.last_write_time(ec);
        if (ec)
            return std::nullopt;
        st.mtime = std::chrono::duration_cast<std::chrono::milliseconds>(
            ft.time_since_epoch()).count();
        st.size = entry.file_size(ec);
        if (ec)
            st.size = 0;
        return st;
    }
    return std::nullopt;
}

void PeerManager::writeEndpoint()
{
    auto nodeDir = m_farmPath / "nodes" / m_nodeId;
    std::error_code ec;
    std::filesystem::create_directories(nodeDir, ec);

    auto endpointPath = nodeDir / "endpoint.json";

    // Parse ip:port from m_localEndpoint
    std::string ip = m_localEndpoint;
    uint16_t port = 8420;
    auto colonPos = m_localEndpoint.rfind(':');
    if (colonPos != std::string::npos)
    {
        ip = m_localEndpoint.substr(0, colonPos);
        port = static_cast<uint16_t>(std::stoi(m_localEndpoint.substr(colonPos + 1)));
    }

    PeerEndpoint ep;
    ep.node_id = m_nodeId;
    ep.ip = ip;
    ep.port = port;
    ep.timestamp_ms = nowMs();

    // Written once per farm start (and again only if the file vanishes),
    // NOT on a timer. Every rewrite is a rename-over on the share, and a
    // Windows SMB client that keeps reopening a file another Windows
    // client renames over can end up pinned to a cached handle of the old
    // file — it then reads the old content for as long as it keeps
    // reopening, and an IP change never propagates (2026-11-03 incident).
    // Fewer rewrites mean fewer chances to trip that, and readers that
    // only open on a listing change let their handles go dormant between
    // writes. Nothing reads timestamp_ms; liveness is HTTP/UDP.
    try
    {
        nlohmann::json j = ep;
        // Write to temp file then rename for atomicity
        auto tmpPath = endpointPath;
        tmpPath += ".tmp";
        {
            std::ofstream ofs(tmpPath);
            ofs << j.dump(2);
        }
        std::filesystem::rename(tmpPath, endpointPath, ec);
        if (ec)
        {
            // Fallback: direct write (rename can fail across filesystems)
            std::ofstream ofs(endpointPath);
            ofs << j.dump(2);
        }
        m_lastEndpointWriteMs = nowMs();
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("peer", std::string("Failed to write endpoint.json: ") + e.what());
    }
}

void PeerManager::ensureEndpointPresent()
{
    // Give the share's directory cache time to reflect our own write
    // before trusting a "missing" verdict (Windows caches listings ~10s).
    if (nowMs() - m_lastEndpointWriteMs < 15000)
        return;
    if (!findEndpointEntry(m_farmPath / "nodes" / m_nodeId))
    {
        MonitorLog::instance().info("peer", "endpoint.json missing on the share — rewriting");
        writeEndpoint();
    }
}

void PeerManager::removeEndpoint()
{
    auto endpointPath = m_farmPath / "nodes" / m_nodeId / "endpoint.json";
    std::error_code ec;
    std::filesystem::remove(endpointPath, ec);
}

void PeerManager::discoverPeers()
{
    auto nodesDir = m_farmPath / "nodes";
    std::error_code ec;
    if (!std::filesystem::is_directory(nodesDir, ec))
        return;

    const auto now = nowMs();

    for (const auto& entry : std::filesystem::directory_iterator(nodesDir, ec))
    {
        if (!entry.is_directory())
            continue;

        auto nodeId = entry.path().filename().string();
        if (nodeId == m_nodeId)
            continue;  // skip self

        // Presence and change detection come from the directory listing,
        // never from opening the file. On Windows, repeatedly opening a
        // file that another Windows client renames over can pin this
        // client to a cached handle of the *old* file: every open then
        // returns the old content (or fails with ERROR_INVALID_PARAMETER),
        // exists() and by-path attribute queries follow suit, and only the
        // listing stays truthful. So the file is opened only when the
        // listing says it changed, which also lets the cached handle go
        // dormant between changes so the next open is a real one.
        auto stamp = findEndpointEntry(entry.path());
        if (!stamp)
            continue;

        auto& rs = m_endpointReads[nodeId];

        bool known;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            known = m_peers.find(nodeId) != m_peers.end();
        }
        if (rs.haveRead && known && rs.stamp == *stamp)
            continue;  // unchanged since our last good read
        if (now < rs.retryAfterMs)
            continue;  // backing off after a failed/stale read

        auto endpointPath = entry.path() / "endpoint.json";
        auto warn = [&](const std::string& msg)
        {
            if (now - rs.lastWarnMs > 300000)
            {
                MonitorLog::instance().warn("peer", msg);
                rs.lastWarnMs = now;
            }
        };

        // Read and parse endpoint.json. This is the only authority for a
        // peer's endpoint (UDP heartbeats deliberately can't set one), so
        // known peers are re-read too rather than skipped — otherwise a
        // node that legitimately changed IP could never be corrected.
        PeerEndpoint ep;
        try
        {
            std::ifstream ifs(endpointPath);
            if (!ifs.good())
                throw std::runtime_error("open failed");
            auto j = nlohmann::json::parse(ifs);
            ep = j.get<PeerEndpoint>();
        }
        catch (const std::exception& e)
        {
            // Was a silent skip; a peer whose file can't be read is
            // invisible otherwise. Back off so a poisoned cached handle
            // can expire before the next attempt.
            rs.retryAfterMs = now + 15000;
            warn("Could not read endpoint.json for " + nodeId + " (" + e.what() +
                 ") — retrying in 15 s");
            continue;
        }

        // The directory name is the node's identity; a file claiming a
        // different node_id would key the peer map inconsistently.
        if (ep.node_id != nodeId)
            continue;

        // Stale-cache detection: the listing says the file changed, but
        // the content is byte-for-byte what we read last time (every
        // write stamps a new timestamp_ms). That is the SMB client
        // handing back its cached copy. Leave the file alone for a while
        // so the handle can expire, then try again.
        if (rs.haveRead && rs.stamp != *stamp && ep.timestamp_ms == rs.timestampMs)
        {
            // Escalate 20 s → 40 s (a cached handle was seen to outlive a
            // 3 s gap but not a 46 s one), then accept: a file that still
            // reads the same after a minute untouched is genuinely
            // unchanged (someone touched it without rewriting).
            if (rs.staleRetries < 2)
            {
                ++rs.staleRetries;
                const int64_t delay = 20000 * rs.staleRetries;
                rs.retryAfterMs = now + delay;
                warn("Stale read of endpoint.json for " + nodeId +
                     " (changed on the share, content unchanged — SMB client cache); retrying in " +
                     std::to_string(delay / 1000) + " s");
                continue;
            }
            MonitorLog::instance().info("peer", "endpoint.json for " + nodeId +
                " still reads unchanged after backing off — accepting it");
        }

        rs.haveRead = true;
        rs.stamp = *stamp;
        rs.timestampMs = ep.timestamp_ms;
        rs.retryAfterMs = 0;
        rs.staleRetries = 0;

        std::string endpoint = ep.ip + ":" + std::to_string(ep.port);

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_peers.find(nodeId);
        if (it != m_peers.end())
        {
            // Known peer — only correct the endpoint, keep live state.
            if (it->second.endpoint != endpoint)
            {
                MonitorLog::instance().info("peer", "Peer endpoint changed: " +
                    nodeId + " " + it->second.endpoint + " -> " + endpoint);
                it->second.endpoint = endpoint;
                it->second.failed_polls = 0;  // give the new address a clean run
            }
            continue;
        }

        PeerInfo info;
        info.node_id = ep.node_id;
        info.endpoint = endpoint;
        info.is_alive = true;
        info.failed_polls = 0;
        info.last_seen_ms = 0;  // not yet polled

        m_peers[nodeId] = info;
        MonitorLog::instance().info("peer", "Discovered peer: " + nodeId +
            " at " + info.endpoint);
    }
}

void PeerManager::pollPeers()
{
    auto now = nowMs();

    // Clear UDP contact for peers with >15s of UDP silence
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, info] : m_peers)
        {
            if (info.has_udp_contact && (now - info.last_udp_contact_ms > 15000))
            {
                info.has_udp_contact = false;
                MonitorLog::instance().info("peer", "UDP contact lost for: " + id);
            }

            // UDP silence >10s on a UDP-contacted peer = dead
            if (info.is_alive && info.last_udp_contact_ms > 0 &&
                !info.has_udp_contact && (now - info.last_udp_contact_ms > 10000))
            {
                // Only mark dead if also failing HTTP polls (belt + suspenders)
                // The 15s UDP clear above + 3 failed HTTP polls covers this
            }
        }
    }

    // Take a snapshot of peers to poll (avoid holding lock during HTTP calls)
    // Adaptive: peers with UDP contact get polled every ~9s (skip 2 out of 3 cycles)
    std::vector<std::tuple<std::string, std::string, double>> toCheck;  // nodeId, endpoint, rtt EWMA ms
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, info] : m_peers)
        {
            if (info.has_udp_contact && !info.hostname.empty())
            {
                // Skip HTTP poll if last successful HTTP poll was within 9s
                // (always poll at least once to get full hardware info)
                if (info.last_seen_ms > 0 && (now - info.last_seen_ms < 9000))
                    continue;
            }
            toCheck.emplace_back(id, info.endpoint, info.poll_rtt_ewma_ms);
        }
    }

    for (const auto& [nodeId, endpoint, rttEwma] : toCheck)
    {
        if (!m_running.load())
            return;

        // Parse ip:port
        std::string host = endpoint;
        int port = 8420;
        auto colonPos = endpoint.rfind(':');
        if (colonPos != std::string::npos)
        {
            host = endpoint.substr(0, colonPos);
            port = std::stoi(endpoint.substr(colonPos + 1));
        }

        // Fresh client per call (httplib::Client is not safe for concurrent reuse)
        // Timeouts adapt to the peer's observed RTT so a slow home-VPN
        // node isn't declared dead by LAN-tuned limits: 4× the smoothed
        // RTT, floored at the old LAN values (2s/3s), capped at 8s so a
        // truly dead peer still resolves in ~3 polls.
        const int64_t connectMs = std::clamp<int64_t>(
            static_cast<int64_t>(rttEwma * 4.0), 2000, 8000);
        httplib::Client cli(host, port);
        cli.set_connection_timeout(connectMs / 1000,
                                   (connectMs % 1000) * 1000);
        cli.set_read_timeout((connectMs + 1000) / 1000,
                             ((connectMs + 1000) % 1000) * 1000);

        const auto pollStart = std::chrono::steady_clock::now();
        auto res = cli.Get("/api/status");
        const double rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - pollStart).count();

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_peers.find(nodeId);
        if (it == m_peers.end())
            continue;

        if (res && res->status == 200)
        {
            try
            {
                auto j = nlohmann::json::parse(res->body);
                PeerInfo updated = j.get<PeerInfo>();

                // Preserve runtime fields
                updated.is_local = false;
                updated.is_alive = true;
                updated.is_leader = it->second.is_leader;
                updated.failed_polls = 0;
                updated.last_seen_ms = nowMs();
                updated.has_udp_contact = it->second.has_udp_contact;
                updated.last_udp_contact_ms = it->second.last_udp_contact_ms;

                // Preserve UDP-sourced health if HTTP response has default "ok"
                // (UDP updates more frequently than HTTP)
                if (updated.agent_health == "ok" && it->second.has_udp_contact)
                {
                    updated.agent_health = it->second.agent_health;
                    updated.alert_reason = it->second.alert_reason;
                }

                // Preserve UDP-sourced readiness (UDP updates more frequently)
                if (it->second.has_udp_contact)
                    updated.ready_for_work = it->second.ready_for_work;

                // Smooth the poll RTT (only successful polls sample it —
                // a timeout would just echo the timeout back).
                updated.poll_rtt_ewma_ms = it->second.poll_rtt_ewma_ms <= 0.0
                    ? rttMs
                    : it->second.poll_rtt_ewma_ms * 0.7 + rttMs * 0.3;

                it->second = updated;
            }
            catch (const std::exception&)
            {
                // Bad JSON — count as failure
                it->second.failed_polls++;
            }
        }
        else
        {
            it->second.failed_polls++;
        }

        // 3 consecutive failures = dead — but recent UDP contact vetoes
        // the verdict: the node is provably alive, its HTTP path is
        // just slow/blocked (typical for home-VPN peers), and killing
        // it here would trigger chunk reclaim under an active render.
        const bool recentUdp = it->second.last_udp_contact_ms > 0
            && (nowMs() - it->second.last_udp_contact_ms) < 10000;
        if (it->second.failed_polls >= 3 && it->second.is_alive && !recentUdp)
        {
            it->second.is_alive = false;
            MonitorLog::instance().warn("peer", "Peer dead: " + nodeId +
                " (" + std::to_string(it->second.failed_polls) + " failed polls)");
        }
    }

    // Remove peers whose endpoint.json is deleted AND are dead
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_peers.begin(); it != m_peers.end(); )
        {
            if (!it->second.is_alive)
            {
                // Listing, not exists(): see discoverPeers on SMB caching.
                if (!findEndpointEntry(m_farmPath / "nodes" / it->first))
                {
                    MonitorLog::instance().info("peer", "Removed stale peer: " + it->first);
                    m_endpointReads.erase(it->first);  // next appearance is read fresh
                    it = m_peers.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
}

void PeerManager::recomputeLeader()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Collect alive candidates: self + alive peers (including stopped nodes as fallback).
    // Stopped nodes can still lead (coordinate dispatch) even though they don't render.
    // Election rank: "leader" tag > NOT "noleader" tag > alphabetical node_id.
    // Deterministic sort — same inputs always produce the same winner.
    struct Candidate { std::string id; bool hasLeaderTag; bool hasNoLeaderTag; };
    std::vector<Candidate> candidates;

    // Self is always a candidate
    {
        std::lock_guard<std::mutex> slock(m_stateMutex);
        bool selfLeader = std::find(m_localTags.begin(), m_localTags.end(),
                                    "leader") != m_localTags.end();
        bool selfNoLeader = std::find(m_localTags.begin(), m_localTags.end(),
                                      "noleader") != m_localTags.end();
        candidates.push_back({m_nodeId, selfLeader, selfNoLeader});
    }

    for (const auto& [id, info] : m_peers)
    {
        if (info.is_alive)
        {
            bool peerLeader = std::find(info.tags.begin(), info.tags.end(),
                                        "leader") != info.tags.end();
            bool peerNoLeader = std::find(info.tags.begin(), info.tags.end(),
                                          "noleader") != info.tags.end();
            candidates.push_back({id, peerLeader, peerNoLeader});
        }
    }

    if (candidates.empty())
    {
        m_leaderId.clear();
        m_isLeader.store(false);
        return;
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
    {
        // "leader" tagged nodes first
        if (a.hasLeaderTag != b.hasLeaderTag)
            return a.hasLeaderTag > b.hasLeaderTag;
        // "noleader" tagged nodes last
        if (a.hasNoLeaderTag != b.hasNoLeaderTag)
            return a.hasNoLeaderTag < b.hasNoLeaderTag;
        // Alphabetical tiebreak
        return a.id < b.id;
    });

    std::string newLeader = candidates[0].id;
    bool nowLeader = (newLeader == m_nodeId);

    if (newLeader != m_leaderId)
    {
        m_leaderId = newLeader;
        MonitorLog::instance().info("peer", "Leader elected: " + newLeader +
            (nowLeader ? " (this node)" : ""));
    }

    m_isLeader.store(nowLeader);

    // Update leader flags on peers
    for (auto& [id, info] : m_peers)
        info.is_leader = (id == newLeader);
}

void PeerManager::checkRestartSignal()
{
    auto signalPath = m_farmPath / "nodes" / m_nodeId / "restart";
    std::error_code ec;
    if (!std::filesystem::exists(signalPath, ec))
        return;

    // Ignore stale signal files (older than 60 seconds)
    auto ftime = std::filesystem::last_write_time(signalPath, ec);
    if (!ec)
    {
        auto age = std::filesystem::file_time_type::clock::now() - ftime;
        auto ageSec = std::chrono::duration_cast<std::chrono::seconds>(age).count();
        if (ageSec > 60)
        {
            // Stale — delete and ignore
            std::filesystem::remove(signalPath, ec);
            MonitorLog::instance().info("peer", "Removed stale restart signal (" +
                std::to_string(ageSec) + "s old)");
            return;
        }
    }

    // Valid signal — consume it
    std::filesystem::remove(signalPath, ec);
    m_restartSignaled.store(true);
    MonitorLog::instance().info("peer", "Restart signal file detected and consumed");
}

bool PeerManager::consumeRestartSignal()
{
    return m_restartSignaled.exchange(false);
}

} // namespace MR
