#pragma once

#include "core/config.h"
#include "core/node_identity.h"
#include "core/peer_info.h"
#include "core/http_server.h"
#include "core/udp_notify.h"
#include "monitor/agent_supervisor.h"
#include "monitor/render_coordinator.h"
#include "monitor/template_manager.h"
#include "monitor/peer_manager.h"
#include "monitor/database_manager.h"
#include "monitor/dispatch_manager.h"
#include "monitor/submission_watcher.h"

#include "core/system_tray.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <mutex>

namespace MR {

enum class NodeState { Active, Stopped };

class MonitorApp
{
public:
    bool init();
    void update();
    void shutdown();

    // Config accessors
    Config& config() { return m_config; }
    const Config& config() const { return m_config; }
    const NodeIdentity& identity() const { return m_identity; }
    AgentSupervisor& agentSupervisor() { return m_agentSupervisor; }
    RenderCoordinator& renderCoordinator() { return m_renderCoordinator; }
    PeerManager& peerManager() { return m_peerManager; }
    DatabaseManager& databaseManager() { return m_databaseManager; }
    DispatchManager& dispatchManager() { return m_dispatchManager; }

    // Build this node's PeerInfo (called by HttpServer for GET /api/status)
    PeerInfo buildLocalPeerInfo() const;

    // Cached snapshots for UI
    const std::vector<JobInfo>& cachedJobs() const { return m_cachedJobs; }
    const std::vector<JobTemplate>& cachedTemplates() const { return m_cachedTemplates; }

    // Thread-safe cached JSON for HTTP handlers
    std::string getCachedJobsJson() const;
    std::string getCachedJobDetailJson(const std::string& jobId) const;

    // Chunk data access (leader: DB query, worker: HTTP GET from leader)
    std::vector<ChunkRow> getChunksForJob(const std::string& jobId);

    // Job controls
    void pauseJob(const std::string& jobId);
    void resumeJob(const std::string& jobId);
    // Reorder via the drag handles in the jobs panel. Crossing a
    // priority-group boundary makes the job adopt the target's priority.
    // Returns false when the leader rejects the move locally;
    // worker-side forwards always return true (result arrives async).
    bool moveJob(const std::string& jobId, const std::string& targetJobId, bool before);
    void setJobPriority(const std::string& jobId, int priority);
    // Outcome of a job edit. On workers the terminal code arrives async
    // (the leader round-trip); on the leader it is known synchronously.
    struct EditResult
    {
        enum class Code
        {
            Applied,         // leader stored + executed the edit
            InvalidManifest, // manifest JSON rejected
            JobNotFound,     // leader has no such job
            DbUnavailable,   // leader DB not ready
            LeaderError,     // leader answered an error
            NoResponse,      // no leader reachable / connect failed
            RouteMissing,    // leader too old for /edit (v0 feature set)
            QueuedToShare,   // leader unreachable — durably queued to
                             // {farmPath}/commands/ instead
        };
        Code code = Code::NoResponse;
        std::string message;
        bool ok() const { return code == Code::Applied; }
    };

    // Apply an edited manifest to an existing job. mode:
    //   "continue"  — manifest UPDATE only; next dispatches pick it up
    //   "restart"   — also return in-flight chunks to pending + abort them
    //   "startover" — also drop and recompute ALL chunks from the new
    //                 frame range / chunk size
    // `done` fires exactly once with the outcome — synchronously on the
    // leader, from the UI-thread-marshalled HTTP callback on workers.
    void editJob(const std::string& jobId, const std::string& manifestJson,
                 const std::string& mode, int priority,
                 std::function<void(const EditResult&)> done = nullptr);
    // Leader-local edit execution (DB mutation, chunk reset/rebuild,
    // abort pushes). Shared by editJob, the /edit HTTP handler, and the
    // farm-share command consumer.
    EditResult applyEditLocally(const std::string& jobId,
                                const std::string& manifestJson,
                                const std::string& mode, int priority);
    // Fallback command transport when the leader can't be reached over
    // HTTP: drop the edit as {farmPath}/commands/*.json for whichever
    // node is leader to consume (pollFarmCommands). Survives leader
    // handover — the share is the queue, same philosophy as the
    // endpoint.json phonebook. Returns false if the write failed.
    bool writeEditCommand(const std::string& jobId,
                          const std::string& manifestJson,
                          const std::string& mode, int priority);
    // Abort a local in-flight render of this job without failure
    // bookkeeping (see RenderCoordinator::abandonCurrentRender). Called
    // by the leader's edit push via POST /api/render/abort.
    void abandonJobRender(const std::string& jobId, const std::string& reason);
    // Full manifest JSON for a job — from the DB on the leader, via a
    // blocking HTTP GET to the leader on workers (same pattern as
    // getChunksForJob). Empty string when unavailable; errorOut (when
    // given) distinguishes why: "not_found", "route_missing" (old
    // leader), "no_leader", "unreachable", or a leader error body.
    std::string getJobManifestJson(const std::string& jobId,
                                   std::string* errorOut = nullptr);
    void cancelJob(const std::string& jobId);
    void requeueJob(const std::string& jobId);
    void deleteJob(const std::string& jobId);
    void archiveJob(const std::string& jobId);
    void retryFailedChunks(const std::string& jobId);
    void reassignChunk(int64_t chunkId, const std::string& targetNodeId = {});
    // Move a chunk to the terminal 'stopped' state and abort whoever is
    // rendering it (abandon semantics — no blacklist). Requeue resumes.
    void stopChunk(int64_t chunkId);
    std::string resubmitJob(const std::string& jobId);
    std::string resubmitChunkAsJob(const std::string& jobId,
                                    int frameStart, int frameEnd, int chunkSize);
    std::string resubmitIncomplete(const std::string& jobId);
    void unsuspendNode(const std::string& nodeId);

    // --- Suspension mirror ---------------------------------------------
    // The failure tracker lives only on the leader, so without this a
    // suspended node has no idea it has been benched and the UI shows it
    // as a healthy idle node that silently never gets work. Every node
    // mirrors the leader's verdicts here: the leader reads its own
    // tracker directly, workers poll GET /api/nodes/suspended.
    struct SuspensionInfo
    {
        int failure_count = 0;
        int64_t last_failure_ms = 0;
    };
    std::unordered_map<std::string, SuspensionInfo> suspendedNodes() const;
    bool isNodeSuspended(const std::string& nodeId) const;

    // Self-service recovery. unsuspendNode() already proxies to the
    // leader when this node isn't it, so a benched node can clear its own
    // entry without needing someone at another machine to do it.
    void unsuspendThisNode();
    bool isThisNodeSuspended() const;
    int thisNodeSuspendFailureCount() const;

    // Peer remote-control actions. Each looks up the peer's endpoint
    // from PeerManager and queues an HTTP POST on the shared worker
    // thread (no blocking from callers). No-op if the peer is unknown
    // or has no endpoint. setPeerNodeActive also applies the new state
    // locally on PeerManager so the UI reflects the change before the
    // peer's status poll confirms it.
    void setPeerNodeActive(const std::string& nodeId, bool active);
    void restartPeerApp(const std::string& nodeId);

    // Dead-peer fallback: write an empty file at
    // {farmPath}/nodes/{nodeId}/restart so the peer's own filesystem
    // watcher triggers a local restart next time it comes back. Returns
    // false if the farm isn't running or the write failed.
    bool writePeerRestartSignal(const std::string& nodeId);

    // Remove a peer's endpoint.json from the shared farm so
    // PeerManager's stale-peer reaper drops the cached record on its
    // next discovery pass. Used to clean up a duplicate peer row
    // caused by a prior install writing a second endpoint under a
    // different node_id. Returns false if the farm isn't running or
    // the file didn't exist (the latter is also the desired end
    // state, so callers shouldn't treat it as failure).
    bool forgetPeer(const std::string& nodeId);

    // Farm cleanup — shared implementation used by both /api/farm/*
    // endpoints and the Qt FarmCleanupDialog. scanFarmCleanup walks
    // the shared filesystem + DB + peer list and returns six groups:
    //   finished_jobs, archived_jobs, orphaned_dirs, stale_peers,
    //   stale_staging_dirs, failed_staging_copies
    // Each group item is {id, label, detail}. executeFarmCleanup
    // applies one of the actions (archive / delete_jobs / delete_dirs /
    // remove_peers) to the given ids and returns the count processed.
    nlohmann::json scanFarmCleanup() const;
    int executeFarmCleanup(const std::string& action,
                           const std::vector<std::string>& ids);

    // Node state controls
    void setNodeState(NodeState state);
    NodeState nodeState() const { return m_nodeState; }

    // Async HTTP to leader (fire-and-forget or with callback)
    void postToLeaderAsync(const std::string& endpoint, const std::string& body,
                           std::function<void(bool success, const std::string& response)> callback = nullptr,
                           const std::string& method = "POST");
    // Status-aware variant: callback gets the HTTP status (0 = no
    // response) so callers can tell unreachable from answered-error.
    void postToLeaderAsyncEx(const std::string& endpoint, const std::string& body,
                             std::function<void(int status, const std::string& response)> statusCallback,
                             const std::string& method = "POST");

    // Leader election (delegated to PeerManager)
    bool isLeader() const { return m_peerManager.isLeader(); }
    std::string getLeaderEndpoint() const;

    // True when the current leader advertises the capability token in
    // its /api/status. Old builds send no capabilities (= v0 set).
    bool leaderSupports(const std::string& capability) const;

    // Tray state
    // trayIconName() returns "green" | "blue" | "yellow" | "red" | "gray" for
    // external consumers (e.g. macOS Swift companion via IPC). The Windows
    // tray itself uses a single icon and surfaces status via trayStatusText().
    std::string trayIconName() const;
    std::string trayTooltip() const;
    std::string trayStatusText() const;

    // Restart (sidecar batch script that relaunches the app)
    bool launchRestartSidecar();

    // Exit flow
    void requestExit();
    bool isExitPending() const { return m_exitRequested && !m_shouldExit; }
    bool shouldExit() const { return m_shouldExit; }
    void beginForceExit();
    void cancelExit();

    void saveConfig();

    // Farm lifecycle
    bool startFarm();
    void stopFarm();
    void requestFarmRestart() { m_farmRestartRequested.store(true); }

    // The IPv4 address peers should use for this node: the override if
    // set, else the address that routes to the farm share's server.
    std::string advertisedIp() const;
    bool isFarmRunning() const { return m_farmRunning; }
    const std::filesystem::path& farmPath() const { return m_farmPath; }
    bool hasFarmError() const { return !m_farmError.empty(); }
    const std::string& farmError() const { return m_farmError; }
    const std::string& farmSecret() const { return m_farmSecret; }

    // Short non-reversible id for the loaded secret — for UI display and
    // /api/status. Empty when no secret is loaded. See core/sha256.h.
    std::string farmSecretFingerprint() const;

    // Job selection state
    void selectJob(const std::string& id);
    void requestSubmissionMode();
    const std::string& selectedJobId() const { return m_selectedJobId; }
    bool shouldEnterSubmission();

private:
    void loadConfig();
    void onBecomeLeader();
    void onLoseLeadership();
    // Leader-only consumer of {farmPath}/commands/ — applies queued
    // commands via applyEditLocally, acks by moving each file to
    // commands/processed/ (SubmissionWatcher idiom), drops entries
    // older than 15 min unapplied, prunes processed/ after 7 days.
    void pollFarmCommands();
    std::chrono::steady_clock::time_point m_lastCommandPoll{};
    void refreshCachedJobs();
    void refreshSuspendedNodes();
    void abortRenderIfJobGone();
    void reportCompletion(const std::string& jobId, const ChunkRange& chunk,
                          const std::string& state, int64_t editEpoch);
    void reportFrameCompletion(const std::string& jobId, int frame,
                               int64_t editEpoch);
    void handleUdpMessages();
    void sendUdpHeartbeat();

    // Background HTTP worker
    void startHttpWorker();
    void stopHttpWorker();
    void httpWorkerLoop();
    bool flushCompletionReports();
    bool flushFrameReports();

    std::filesystem::path m_appDataDir;
    std::filesystem::path m_configPath;

    NodeIdentity m_identity;
    Config m_config;
    AgentSupervisor m_agentSupervisor;
    RenderCoordinator m_renderCoordinator;
    TemplateManager m_templateManager;
    HttpServer m_httpServer;
    PeerManager m_peerManager;
    DatabaseManager m_databaseManager;
    DispatchManager m_dispatchManager;
    SubmissionWatcher m_submissionWatcher;
    UdpNotify m_udpNotify;

    // Cached snapshots
    std::vector<JobInfo> m_cachedJobs;
    std::vector<JobTemplate> m_cachedTemplates;

    // Thread-safe cached JSON for HTTP handlers
    mutable std::mutex m_cachedJsonMutex;
    std::string m_cachedJobsJson = "[]";

    // Farm state
    std::filesystem::path m_farmPath;
    std::string m_farmError;
    std::string m_farmSecret;
    bool m_farmRunning = false;
    NodeState m_nodeState = NodeState::Active;

    // Last (render, node) state published via publishNodeState(); written
    // only on change so the registry key is quiet between transitions.
    bool m_nodeStatePublished = false;
    bool m_lastPublishedRendering = false;
    bool m_lastPublishedStopped = false;

    // Leader tracking
    bool m_wasLeader = false;
    std::atomic<bool> m_leaderDbReady{false};
    std::thread m_leaderThread;

    // Leader contact cooldown — after any failed HTTP to leader, skip for 5s
    std::chrono::steady_clock::time_point m_leaderContactCooldown;

    // Mirror of the leader's suspension verdicts (see suspendedNodes()).
    // Guarded by its own mutex: written on the main thread by
    // refreshSuspendedNodes(), read from the UI thread via AppBridge.
    mutable std::mutex m_suspendedMutex;
    std::unordered_map<std::string, SuspensionInfo> m_suspendedNodes;
    std::chrono::steady_clock::time_point m_lastSuspendedRefresh;
    static constexpr int SUSPENDED_REFRESH_MS = 3000;

    // Pending completion reports (worker -> leader, buffered on failure)
    struct PendingReport
    {
        std::string jobId;
        int frameStart = 0, frameEnd = 0;
        std::string state;
        int64_t elapsedMs = 0;
        int exitCode = 0;
        std::string error;
        int64_t editEpoch = -1;   // echoed to the leader's epoch fence
    };
    std::vector<PendingReport> m_pendingReports;

    // Pending frame completion reports (worker -> leader)
    struct PendingFrameReport
    {
        std::string jobId;
        int frame = 0;
        int64_t editEpoch = -1;
    };
    std::vector<PendingFrameReport> m_pendingFrameReports;

    // Thread-safe access to pending reports (worker thread flushes them)
    std::mutex m_reportMutex;

    // Async HTTP queue for one-off requests to leader
    struct HttpRequest {
        std::string host;
        int port = 0;
        std::string method = "POST";
        std::string endpoint;
        std::string body;
        std::function<void(bool success, const std::string& response)> callback;
        // Status-aware variant: status 0 = no response (connect/read
        // failure), otherwise the HTTP status. Lets callers distinguish
        // "leader unreachable" from "leader answered an error" — the
        // bool callback above conflates the two.
        std::function<void(int status, const std::string& response)> statusCallback;
    };
    std::mutex m_httpQueueMutex;
    std::queue<HttpRequest> m_httpQueue;
    std::thread m_httpWorkerThread;
    std::atomic<bool> m_httpWorkerRunning{false};

    // UDP heartbeat timing
    std::chrono::steady_clock::time_point m_lastUdpHeartbeat;

    // Job cache refresh timing
    std::chrono::steady_clock::time_point m_lastJobCacheRefresh;
    // Set from the HTTP worker thread after a successful move/priority
    // post so the next update() tick refreshes the jobs cache early.
    std::atomic<bool> m_forceJobsRefresh{false};

    // Job selection
    std::string m_selectedJobId;
    bool m_requestSubmission = false;

    // Deferred farm restart (set by HTTP handler, processed by update())
    mutable std::mutex m_advertisedIpMutex;
    mutable std::string m_advertisedIpCache;                         // guarded by m_advertisedIpMutex
    mutable std::chrono::steady_clock::time_point m_advertisedIpCachedAt;  // guarded by m_advertisedIpMutex
    std::atomic<bool> m_farmRestartRequested{false};

    // Exit state
    bool m_exitRequested = false;
    bool m_shouldExit = false;

    // Deferred restart — gives sidecar time to start before we exit
    bool m_restartPending = false;
    std::chrono::steady_clock::time_point m_restartLaunchedAt;
};

} // namespace MR
