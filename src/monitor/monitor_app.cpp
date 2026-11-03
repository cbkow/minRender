#include "monitor/monitor_app.h"
#include "monitor/farm_init.h"
#include "core/platform.h"
#include "core/monitor_log.h"
#include "core/net_utils.h"
#include "core/sha256.h"

#include <nlohmann/json.hpp>
#include <httplib.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <set>
#include <map>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace MR {

bool MonitorApp::init()
{
    // Load node identity
    m_appDataDir = getAppDataDir();
    m_identity.loadOrGenerate(m_appDataDir);
    m_identity.querySystemInfo();

    // Load config
    m_configPath = m_appDataDir / "config.json";
    loadConfig();

    // Cleanup leftover restart.bat from older versions
    {
        std::error_code ec;
        std::filesystem::remove(m_appDataDir / "restart.bat", ec);
    }

    // Restore persisted node state
    if (m_config.node_stopped)
        setNodeState(NodeState::Stopped);

    // Initialize HTTP server (routes set up before farm starts)
    m_httpServer.init(this);

    // Initialize agent supervisor
    m_agentSupervisor.start(m_identity.nodeId());

    // Set up agent message handler
    m_agentSupervisor.setMessageHandler([this](const std::string& type, const nlohmann::json& msg)
    {
        m_renderCoordinator.handleAgentMessage(type, msg);
    });

    // Start background HTTP worker
    startHttpWorker();

    // (The UI IPC server and its log-push callback were used by the old
    // Tauri UI scaffold. Removed in Phase 7 — the Qt LogModel consumes
    // MonitorLog directly via AppBridge.)

    // (Qt UI layer initializes separately in main_qt.cpp via AppBridge;
    // the backend here is UI-agnostic.)

    // Auto-start farm if sync_root is configured
    if (!m_config.sync_root.empty() &&
        std::filesystem::is_directory(m_config.sync_root))
    {
        startFarm();
    }

    return true;
}

void MonitorApp::update()
{
    // Poll UDP messages (fast path — every frame)
    if (m_udpNotify.isRunning())
    {
        handleUdpMessages();

        // Send UDP heartbeat every ~3s
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastUdpHeartbeat).count();
        if (elapsed >= 3000)
        {
            sendUdpHeartbeat();
            m_lastUdpHeartbeat = now;
        }
    }

    // Process agent messages
    m_agentSupervisor.processMessages();

    // Render coordinator update
    m_renderCoordinator.update(m_agentSupervisor);

    // Leader transition detection
    if (isLeader() && !m_wasLeader)
        onBecomeLeader();
    if (!isLeader() && m_wasLeader)
        onLoseLeadership();
    m_wasLeader = isLeader();

    // If leader: run dispatch cycle (gated on background DB init)
    if (isLeader() && m_leaderDbReady.load() && m_databaseManager.isOpen())
        m_dispatchManager.update();

    // Refresh cached jobs + templates periodically
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastJobCacheRefresh).count();
    if (m_forceJobsRefresh.exchange(false) || elapsed >= 2000)
    {
        refreshCachedJobs();
        m_cachedTemplates = m_templateManager.getTemplateSnapshot();
        m_lastJobCacheRefresh = now;
    }

    // Mirror the leader's suspension verdicts (self-throttled to 3s) so
    // this node's UI can show who is benched — including itself.
    refreshSuspendedNodes();

    // Poll local submission dropbox
    m_submissionWatcher.poll();

    // Poll the farm-share command dropbox (leader only) — edits queued
    // by workers that couldn't reach us over HTTP.
    pollFarmCommands();

    // Note: completion/frame reports are flushed by the background HTTP worker thread.

    // Update render state on PeerManager
    if (m_renderCoordinator.isRendering())
    {
        m_peerManager.setRenderState("rendering",
            m_renderCoordinator.currentJobId(),
            m_renderCoordinator.currentChunkLabel());
    }
    else
    {
        m_peerManager.setRenderState("idle");
    }

    // Publish the same two facts for same-machine consumers (registry key
    // on Windows, no-op elsewhere). The first tick always writes, which
    // overwrites whatever a crashed previous instance left behind.
    {
        const bool rendering = m_renderCoordinator.isRendering();
        const bool stopped   = (m_nodeState == NodeState::Stopped);
        if (!m_nodeStatePublished
            || rendering != m_lastPublishedRendering
            || stopped   != m_lastPublishedStopped)
        {
            publishNodeState(rendering ? "rendering" : "idle",
                             stopped   ? "stopped"   : "active",
                             /*alive=*/true);
            m_nodeStatePublished      = true;
            m_lastPublishedRendering  = rendering;
            m_lastPublishedStopped    = stopped;
        }
    }

    // Deferred farm restart (triggered by POST /api/config)
    if (m_farmRestartRequested.exchange(false))
    {
        MonitorLog::instance().info("config", "Restarting farm (deferred from config update)");
        if (m_farmRunning) stopFarm();
        if (!m_config.sync_root.empty()) startFarm();
    }

    // Check for filesystem restart signal
    if (m_peerManager.consumeRestartSignal())
    {
        MonitorLog::instance().info("farm", "Restart signal detected via filesystem");
        launchRestartSidecar();
    }

    // Deferred restart — wait for sidecar to start before shutting down
    if (m_restartPending)
    {
        auto restartElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_restartLaunchedAt).count();
        if (restartElapsed >= 3000)
        {
            m_restartPending = false;
            beginForceExit();
        }
    }

    // Process exit
    if (m_exitRequested && !m_shouldExit)
    {
        if (!m_renderCoordinator.isRendering())
        {
            m_shouldExit = true;
        }
    }
}


void MonitorApp::shutdown()
{
    MonitorLog::instance().setCallback(nullptr);
    stopHttpWorker();
    stopFarm();

    m_renderCoordinator.abortCurrentRender("shutdown");
    m_agentSupervisor.stop();

    // Clean-exit marker for out-of-process consumers: idle, Pid = 0.
    publishNodeState("idle",
                     (m_nodeState == NodeState::Stopped) ? "stopped" : "active",
                     /*alive=*/false);

    saveConfig();
}

// ---------------------------------------------------------------------------
// UI IPC push events
// ---------------------------------------------------------------------------

void MonitorApp::loadConfig()
{
    std::error_code ec;
    if (!std::filesystem::exists(m_configPath, ec))
    {
        std::cout << "[MonitorApp] No config found, using defaults" << std::endl;
        return;
    }

    try
    {
        std::ifstream ifs(m_configPath);
        nlohmann::json j = nlohmann::json::parse(ifs);
        m_config = j.get<Config>();
        std::cout << "[MonitorApp] Config loaded" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MonitorApp] Failed to load config: " << e.what() << std::endl;
    }
}

void MonitorApp::saveConfig()
{
    try
    {
        nlohmann::json j = m_config;
        std::ofstream ofs(m_configPath);
        ofs << j.dump(2);
        std::cout << "[MonitorApp] Config saved" << std::endl;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[MonitorApp] Failed to save config: " << e.what() << std::endl;
    }
}

bool MonitorApp::startFarm()
{
    if (m_farmRunning)
        return true;

    m_farmPath = std::filesystem::path(m_config.sync_root) /
        ("MinRender-v" + std::to_string(PROTOCOL_VERSION));
    m_farmError.clear();

    // Check if sync root exists
    std::error_code ec;
    if (!std::filesystem::is_directory(m_config.sync_root, ec))
    {
        m_farmError = "Sync root does not exist: " + m_config.sync_root;
        return false;
    }

    // Create farm directory structure if needed
    std::filesystem::create_directories(m_farmPath / "templates" / "examples", ec);
    std::filesystem::create_directories(m_farmPath / "jobs", ec);
    std::filesystem::create_directories(m_farmPath / "nodes", ec);
    std::filesystem::create_directories(m_farmPath / "state", ec);

    // Start file logging before farm init, so its copy results reach this
    // node's log on the farm rather than only the in-app view.
    MonitorLog::instance().startFileLogging(m_farmPath, m_identity.nodeId());

    // Farm init — copy example templates + plugins on first run or when this app is newer
    auto initResult = FarmInit::init(m_farmPath, m_identity.nodeId());
    if (!initResult.success)
        MonitorLog::instance().warn("farm", "Farm init: " + initResult.error);

    // Load API secret from farm.json
    {
        auto farmJsonPath = m_farmPath / "farm.json";
        try
        {
            std::ifstream ifs(farmJsonPath);
            nlohmann::json fj = nlohmann::json::parse(ifs);
            if (fj.contains("api_secret") && fj["api_secret"].is_string())
                m_farmSecret = fj["api_secret"].get<std::string>();
        }
        catch (...) {}
    }
    m_httpServer.setApiSecret(m_farmSecret);

    MonitorLog::instance().info("farm", "Farm started at " + m_farmPath.string());

    // Apply staging setting
    m_renderCoordinator.setStagingEnabled(m_config.staging_enabled);

    // Initialize render coordinator with completion callbacks
    m_renderCoordinator.init(m_farmPath, m_identity.nodeId(), getOS(),
        [this](const std::string& jobId, const ChunkRange& chunk,
               const std::string& state, int64_t editEpoch)
        {
            reportCompletion(jobId, chunk, state, editEpoch);
        },
        [this](const std::string& jobId, int frame, int64_t editEpoch)
        {
            reportFrameCompletion(jobId, frame, editEpoch);
        },
        &m_agentSupervisor);

    // Initialize dispatch manager (will be active only when leader)
    m_dispatchManager.init(this, &m_databaseManager);

    // Start HTTP server
    std::string localIp = advertisedIp();
    uint16_t httpPort = m_config.http_port;

    if (!m_httpServer.start("0.0.0.0", httpPort))
    {
        m_farmError = "Failed to bind HTTP server on port " + std::to_string(httpPort);
        MonitorLog::instance().error("farm", m_farmError);
    }

    // Start peer manager
    std::string localEndpoint = localIp + ":" + std::to_string(httpPort);
    m_peerManager.start(m_farmPath, m_identity.nodeId(), localEndpoint,
                        m_config.tags);
    MonitorLog::instance().info("farm", "Local endpoint: " + localEndpoint);

    // Start UDP multicast (after PeerManager)
    if (m_config.udp_enabled)
    {
        if (m_udpNotify.start(m_identity.nodeId(), m_config.udp_port))
        {
            MonitorLog::instance().info("farm",
                "UDP multicast active on port " + std::to_string(m_config.udp_port));
            // Send immediate announcement
            sendUdpHeartbeat();
            m_lastUdpHeartbeat = std::chrono::steady_clock::now();
        }
        else
        {
            MonitorLog::instance().warn("farm",
                "UDP multicast failed to start — running HTTP-only");
        }
    }

    // Start template manager
    m_templateManager.start(m_farmPath);

    // Initialize local submission watcher
    m_submissionWatcher.init(this, m_appDataDir);

    m_farmRunning = true;
    m_wasLeader = false;

    return true;
}

std::string MonitorApp::advertisedIp() const
{
    if (!m_config.ip_override.empty())
        return m_config.ip_override;

    // Called per status request and heartbeat; the route lookup may hit
    // DNS when sync_root names the server, so cache it for 30 s.
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_advertisedIpMutex);
    if (!m_advertisedIpCache.empty() && now - m_advertisedIpCachedAt < std::chrono::seconds(30))
        return m_advertisedIpCache;

    // Peers reach us over whatever network also carries the farm share,
    // so the source address the OS picks for the share's server is the
    // one to advertise: the LAN port in the office, the VPN tunnel at
    // home. Enumeration order (getLocalIpAddress) would hand a Tailscale
    // or WireGuard adapter to the office nodes.
    std::string ip;
    std::string how;
    const std::string host = shareHostForPath(m_config.sync_root);
    if (!host.empty())
    {
        ip = getLocalIpAddressTowards(host);
        how = "route to share host " + host;
    }
    if (ip.empty())
    {
        ip = getLocalIpAddress();
        how = host.empty() ? "first adapter (sync root is a local path)"
                           : "first adapter (no route to share host " + host + ")";
    }

    if (ip != m_advertisedIpCache)
        MonitorLog::instance().info("farm", "Advertising " + ip + " (" + how + ")");
    m_advertisedIpCache = ip;
    m_advertisedIpCachedAt = now;
    return ip;
}

void MonitorApp::stopFarm()
{
    if (!m_farmRunning)
        return;

    // Wait for any in-progress leader DB init
    if (m_leaderThread.joinable())
        m_leaderThread.join();
    m_leaderDbReady.store(false);

    // Send UDP goodbye before stopping
    if (m_udpNotify.isRunning())
    {
        nlohmann::json bye = {
            {"t", "bye"},
            {"from", m_identity.nodeId()},
            {"n", m_identity.nodeId()},
        };
        m_udpNotify.send(bye);
        m_udpNotify.stop();
    }

    // Close database before stopping managers
    m_databaseManager.close();

    m_peerManager.stop();
    m_httpServer.stop();
    m_templateManager.stop();
    MonitorLog::instance().info("farm", "Farm stopped");
    MonitorLog::instance().stopFileLogging();

    m_farmRunning = false;
    m_wasLeader = false;
}

// --- Leader transitions ---

void MonitorApp::onBecomeLeader()
{
    MonitorLog::instance().info("farm", "This node is now leader — initializing DB...");
    m_leaderDbReady.store(false);

    // Join any previous transition thread
    if (m_leaderThread.joinable())
        m_leaderThread.join();

    // DB restore/open on background thread to avoid blocking UI
    auto snapshotPath = m_farmPath / "state" / "snapshot.db";
    auto localDbPath = m_appDataDir / "minrender.db";

    m_leaderThread = std::thread([this, snapshotPath, localDbPath]()
    {
        std::error_code ec;
        if (std::filesystem::exists(snapshotPath, ec))
        {
            MonitorLog::instance().info("farm", "Restoring DB from snapshot...");
            if (m_databaseManager.restoreFrom(snapshotPath, localDbPath))
            {
                MonitorLog::instance().info("farm", "DB restored from snapshot");
                m_leaderDbReady.store(true);
                return;
            }
            MonitorLog::instance().warn("farm", "Snapshot restore failed, opening fresh DB");
        }

        if (m_databaseManager.open(localDbPath))
            m_leaderDbReady.store(true);
        else
            MonitorLog::instance().error("farm", "Failed to open database!");
    });
}

void MonitorApp::onLoseLeadership()
{
    MonitorLog::instance().info("farm", "No longer leader");

    // Wait for any in-progress DB init to finish before closing
    if (m_leaderThread.joinable())
        m_leaderThread.join();

    m_leaderDbReady.store(false);
    m_databaseManager.close();
}

// --- Completion reporting ---

void MonitorApp::reportCompletion(const std::string& jobId, const ChunkRange& chunk,
                                   const std::string& state, int64_t editEpoch)
{
    MonitorLog::instance().info("render",
        "Chunk " + state + ": " + jobId + " " + chunk.rangeStr());

    if (state == "abandoned")
        return; // no report needed

    if (isLeader() && m_databaseManager.isOpen())
    {
        // Direct DB update
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (state == "completed")
        {
            m_databaseManager.completeChunk(jobId, chunk.frame_start, chunk.frame_end,
                                            nowMs, m_identity.nodeId(), editEpoch);
        }
        else if (state == "failed")
        {
            auto jobOpt = m_databaseManager.getJob(jobId);
            int maxRetries = 3;
            if (jobOpt.has_value())
            {
                try
                {
                    auto manifest = nlohmann::json::parse(jobOpt->manifest_json).get<JobManifest>();
                    maxRetries = manifest.max_retries;
                }
                catch (...) {}
            }
            m_databaseManager.failChunk(jobId, chunk.frame_start, chunk.frame_end,
                maxRetries, m_identity.nodeId(), editEpoch);
        }
    }
    else
    {
        // Buffer for HTTP delivery to leader (flushed by worker thread)
        PendingReport report;
        report.jobId = jobId;
        report.frameStart = chunk.frame_start;
        report.frameEnd = chunk.frame_end;
        report.state = state;
        report.editEpoch = editEpoch;
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingReports.push_back(std::move(report));
    }
}

// --- Frame completion reporting ---

void MonitorApp::reportFrameCompletion(const std::string& jobId, int frame,
                                       int64_t editEpoch)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        // Direct DB write — no queue, no 2s dispatch throttle.
        // We're already on the main thread (agent message handler).
        if (m_databaseManager.isJobEpochCurrent(jobId, editEpoch))
            m_databaseManager.addCompletedFrames(jobId, frame);
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingFrameReports.push_back({jobId, frame, editEpoch});
    }
}

// --- Chunk data access (leader: DB, worker: HTTP) ---

std::vector<ChunkRow> MonitorApp::getChunksForJob(const std::string& jobId)
{
    if (isLeader() && m_leaderDbReady.load() && m_databaseManager.isOpen())
    {
        return m_databaseManager.getChunksForJob(jobId);
    }

    // Worker: fetch from leader via HTTP (skip if cooldown active)
    if (std::chrono::steady_clock::now() < m_leaderContactCooldown)
        return {};

    std::string leaderEp = getLeaderEndpoint();
    if (leaderEp.empty())
        return {};

    auto [host, port] = parseEndpoint(leaderEp);
    if (host.empty())
        return {};

    try
    {
        httplib::Client cli(host, port);
        cli.set_connection_timeout(0, 500000); // 500ms
        cli.set_read_timeout(1);

        auto res = cli.Get("/api/jobs/" + jobId, authHeaders(m_farmSecret));
        if (!res || res->status != 200)
        {
            if (!res)
                m_leaderContactCooldown = std::chrono::steady_clock::now() +
                                         std::chrono::seconds(5);
            return {};
        }

        auto body = nlohmann::json::parse(res->body);
        if (!body.contains("chunks") || !body["chunks"].is_array())
            return {};

        std::vector<ChunkRow> result;
        for (const auto& cj : body["chunks"])
        {
            ChunkRow row;
            row.id = cj.value("id", int64_t(0));
            row.frame_start = cj.value("frame_start", 0);
            row.frame_end = cj.value("frame_end", 0);
            row.state = cj.value("state", "pending");
            row.assigned_to = cj.value("assigned_to", "");
            row.assigned_at_ms = cj.value("assigned_at_ms", int64_t(0));
            row.completed_at_ms = cj.value("completed_at_ms", int64_t(0));
            row.retry_count = cj.value("retry_count", 0);
            if (cj.contains("completed_frames") && cj["completed_frames"].is_array())
                row.completed_frames = cj["completed_frames"].get<std::vector<int>>();
            if (cj.contains("failed_on") && cj["failed_on"].is_array())
                row.failed_on = cj["failed_on"].get<std::vector<std::string>>();
            result.push_back(std::move(row));
        }
        return result;
    }
    catch (...)
    {
        m_leaderContactCooldown = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
    }
    return {};
}

// --- Cached job data ---

void MonitorApp::refreshCachedJobs()
{
    if (isLeader() && m_leaderDbReady.load() && m_databaseManager.isOpen())
    {
        // Build from DB
        auto summaries = m_databaseManager.getAllJobs();

        std::vector<JobInfo> jobs;
        nlohmann::json jsonArr = nlohmann::json::array();

        for (auto& s : summaries)
        {
            // Skip archived jobs — they're hidden from the main job list
            if (s.job.current_state == "archived")
                continue;

            JobInfo info;
            try
            {
                info.manifest = nlohmann::json::parse(s.job.manifest_json).get<JobManifest>();
            }
            catch (...) { continue; }

            info.current_state = s.job.current_state;
            info.current_priority = s.job.priority;
            info.total_chunks = s.progress.total;
            info.completed_chunks = s.progress.completed;
            info.failed_chunks = s.progress.failed;
            info.rendering_chunks = s.progress.rendering;
            info.stopped_chunks = s.progress.stopped;
            info.first_assigned_ms = s.progress.first_assigned_ms;
            info.last_completed_ms = s.progress.last_completed_ms;

            // Build JSON for HTTP cache BEFORE moving info
            nlohmann::json jj = {
                {"job_id", s.job.job_id},
                {"template_id", info.manifest.template_id},
                {"current_state", s.job.current_state},
                {"priority", s.job.priority},
                {"submitted_at_ms", s.job.submitted_at_ms},
                {"submitted_by", info.manifest.submitted_by},
                {"frame_start", info.manifest.frame_start},
                {"frame_end", info.manifest.frame_end},
                {"chunk_size", info.manifest.chunk_size},
                {"total_chunks", s.progress.total},
                {"completed_chunks", s.progress.completed},
                {"failed_chunks", s.progress.failed},
                {"rendering_chunks", s.progress.rendering},
                {"pending_chunks", s.progress.pending},
                {"stopped_chunks", s.progress.stopped},
                {"first_assigned_ms", s.progress.first_assigned_ms},
                {"last_completed_ms", s.progress.last_completed_ms},
            };
            if (info.manifest.output_dir.has_value())
                jj["output_dir"] = info.manifest.output_dir.value();
            // Output filename pattern for worker-side frame previews —
            // workers only see this summary, not the manifest flags.
            for (const auto& f : info.manifest.flags)
            {
                if (f.is_output && f.value.has_value() && !f.value->empty())
                {
                    jj["output_stem"] = *f.value;
                    break;
                }
            }
            jsonArr.push_back(std::move(jj));

            jobs.push_back(std::move(info));
        }

        m_cachedJobs = std::move(jobs);

        {
            std::lock_guard<std::mutex> lock(m_cachedJsonMutex);
            m_cachedJobsJson = jsonArr.dump();
        }

        abortRenderIfJobGone();
    }
    else if (m_farmRunning)
    {
        // Worker: fetch from leader via HTTP (skip if cooldown active)
        if (std::chrono::steady_clock::now() >= m_leaderContactCooldown)
        {
            std::string leaderEp = getLeaderEndpoint();
            if (!leaderEp.empty())
            {
                auto [host, port] = parseEndpoint(leaderEp);
                if (!host.empty())
                {
                    try
                    {
                        httplib::Client cli(host, port);
                        cli.set_connection_timeout(0, 500000); // 500ms
                        cli.set_read_timeout(1);

                        auto res = cli.Get("/api/jobs", authHeaders(m_farmSecret));
                        if (res && res->status == 200)
                        {
                            auto arr = nlohmann::json::parse(res->body);
                            std::vector<JobInfo> jobs;

                            for (const auto& jj : arr)
                            {
                                JobInfo info;
                                info.manifest.job_id = jj.value("job_id", "");
                                info.manifest.template_id = jj.value("template_id", "");
                                info.manifest.submitted_by = jj.value("submitted_by", "");
                                info.manifest.submitted_at_ms = jj.value("submitted_at_ms", int64_t(0));
                                info.manifest.frame_start = jj.value("frame_start", 1);
                                info.manifest.frame_end = jj.value("frame_end", 250);
                                info.manifest.chunk_size = jj.value("chunk_size", 1);
                                info.current_state = jj.value("current_state", "active");
                                info.current_priority = jj.value("priority", 50);
                                info.total_chunks = jj.value("total_chunks", 0);
                                info.completed_chunks = jj.value("completed_chunks", 0);
                                info.failed_chunks = jj.value("failed_chunks", 0);
                                info.rendering_chunks = jj.value("rendering_chunks", 0);
                                info.stopped_chunks = jj.value("stopped_chunks", 0);
                                info.first_assigned_ms = jj.value("first_assigned_ms", int64_t(0));
                                info.last_completed_ms = jj.value("last_completed_ms", int64_t(0));
                                if (jj.contains("output_dir") && jj["output_dir"].is_string())
                                    info.manifest.output_dir = jj["output_dir"].get<std::string>();
                                // Rebuild the output flag so frame-preview
                                // pattern matching works on workers (older
                                // leaders don't send this; the filename
                                // heuristic covers those).
                                if (jj.contains("output_stem") && jj["output_stem"].is_string())
                                {
                                    ManifestFlag f;
                                    f.is_output = true;
                                    f.value = jj["output_stem"].get<std::string>();
                                    info.manifest.flags.push_back(std::move(f));
                                }
                                jobs.push_back(std::move(info));
                            }

                            m_cachedJobs = std::move(jobs);

                            {
                                std::lock_guard<std::mutex> lock(m_cachedJsonMutex);
                                m_cachedJobsJson = res->body;
                            }

                            abortRenderIfJobGone();
                        }
                        else
                        {
                            m_leaderContactCooldown = std::chrono::steady_clock::now() +
                                                     std::chrono::seconds(5);
                        }
                    }
                    catch (...)
                    {
                        m_leaderContactCooldown = std::chrono::steady_clock::now() +
                                                 std::chrono::seconds(5);
                    }
                }
            }
        }
    }
}

// Called after every successful jobs-cache refresh (leader DB read or
// worker HTTP fetch). If the job we're currently rendering was cancelled
// or deleted on another node, kill the local render immediately — this is
// what propagates an aggressive cancel across the farm (≤ one refresh
// interval of latency). Failed refreshes never reach here, so a flaky
// leader can't take down a healthy render.
void MonitorApp::abortRenderIfJobGone()
{
    if (!m_renderCoordinator.isRendering())
        return;

    const std::string jobId = m_renderCoordinator.currentJobId();
    if (jobId.empty())
        return;

    const JobInfo* found = nullptr;
    for (const auto& info : m_cachedJobs)
    {
        if (info.manifest.job_id == jobId)
        {
            found = &info;
            break;
        }
    }

    if (found && found->current_state != "cancelled")
        return;

    const std::string reason = found ? "job cancelled" : "job removed";
    MonitorLog::instance().info("render",
        "Aborting local render, " + reason + " elsewhere: " + jobId);
    m_renderCoordinator.abortCurrentRender(reason);
    m_renderCoordinator.purgeJob(jobId);
}

std::string MonitorApp::getCachedJobsJson() const
{
    std::lock_guard<std::mutex> lock(m_cachedJsonMutex);
    return m_cachedJobsJson;
}

std::string MonitorApp::getCachedJobDetailJson(const std::string& jobId) const
{
    if (!m_leaderDbReady.load() || !m_databaseManager.isOpen())
        return {};

    auto jobOpt = const_cast<DatabaseManager&>(m_databaseManager).getJob(jobId);
    if (!jobOpt.has_value())
        return {};

    auto chunks = const_cast<DatabaseManager&>(m_databaseManager).getChunksForJob(jobId);

    nlohmann::json result = {
        {"job_id", jobOpt->job_id},
        {"current_state", jobOpt->current_state},
        {"priority", jobOpt->priority},
        {"submitted_at_ms", jobOpt->submitted_at_ms},
        {"manifest", nlohmann::json::parse(jobOpt->manifest_json)},
    };

    nlohmann::json chunkArr = nlohmann::json::array();
    for (const auto& c : chunks)
    {
        chunkArr.push_back({
            {"id", c.id},
            {"frame_start", c.frame_start},
            {"frame_end", c.frame_end},
            {"state", c.state},
            {"assigned_to", c.assigned_to},
            {"assigned_at_ms", c.assigned_at_ms},
            {"completed_at_ms", c.completed_at_ms},
            {"retry_count", c.retry_count},
            {"completed_frames", c.completed_frames},
            {"failed_on", c.failed_on},
        });
    }
    result["chunks"] = chunkArr;

    return result.dump();
}

// --- Job controls ---

void MonitorApp::pauseJob(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.updateJobState(jobId, "paused");
        MonitorLog::instance().info("job", "Paused job: " + jobId);
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId + "/pause", "");
    }
}

void MonitorApp::resumeJob(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.updateJobState(jobId, "active");
        MonitorLog::instance().info("job", "Resumed job: " + jobId);
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId + "/resume", "");
    }
}

bool MonitorApp::moveJob(const std::string& jobId, const std::string& targetJobId, bool before)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        if (!m_databaseManager.moveJob(jobId, targetJobId, before))
        {
            MonitorLog::instance().warn("job",
                "Move rejected (missing job or target): " + jobId);
            return false;
        }
        MonitorLog::instance().info("job", "Moved job " + jobId
            + (before ? " before " : " after ") + targetJobId);
        // Refresh now so the UI shows the new order on the next tick
        // instead of after the 2s cadence.
        refreshCachedJobs();
        return true;
    }

    nlohmann::json body = {
        {"target", targetJobId},
        {"position", before ? "before" : "after"},
    };
    postToLeaderAsync("/api/jobs/" + jobId + "/move", body.dump(),
        [this, jobId](bool ok, const std::string& resp) {
            if (ok)
            {
                // Pull the new order promptly rather than waiting out
                // the 2s refresh interval. (Atomic — we're on the HTTP
                // worker thread here.)
                m_forceJobsRefresh.store(true);
            }
            else
            {
                std::string msg = "Move failed for " + jobId;
                if (!resp.empty()) msg += " — leader responded: " + resp;
                else               msg += " — no response (leader unreachable or missing endpoint)";
                MonitorLog::instance().warn("job", msg);
            }
        });
    return true;
}

void MonitorApp::setJobPriority(const std::string& jobId, int priority)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.updateJobPriority(jobId, priority);
        MonitorLog::instance().info("job",
            "Set priority " + std::to_string(priority) + " on job: " + jobId);
        refreshCachedJobs();
    }
    else
    {
        nlohmann::json body = {{"priority", priority}};
        postToLeaderAsync("/api/jobs/" + jobId + "/priority", body.dump(),
            [this](bool ok, const std::string&) {
                if (ok)
                    m_forceJobsRefresh.store(true);
            });
    }
}

void MonitorApp::cancelJob(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.updateJobState(jobId, "cancelled");
        MonitorLog::instance().info("job", "Cancelled job: " + jobId);
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId + "/cancel", "");
    }

    // Abort local render if it's for this job
    if (m_renderCoordinator.isRendering() && m_renderCoordinator.currentJobId() == jobId)
    {
        m_renderCoordinator.abortCurrentRender("job cancelled");
    }
    m_renderCoordinator.purgeJob(jobId);
}

void MonitorApp::requeueJob(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.resetAllChunks(jobId);
        m_databaseManager.updateJobState(jobId, "active");
        MonitorLog::instance().info("job", "Requeued job: " + jobId);
    }
    else
    {
        // Log the leader's reply so a 404 (leader on old build without
        // this endpoint) or auth failure shows up locally instead of
        // silently looking like "button does nothing".
        postToLeaderAsync("/api/jobs/" + jobId + "/requeue", "",
            [jobId](bool ok, const std::string& resp) {
                if (ok)
                    MonitorLog::instance().info("job", "Requeued job: " + jobId);
                else
                {
                    std::string msg = "Requeue failed for " + jobId;
                    if (!resp.empty()) msg += " — leader responded: " + resp;
                    else               msg += " — no response (leader unreachable or missing endpoint)";
                    MonitorLog::instance().warn("job", msg);
                }
            });
    }
}

void MonitorApp::deleteJob(const std::string& jobId)
{
    // The id becomes jobs/<id> below and is removed recursively. Job ids
    // are slugs ([a-z0-9_-]), so anything else is a traversal attempt or
    // a bug; refuse rather than let ".." resolve to the farm root.
    if (!isSafePathSegment(jobId))
    {
        MonitorLog::instance().warn("job", "deleteJob: refusing unsafe job id: " + jobId);
        return;
    }

    // Cancel first (abort any active render)
    cancelJob(jobId);

    // Tell the render coordinator to drop any post-cancel stdout for this
    // job. Without this, flushStdout() recreates the subtree we are about
    // to remove and remove_all fails with ENOTEMPTY on macOS.
    m_renderCoordinator.markJobDeleted(jobId);

    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.deleteJob(jobId);
        MonitorLog::instance().info("job", "Deleted job: " + jobId);
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId, "", nullptr, "DELETE");
    }

    // Clean up shared FS job directory
    if (m_farmRunning)
    {
        auto jobDir = m_farmPath / "jobs" / jobId;
        std::error_code ec;
        std::filesystem::remove_all(jobDir, ec);

        // One retry on transient errors — covers macOS sidecar files
        // (.DS_Store, ._*) and Spotlight metadata that briefly re-occupy
        // the directory during teardown.
        if (ec)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ec.clear();
            std::filesystem::remove_all(jobDir, ec);
        }

        if (ec)
            MonitorLog::instance().warn("job",
                "Failed to remove job dir " + jobDir.string() + ": " + ec.message());
    }

    if (m_selectedJobId == jobId)
        m_selectedJobId.clear();
}

void MonitorApp::archiveJob(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.updateJobState(jobId, "archived");
        MonitorLog::instance().info("job", "Archived job: " + jobId);
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId + "/archive", "");
    }

    if (m_selectedJobId == jobId)
        m_selectedJobId.clear();
}

void MonitorApp::retryFailedChunks(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_dispatchManager.retryFailedChunks(jobId);
        MonitorLog::instance().info("job", "Retrying failed chunks: " + jobId);
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId + "/retry-failed", "");
    }
}

void MonitorApp::reassignChunk(int64_t chunkId, const std::string& targetNodeId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        m_databaseManager.reassignChunk(chunkId, targetNodeId);
        MonitorLog::instance().info("job", "Reassigned chunk " + std::to_string(chunkId) +
            (targetNodeId.empty() ? " to pending" : " to " + targetNodeId));
    }
    else
    {
        nlohmann::json body = {{"chunk_id", chunkId}};
        if (!targetNodeId.empty())
            body["target_node"] = targetNodeId;
        postToLeaderAsync("/api/chunks/reassign", body.dump());
    }
}

namespace {
// Look up a peer's endpoint by nodeId without PeerManager gaining a
// dedicated accessor (the snapshot is cheap — it's a copy of a map).
std::string peerEndpointFor(const std::vector<PeerInfo>& peers,
                            const std::string& nodeId)
{
    for (const auto& p : peers)
        if (p.node_id == nodeId) return p.endpoint;
    return {};
}

// Log HTTP round-trip result for peer remote-control actions so a
// silent failure (peer unreachable, 404, auth rejection) shows up in
// the local log instead of making the UI look buggy.
void logPeerResult(const std::string& action, const std::string& nodeId,
                   bool success, const std::string& response)
{
    if (success)
    {
        MonitorLog::instance().info("peer",
            action + " ok: " + nodeId);
    }
    else
    {
        std::string msg = action + " failed: " + nodeId;
        if (!response.empty()) msg += " (" + response + ")";
        MonitorLog::instance().warn("peer", msg);
    }
}
} // namespace

void MonitorApp::stopChunk(int64_t chunkId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        const auto row = m_databaseManager.stopChunk(chunkId);
        if (!row.has_value())
            return;   // already terminal / unknown — nothing to do

        MonitorLog::instance().info("job",
            "Stopped chunk " + std::to_string(chunkId) + " (" + row->job_id +
            " f" + std::to_string(row->frame_start) +
            "-" + std::to_string(row->frame_end) + ")");

        // Kill the in-flight render, if any — same abort push the job
        // edits use (abandon: no failure bookkeeping, no blacklist). A
        // node that misses the push finishes a wasted render whose
        // completion bounces off the state='assigned' guard.
        if (!row->assigned_to.empty())
        {
            if (row->assigned_to == m_identity.nodeId())
            {
                abandonJobRender(row->job_id, "chunk stopped");
            }
            else
            {
                const std::string ep =
                    peerEndpointFor(m_peerManager.getPeerSnapshot(), row->assigned_to);
                auto [host, port] = ep.empty()
                    ? std::pair<std::string, int>{std::string{}, 0}
                    : parseEndpoint(ep);
                if (!host.empty())
                {
                    HttpRequest req;
                    req.host     = host;
                    req.port     = port;
                    req.method   = "POST";
                    req.endpoint = "/api/render/abort";
                    req.body     = nlohmann::json{{"job_id", row->job_id},
                                                  {"reason", "chunk stopped"}}.dump();
                    const std::string nodeId = row->assigned_to;
                    req.callback = [nodeId](bool ok, const std::string& resp) {
                        logPeerResult("render abort", nodeId, ok, resp);
                    };
                    std::lock_guard<std::mutex> lock(m_httpQueueMutex);
                    m_httpQueue.push(std::move(req));
                }
                // offline — the wasted render is fenced on report
            }
        }
        refreshCachedJobs();
    }
    else
    {
        nlohmann::json body = {{"chunk_id", chunkId}};
        postToLeaderAsyncEx("/api/chunks/stop", body.dump(),
            [chunkId, this](int status, const std::string&) {
                if (status == 200)
                    m_forceJobsRefresh.store(true);
                else
                    MonitorLog::instance().warn("job",
                        "Stop chunk " + std::to_string(chunkId) +
                        " failed (status " + std::to_string(status) + ")");
            });
    }
}

std::string MonitorApp::resubmitJob(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        auto newId = m_dispatchManager.resubmitJob(jobId);
        if (!newId.empty())
        {
            MonitorLog::instance().info("job", "Resubmitted job: " + jobId + " -> " + newId);
            selectJob(newId);
        }
        return newId;
    }
    else
    {
        postToLeaderAsync("/api/jobs/" + jobId + "/resubmit", "");
    }
    return {};
}

std::string MonitorApp::resubmitChunkAsJob(const std::string& jobId,
                                            int frameStart, int frameEnd, int chunkSize)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        auto newId = m_dispatchManager.resubmitJob(jobId, frameStart, frameEnd, chunkSize);
        if (!newId.empty())
        {
            MonitorLog::instance().info("job", "Resubmitted chunk as job: "
                + jobId + " [" + std::to_string(frameStart) + "-"
                + std::to_string(frameEnd) + "] -> " + newId);
            selectJob(newId);
        }
        return newId;
    }
    else
    {
        nlohmann::json body = {
            {"frame_start", frameStart},
            {"frame_end", frameEnd},
            {"chunk_size", chunkSize},
        };
        MonitorLog::instance().info("job", "Sending resubmit-chunk to leader: " + jobId);
        postToLeaderAsync("/api/jobs/" + jobId + "/resubmit", body.dump(),
            [jobId](bool success, const std::string& response) {
                if (success)
                    MonitorLog::instance().info("job", "Resubmit-chunk succeeded for " + jobId
                        + ": " + response);
                else
                    MonitorLog::instance().warn("job", "Resubmit-chunk failed for " + jobId
                        + (response.empty() ? "" : ": " + response));
            });
    }
    return {};
}

std::string MonitorApp::resubmitIncomplete(const std::string& jobId)
{
    if (isLeader() && m_databaseManager.isOpen())
    {
        auto newId = m_dispatchManager.resubmitIncomplete(jobId);
        if (!newId.empty())
        {
            MonitorLog::instance().info("job",
                "Resubmitted incomplete: " + jobId + " -> " + newId);
            selectJob(newId);
        }
        return newId;
    }
    else
    {
        nlohmann::json body = {{"incomplete_only", true}};
        postToLeaderAsync("/api/jobs/" + jobId + "/resubmit", body.dump());
    }
    return {};
}

void MonitorApp::unsuspendNode(const std::string& nodeId)
{
    if (isLeader())
    {
        m_dispatchManager.failureTracker().clearNode(nodeId);
        MonitorLog::instance().info("job", "Unsuspended node: " + nodeId);
    }
    else
    {
        postToLeaderAsync("/api/nodes/" + nodeId + "/unsuspend", "");
    }
}

void MonitorApp::unsuspendThisNode()
{
    const std::string me = m_identity.nodeId();
    unsuspendNode(me);

    // Drop the mirror entry optimistically so the banner clears on the
    // next UI tick instead of after the 3s refresh. If the leader didn't
    // actually clear it (POST lost, leader unreachable), the next
    // refreshSuspendedNodes() puts it back.
    {
        std::lock_guard<std::mutex> lock(m_suspendedMutex);
        m_suspendedNodes.erase(me);
    }
}

bool MonitorApp::isThisNodeSuspended() const
{
    return isNodeSuspended(m_identity.nodeId());
}

int MonitorApp::thisNodeSuspendFailureCount() const
{
    std::lock_guard<std::mutex> lock(m_suspendedMutex);
    auto it = m_suspendedNodes.find(m_identity.nodeId());
    return it == m_suspendedNodes.end() ? 0 : it->second.failure_count;
}

std::unordered_map<std::string, MonitorApp::SuspensionInfo>
MonitorApp::suspendedNodes() const
{
    std::lock_guard<std::mutex> lock(m_suspendedMutex);
    return m_suspendedNodes;
}

bool MonitorApp::isNodeSuspended(const std::string& nodeId) const
{
    std::lock_guard<std::mutex> lock(m_suspendedMutex);
    return m_suspendedNodes.find(nodeId) != m_suspendedNodes.end();
}

void MonitorApp::refreshSuspendedNodes()
{
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastSuspendedRefresh).count() < SUSPENDED_REFRESH_MS)
        return;
    m_lastSuspendedRefresh = now;

    std::unordered_map<std::string, SuspensionInfo> next;

    if (isLeader())
    {
        // We own the tracker — read it directly, no HTTP.
        for (const auto& [nodeId, rec] : m_dispatchManager.failureTracker().getSuspended())
            next.emplace(nodeId, SuspensionInfo{rec.failure_count, rec.last_failure_ms});
    }
    else if (m_farmRunning)
    {
        // Worker: mirror the leader's verdicts. Shares
        // m_leaderContactCooldown with refreshCachedJobs so an
        // unreachable leader backs both polls off together rather than
        // stalling update() twice per cycle.
        if (now < m_leaderContactCooldown)
            return;   // keep the last known mirror rather than blanking it

        const std::string leaderEp = getLeaderEndpoint();
        if (leaderEp.empty())
            return;
        auto [host, port] = parseEndpoint(leaderEp);
        if (host.empty())
            return;

        try
        {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(0, 500000); // 500ms
            cli.set_read_timeout(1);

            auto res = cli.Get("/api/nodes/suspended", authHeaders(m_farmSecret));
            if (!res || res->status != 200)
            {
                // A non-200 is also how an older leader answers a route
                // it doesn't have; either way keep the previous mirror.
                if (!res)
                    m_leaderContactCooldown = std::chrono::steady_clock::now()
                                            + std::chrono::seconds(5);
                return;
            }

            auto arr = nlohmann::json::parse(res->body);
            if (!arr.is_array())
                return;

            for (const auto& e : arr)
            {
                const std::string id = e.value("node_id", "");
                if (id.empty())
                    continue;
                next.emplace(id, SuspensionInfo{
                    e.value("failure_count", 0),
                    e.value("last_failure_ms", int64_t(0))});
            }
        }
        catch (...)
        {
            m_leaderContactCooldown = std::chrono::steady_clock::now()
                                    + std::chrono::seconds(5);
            return;
        }
    }
    // else: farm stopped — fall through so the mirror clears.

    std::lock_guard<std::mutex> lock(m_suspendedMutex);
    m_suspendedNodes = std::move(next);
}

void MonitorApp::setPeerNodeActive(const std::string& nodeId, bool active)
{
    const std::string ep = peerEndpointFor(m_peerManager.getPeerSnapshot(), nodeId);
    if (ep.empty())
    {
        MonitorLog::instance().warn("peer",
            "setPeerNodeActive: unknown or endpointless peer " + nodeId);
        return;
    }

    auto [host, port] = parseEndpoint(ep);
    if (host.empty())
        return;

    // Optimistic local update — peer's next status poll confirms or
    // reverts this.
    m_peerManager.setPeerNodeState(nodeId, active ? "active" : "stopped");

    const std::string label = active ? "start" : "stop";
    HttpRequest req;
    req.host     = host;
    req.port     = port;
    req.method   = "POST";
    req.endpoint = active ? "/api/node/start" : "/api/node/stop";
    req.callback = [label, nodeId](bool ok, const std::string& resp) {
        logPeerResult("peer " + label, nodeId, ok, resp);
    };
    {
        std::lock_guard<std::mutex> lock(m_httpQueueMutex);
        m_httpQueue.push(std::move(req));
    }
}

void MonitorApp::restartPeerApp(const std::string& nodeId)
{
    const std::string ep = peerEndpointFor(m_peerManager.getPeerSnapshot(), nodeId);
    if (ep.empty())
    {
        MonitorLog::instance().warn("peer",
            "restartPeerApp: unknown or endpointless peer " + nodeId);
        return;
    }

    auto [host, port] = parseEndpoint(ep);
    if (host.empty())
        return;

    HttpRequest req;
    req.host     = host;
    req.port     = port;
    req.method   = "POST";
    req.endpoint = "/api/node/restart";
    req.callback = [nodeId](bool ok, const std::string& resp) {
        logPeerResult("peer restart", nodeId, ok, resp);
    };
    {
        std::lock_guard<std::mutex> lock(m_httpQueueMutex);
        m_httpQueue.push(std::move(req));
    }
}

MonitorApp::EditResult MonitorApp::applyEditLocally(
    const std::string& jobId, const std::string& manifestJson,
    const std::string& mode, int priority)
{
    using Code = EditResult::Code;

    if (!m_databaseManager.isOpen())
        return {Code::DbUnavailable, "leader database not ready"};

    JobManifest manifest;
    try
    {
        manifest = nlohmann::json::parse(manifestJson).get<JobManifest>();
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("job",
            "editJob: invalid manifest for " + jobId + ": " + e.what());
        return {Code::InvalidManifest, e.what()};
    }
    if (manifest.job_id != jobId)
    {
        MonitorLog::instance().error("job",
            "editJob: manifest job_id mismatch (" + manifest.job_id + " vs " + jobId + ")");
        return {Code::InvalidManifest, "manifest job_id mismatch"};
    }
    if (!m_databaseManager.getJob(jobId).has_value())
        return {Code::JobNotFound, "no such job: " + jobId};

    m_databaseManager.updateJobPriority(jobId, priority);
    m_databaseManager.updateJobManifest(jobId, manifestJson);

    if (mode == "restart" || mode == "startover")
    {
        // Snapshot who is rendering this job before the reset — they
        // get an abort push below. A node that misses the push just
        // finishes a wasted render; its stale report bounces off the
        // epoch fence / chunk-state guards.
        const auto nodes = m_databaseManager.getAssignedNodes(jobId);

        // Bump BEFORE the reset: anything re-assigned from here on
        // carries the new epoch, and reports stamped with the old one
        // are rejected (and tell the sender to self-abort).
        m_databaseManager.bumpEditEpoch(jobId);

        if (mode == "restart")
        {
            m_databaseManager.resetAssignedChunks(jobId);
        }
        else
        {
            m_databaseManager.rebuildChunks(jobId,
                computeChunks(manifest.frame_start, manifest.frame_end,
                              manifest.chunk_size));
        }
        m_databaseManager.updateJobState(jobId, "active");

        const nlohmann::json abortBody = {{"job_id", jobId}};
        for (const auto& nodeId : nodes)
        {
            if (nodeId == m_identity.nodeId())
                continue;
            const std::string ep = peerEndpointFor(m_peerManager.getPeerSnapshot(), nodeId);
            if (ep.empty())
                continue;   // offline — dead-worker reclaim covers it
            auto [host, port] = parseEndpoint(ep);
            if (host.empty())
                continue;

            HttpRequest req;
            req.host     = host;
            req.port     = port;
            req.method   = "POST";
            req.endpoint = "/api/render/abort";
            req.body     = abortBody.dump();
            req.callback = [nodeId](bool ok, const std::string& resp) {
                logPeerResult("render abort", nodeId, ok, resp);
            };
            {
                std::lock_guard<std::mutex> lock(m_httpQueueMutex);
                m_httpQueue.push(std::move(req));
            }
        }
        abandonJobRender(jobId, "job edited");
    }

    MonitorLog::instance().info("job", "Edited job " + jobId + " (mode=" + mode + ")");
    refreshCachedJobs();
    return {Code::Applied, {}};
}

void MonitorApp::editJob(const std::string& jobId, const std::string& manifestJson,
                          const std::string& mode, int priority,
                          std::function<void(const EditResult&)> done)
{
    using Code = EditResult::Code;

    if (isLeader())
    {
        // DbUnavailable and friends come back synchronously — a leader
        // must never fall into the POST branch (it would POST to itself).
        const EditResult r = applyEditLocally(jobId, manifestJson, mode, priority);
        if (done) done(r);
        return;
    }

    nlohmann::json body;
    try
    {
        body = {{"manifest", nlohmann::json::parse(manifestJson)},
                {"mode", mode},
                {"priority", priority}};
    }
    catch (const std::exception& e)
    {
        MonitorLog::instance().error("job",
            "editJob: invalid manifest for " + jobId + ": " + e.what());
        if (done) done({Code::InvalidManifest, e.what()});
        return;
    }
    postToLeaderAsyncEx("/api/jobs/" + jobId + "/edit", body.dump(),
        [jobId, manifestJson, mode, priority, done, this](
            int status, const std::string& resp) {
            EditResult r;
            if (status == 200)
            {
                MonitorLog::instance().info("job", "Edited job " + jobId);
                m_forceJobsRefresh.store(true);
                r = {Code::Applied, {}};
            }
            else if (status == 0)
            {
                // Leader unreachable over HTTP — fall back to the farm
                // share. Only for no-response: an HTTP error is a real
                // answer and must not be retried behind the user's back.
                if (writeEditCommand(jobId, manifestJson, mode, priority))
                {
                    MonitorLog::instance().info("job",
                        "Leader unreachable — queued edit for " + jobId +
                        " to the farm share");
                    r = {Code::QueuedToShare,
                         "queued to the farm — applies when a leader picks it up"};
                }
                else
                {
                    MonitorLog::instance().warn("job",
                        "Edit failed for " + jobId + " — no response from leader");
                    r = {Code::NoResponse, "no response from leader"};
                }
            }
            else if (status == 404 && resp.find("job_not_found") != std::string::npos)
            {
                r = {Code::JobNotFound, "leader has no such job"};
            }
            else if (status == 404)
            {
                // Route missing entirely — leader predates job editing.
                r = {Code::RouteMissing, "leader does not support job editing"};
            }
            else
            {
                MonitorLog::instance().warn("job",
                    "Edit failed for " + jobId + " — leader responded: " + resp);
                r = {Code::LeaderError,
                     resp.empty() ? ("HTTP " + std::to_string(status)) : resp};
            }
            if (done) done(r);
        });
}

bool MonitorApp::writeEditCommand(const std::string& jobId,
                                  const std::string& manifestJson,
                                  const std::string& mode, int priority)
{
    if (!m_farmRunning || jobId.empty())
        return false;

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json cmd;
    try
    {
        cmd = {
            {"type", "job_edit"},
            {"command_id", std::to_string(nowMs) + "_edit_" + jobId
                           + "_" + m_identity.nodeId()},
            {"job_id", jobId},
            {"manifest", nlohmann::json::parse(manifestJson)},
            {"mode", mode},
            {"priority", priority},
            {"issued_at_ms", nowMs},
            {"issued_by", m_identity.nodeId()},
        };
    }
    catch (...)
    {
        return false;
    }

    std::error_code ec;
    const auto cmdDir = m_farmPath / "commands";
    std::filesystem::create_directories(cmdDir, ec);
    if (ec)
        return false;

    // Atomic temp-write + rename, same as PeerManager::writeEndpoint —
    // the consumer must never see a half-written command. Filename =
    // command_id: lexical order is chronological, and the name doubles
    // as the idempotency key.
    const auto finalPath = cmdDir / (cmd["command_id"].get<std::string>() + ".json");
    const auto tmpPath   = cmdDir / (cmd["command_id"].get<std::string>() + ".tmp");
    {
        std::ofstream ofs(tmpPath);
        if (!ofs.is_open())
            return false;
        ofs << cmd.dump(2);
    }
    std::filesystem::rename(tmpPath, finalPath, ec);
    if (ec)
    {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
}

void MonitorApp::pollFarmCommands()
{
    if (!m_farmRunning || !isLeader() || !m_leaderDbReady.load()
        || !m_databaseManager.isOpen())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastCommandPoll < std::chrono::seconds(3))
        return;
    m_lastCommandPoll = now;

    std::error_code ec;
    const auto cmdDir = m_farmPath / "commands";
    if (!std::filesystem::is_directory(cmdDir, ec))
        return;

    const auto processedDir = cmdDir / "processed";
    std::filesystem::create_directories(processedDir, ec);

    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    constexpr int64_t kCommandTtlMs = 15 * 60 * 1000;

    // Sorted so commands apply in issue order (filenames lead with the
    // issue timestamp).
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(cmdDir, ec))
    {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());

    for (const auto& path : files)
    {
        // Every terminal path acks by moving to processed/ so a broken
        // or expired command is never retried forever.
        auto ack = [&](const char* why) {
            std::error_code mec;
            std::filesystem::rename(path, processedDir / path.filename(), mec);
            if (mec)
                std::filesystem::remove(path, mec);
            if (why[0] != '\0')
                MonitorLog::instance().warn("job",
                    "Farm command " + path.filename().string() + ": " + why);
        };

        nlohmann::json cmd;
        try
        {
            std::ifstream ifs(path);
            cmd = nlohmann::json::parse(ifs);
        }
        catch (...)
        {
            ack("unparseable — moved to processed/ unapplied");
            continue;
        }

        if (cmd.value("type", "") != "job_edit")
        {
            ack("unknown command type — moved to processed/ unapplied");
            continue;
        }
        const int64_t issuedAt = cmd.value("issued_at_ms", int64_t(0));
        if (issuedAt <= 0 || nowMs - issuedAt > kCommandTtlMs)
        {
            ack("expired (>15 min old) — moved to processed/ unapplied");
            continue;
        }

        const std::string jobId = cmd.value("job_id", "");
        const std::string mode  = cmd.value("mode", "continue");
        const int priority      = cmd.value("priority", 50);
        std::string manifestJson;
        if (cmd.contains("manifest"))
            manifestJson = cmd["manifest"].dump();

        const EditResult r = applyEditLocally(jobId, manifestJson, mode, priority);
        if (r.ok())
        {
            MonitorLog::instance().info("job",
                "Applied farm-share edit command for " + jobId +
                " (mode=" + mode + ", issued by " + cmd.value("issued_by", "?") + ")");
            ack("");
        }
        else if (r.code == EditResult::Code::DbUnavailable)
        {
            // Transient — leave the file for the next poll (or the
            // next leader); the TTL bounds how long it can linger.
            continue;
        }
        else
        {
            ack(("rejected: " + r.message + " — moved to processed/ unapplied").c_str());
        }
    }

    // Prune processed entries older than 7 days.
    const auto pruneBefore = std::filesystem::file_time_type::clock::now()
                             - std::chrono::hours(24 * 7);
    for (const auto& entry : std::filesystem::directory_iterator(processedDir, ec))
    {
        std::error_code pec;
        if (entry.is_regular_file(pec) && entry.last_write_time(pec) < pruneBefore)
            std::filesystem::remove(entry.path(), pec);
    }
}

void MonitorApp::abandonJobRender(const std::string& jobId, const std::string& reason)
{
    if (m_renderCoordinator.isRendering() && m_renderCoordinator.currentJobId() == jobId)
        m_renderCoordinator.abandonCurrentRender(reason);
    m_renderCoordinator.purgeJob(jobId);
}

std::string MonitorApp::getJobManifestJson(const std::string& jobId,
                                           std::string* errorOut)
{
    auto fail = [&](const char* why) -> std::string {
        if (errorOut) *errorOut = why;
        return {};
    };

    if (isLeader() && m_leaderDbReady.load() && m_databaseManager.isOpen())
    {
        auto jobOpt = m_databaseManager.getJob(jobId);
        if (!jobOpt.has_value())
            return fail("not_found");
        return jobOpt->manifest_json;
    }

    std::string leaderEp = getLeaderEndpoint();
    if (leaderEp.empty())
        return fail("no_leader");
    auto [host, port] = parseEndpoint(leaderEp);
    if (host.empty())
        return fail("no_leader");

    try
    {
        httplib::Client cli(host, port);
        // Off the UI thread since the async openJobEditor rework —
        // VPN-tier timeouts are safe here.
        cli.set_connection_timeout(3);
        cli.set_read_timeout(10);

        auto res = cli.Get("/api/jobs/" + jobId + "/manifest", authHeaders(m_farmSecret));
        if (!res)
            return fail("unreachable");
        if (res->status == 404)
        {
            // The route replies 404 with {"error":"not_found"} when the
            // job is missing; a bare/HTML 404 means the leader predates
            // the /manifest route entirely.
            return fail(res->body.find("not_found") != std::string::npos
                            ? "not_found" : "route_missing");
        }
        if (res->status != 200)
        {
            if (errorOut) *errorOut = "leader error: " + res->body;
            return {};
        }
        auto body = nlohmann::json::parse(res->body);
        if (!body.contains("manifest"))
            return fail("leader error: malformed manifest response");
        return body["manifest"].dump();
    }
    catch (...)
    {
        return fail("unreachable");
    }
}

bool MonitorApp::forgetPeer(const std::string& nodeId)
{
    if (!m_farmRunning || nodeId.empty())
        return false;
    if (nodeId == m_identity.nodeId())
    {
        MonitorLog::instance().warn("peer",
            "forgetPeer: refusing to remove own endpoint.json");
        return false;
    }

    std::error_code ec;
    auto endpointPath = m_farmPath / "nodes" / nodeId / "endpoint.json";
    if (std::filesystem::remove(endpointPath, ec))
    {
        MonitorLog::instance().info("peer",
            "Forgot peer (removed endpoint.json): " + nodeId);
        return true;
    }
    MonitorLog::instance().info("peer",
        "forgetPeer: no endpoint.json to remove for " + nodeId);
    return false;
}

bool MonitorApp::writePeerRestartSignal(const std::string& nodeId)
{
    if (!m_farmRunning || nodeId.empty())
        return false;

    std::error_code ec;
    auto nodeDir = m_farmPath / "nodes" / nodeId;
    if (!std::filesystem::is_directory(nodeDir, ec))
    {
        MonitorLog::instance().warn("peer",
            "writePeerRestartSignal: node directory missing for " + nodeId);
        return false;
    }

    auto signalPath = nodeDir / "restart";
    std::ofstream ofs(signalPath);
    if (!ofs.is_open())
    {
        MonitorLog::instance().warn("peer",
            "writePeerRestartSignal: failed to write " + signalPath.string());
        return false;
    }
    // Empty file — presence is the signal.
    MonitorLog::instance().info("peer",
        "Wrote restart signal for " + nodeId);
    return true;
}

// --- Farm cleanup (shared by HTTP handler + Qt FarmCleanupDialog) ---

nlohmann::json MonitorApp::scanFarmCleanup() const
{
    namespace fs = std::filesystem;
    std::error_code ec;
    nlohmann::json result;

    // Sections 1 & 2: Finished / archived jobs (leader only — worker
    // doesn't have DB access, so these come back empty).
    nlohmann::json finishedJobs = nlohmann::json::array();
    nlohmann::json archivedJobs = nlohmann::json::array();
    std::set<std::string> dbJobIds;

    if (isLeader() && m_databaseManager.isOpen())
    {
        auto allJobs = const_cast<DatabaseManager&>(m_databaseManager).getAllJobs();
        for (const auto& s : allJobs)
        {
            dbJobIds.insert(s.job.job_id);
            if (s.job.current_state == "completed"
                || s.job.current_state == "cancelled")
            {
                finishedJobs.push_back({
                    {"id",     s.job.job_id},
                    {"label",  s.job.job_id},
                    {"detail", s.job.current_state + " | "
                               + std::to_string(s.progress.total) + " chunks"}
                });
            }
            else if (s.job.current_state == "archived")
            {
                archivedJobs.push_back({
                    {"id",     s.job.job_id},
                    {"label",  s.job.job_id},
                    {"detail", "archived"}
                });
            }
        }
    }
    result["finished_jobs"] = finishedJobs;
    result["archived_jobs"] = archivedJobs;

    // Section 3: Orphaned jobs/ subdirs — directories under
    // {farmPath}/jobs that don't match any known job_id.
    nlohmann::json orphanedDirs = nlohmann::json::array();
    {
        auto jobsDir = m_farmPath / "jobs";
        if (fs::is_directory(jobsDir, ec))
        {
            if (dbJobIds.empty())
                for (const auto& j : m_cachedJobs)
                    dbJobIds.insert(j.manifest.job_id);

            for (const auto& entry : fs::directory_iterator(jobsDir, ec))
            {
                if (!entry.is_directory()) continue;
                std::string dirName = entry.path().filename().string();
                if (dbJobIds.find(dirName) == dbJobIds.end())
                {
                    orphanedDirs.push_back({
                        {"id",     entry.path().string()},
                        {"label",  dirName},
                        {"detail", "no matching job"}
                    });
                }
            }
        }
    }
    result["orphaned_dirs"] = orphanedDirs;

    // Section 4: Stale peers — offline peers.
    nlohmann::json stalePeers = nlohmann::json::array();
    {
        auto peers = const_cast<PeerManager&>(m_peerManager).getPeerSnapshot();
        for (const auto& p : peers)
        {
            if (!p.is_alive && !p.is_local)
            {
                stalePeers.push_back({
                    {"id",     p.node_id},
                    {"label",  p.hostname + " (" + p.node_id.substr(0, 8) + ")"},
                    {"detail", "last seen: " + std::to_string(p.last_seen_ms)}
                });
            }
        }
    }
    result["stale_peers"] = stalePeers;

    // Sections 5 & 6: Staging directories under the LOCAL app-data
    // (not the shared farm) — per-job staging copies that didn't
    // clean up when the job finished.
    nlohmann::json staleStagingDirs   = nlohmann::json::array();
    nlohmann::json failedStagingCopies = nlohmann::json::array();
    {
        auto stagingRoot = getAppDataDir() / "staging";
        if (fs::is_directory(stagingRoot, ec))
        {
            for (const auto& jobEntry : fs::directory_iterator(stagingRoot, ec))
            {
                if (!jobEntry.is_directory(ec)) continue;
                std::string jobName = jobEntry.path().filename().string();

                int fileCount = 0;
                uintmax_t totalSize = 0;
                for (const auto& f :
                     fs::recursive_directory_iterator(jobEntry.path(), ec))
                {
                    if (f.is_regular_file(ec))
                    {
                        fileCount++;
                        totalSize += f.file_size(ec);
                    }
                }

                if (fileCount > 0)
                {
                    std::string sizeStr;
                    if (totalSize < 1024 * 1024)
                        sizeStr = std::to_string(totalSize / 1024) + " KB";
                    else if (totalSize < 1024ULL * 1024 * 1024)
                        sizeStr = std::to_string(totalSize / (1024 * 1024)) + " MB";
                    else
                        sizeStr = std::to_string(totalSize / (1024ULL * 1024 * 1024)) + " GB";

                    failedStagingCopies.push_back({
                        {"id",     jobEntry.path().string()},
                        {"label",  jobName},
                        {"detail", std::to_string(fileCount)
                                   + " files (" + sizeStr + ")"}
                    });
                }
                else
                {
                    staleStagingDirs.push_back({
                        {"id",     jobEntry.path().string()},
                        {"label",  jobName},
                        {"detail", "empty"}
                    });
                }
            }
        }
    }
    result["stale_staging_dirs"]    = staleStagingDirs;
    result["failed_staging_copies"] = failedStagingCopies;
    result["is_leader"] = isLeader();

    return result;
}

int MonitorApp::executeFarmCleanup(const std::string& action,
                                   const std::vector<std::string>& ids)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    int count = 0;

    // Every id here came from scanFarmCleanup's output in the honest case,
    // but the same action/ids pair is accepted over the API, so each
    // branch re-validates before it touches the filesystem.
    auto rejectUnsafe = [](const std::string& id) {
        MonitorLog::instance().warn("farm", "Cleanup: refusing unsafe id: " + id);
    };

    if (action == "archive")
    {
        for (const auto& id : ids)
        {
            if (!isSafePathSegment(id)) { rejectUnsafe(id); continue; }
            archiveJob(id); count++;
        }
    }
    else if (action == "delete_jobs")
    {
        for (const auto& id : ids)
        {
            if (!isSafePathSegment(id)) { rejectUnsafe(id); continue; }
            deleteJob(id); count++;
        }
    }
    else if (action == "delete_dirs")
    {
        // Orphaned job dirs live under <farm>/jobs; stale staging dirs
        // under the local app-data staging root. Nothing else is ever a
        // legitimate target, so an absolute path outside both is refused
        // no matter who asked.
        const fs::path jobsRoot    = m_farmPath / "jobs";
        const fs::path stagingRoot = getAppDataDir() / "staging";
        for (const auto& id : ids)
        {
            const fs::path target(id);
            if (!target.is_absolute()
                || !(isPathWithin(jobsRoot, target) || isPathWithin(stagingRoot, target)))
            {
                rejectUnsafe(id);
                continue;
            }
            fs::remove_all(target, ec);
            if (!ec) count++;
            else MonitorLog::instance().warn("farm",
                "Cleanup: failed to remove " + id + ": " + ec.message());
        }
    }
    else if (action == "remove_peers")
    {
        for (const auto& id : ids)
        {
            if (!isSafePathSegment(id)) { rejectUnsafe(id); continue; }
            auto nodeDir = m_farmPath / "nodes" / id;
            fs::remove_all(nodeDir, ec);
            if (!ec) count++;
            else MonitorLog::instance().warn("farm",
                "Cleanup: failed to remove peer " + id + ": " + ec.message());
        }
    }
    else
    {
        MonitorLog::instance().warn("farm",
            "Cleanup: unknown action " + action);
    }
    return count;
}

// --- Node state ---

void MonitorApp::setNodeState(NodeState state)
{
    m_nodeState = state;
    if (state == NodeState::Stopped)
    {
        m_renderCoordinator.setStopped(true);
        m_renderCoordinator.abortCurrentRender("node stopped");
        m_peerManager.setNodeState("stopped");
    }
    else
    {
        m_renderCoordinator.setStopped(false);
        m_peerManager.setNodeState("active");
    }

    // Persist across restarts
    m_config.node_stopped = (state == NodeState::Stopped);
    saveConfig();
}

// --- Leader endpoint helper ---

std::string MonitorApp::getLeaderEndpoint() const
{
    auto peers = m_peerManager.getPeerSnapshot();
    for (const auto& p : peers)
    {
        if (p.is_leader && p.is_alive)
            return p.endpoint;
    }
    // If we are the leader, return our own endpoint
    if (isLeader())
    {
        return advertisedIp() + ":" + std::to_string(m_config.http_port);
    }
    return {};
}

bool MonitorApp::leaderSupports(const std::string& capability) const
{
    // Our own build always matches its own capability set.
    if (isLeader())
        return true;

    auto peers = m_peerManager.getPeerSnapshot();
    for (const auto& p : peers)
    {
        if (!p.is_leader || !p.is_alive)
            continue;
        // Old builds never send capabilities — empty means the v0
        // feature set, which predates every gated capability.
        return std::find(p.capabilities.begin(), p.capabilities.end(),
                         capability) != p.capabilities.end();
    }
    return false;
}

// --- Tray state ---

std::string MonitorApp::trayIconName() const
{
    if (!m_farmRunning)
        return "gray";

    if (m_nodeState == NodeState::Stopped)
        return "gray";

    if (!m_agentSupervisor.isAgentConnected())
        return "red";

    if (m_renderCoordinator.isRendering())
        return "green";

    return "blue";
}

std::string MonitorApp::trayTooltip() const
{
    std::string tip = "minRender";
    if (m_renderCoordinator.isRendering())
        tip += " - Rendering " + m_renderCoordinator.currentJobId();
    else if (m_nodeState == NodeState::Stopped)
        tip += " - Stopped";
    else if (!m_farmRunning)
        tip += " - No Farm";
    else
        tip += " - Idle";
    return tip;
}

std::string MonitorApp::trayStatusText() const
{
    if (m_renderCoordinator.isRendering())
        return "Rendering " + m_renderCoordinator.currentChunkLabel();
    if (m_nodeState == NodeState::Stopped)
        return "Stopped";
    if (!m_farmRunning)
        return "No Farm";
    return "Idle";
}

// --- Exit flow ---

void MonitorApp::requestExit()
{
    m_exitRequested = true;
    if (!m_renderCoordinator.isRendering())
        m_shouldExit = true;
}

void MonitorApp::beginForceExit()
{
    m_renderCoordinator.abortCurrentRender("shutdown");
    m_shouldExit = true;
}

void MonitorApp::cancelExit()
{
    m_exitRequested = false;
}

// --- Restart sidecar ---

bool MonitorApp::launchRestartSidecar()
{
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();

    wchar_t exePathW[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::filesystem::path exePath(exePathW);

    // Find mr-restart.exe next to monitor exe
    std::filesystem::path restartExe = exePath.parent_path() / "mr-restart.exe";
    if (!std::filesystem::exists(restartExe))
    {
        MonitorLog::instance().error("farm", "mr-restart.exe not found at: " + restartExe.string());
        return false;
    }

    // Cleanup leftover restart.bat from older versions
    {
        auto oldBat = m_appDataDir / "restart.bat";
        std::error_code ec;
        std::filesystem::remove(oldBat, ec);
    }

    // Build command line
    std::wstring cmdLine = L"\"" + restartExe.wstring() + L"\" --pid " +
        std::to_wstring(pid) + L" --exe \"" + exePath.wstring() + L"\" --minimized";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    BOOL ok = CreateProcessW(
        restartExe.wstring().c_str(),
        cmdLine.data(),
        nullptr, nullptr, FALSE,
        CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr,
        &si, &pi);

    if (ok)
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        MonitorLog::instance().info("farm", "Restart sidecar launched (PID " + std::to_string(pid) + ")");
    }
    else
    {
        MonitorLog::instance().error("farm", "Failed to launch restart sidecar");
        return false;
    }

    // Defer shutdown so mr-restart.exe has time to open process handle
    m_restartPending = true;
    m_restartLaunchedAt = std::chrono::steady_clock::now();
    return true;
#else
    MonitorLog::instance().warn("farm", "Restart sidecar not supported on this platform");
    return false;
#endif
}

// --- Job selection ---

void MonitorApp::selectJob(const std::string& id)
{
    m_selectedJobId = id;
}

void MonitorApp::requestSubmissionMode()
{
    m_requestSubmission = true;
}

bool MonitorApp::shouldEnterSubmission()
{
    bool val = m_requestSubmission;
    m_requestSubmission = false;
    return val;
}

// --- Build local PeerInfo for HTTP responses ---

std::string MonitorApp::farmSecretFingerprint() const
{
    return secretFingerprint(m_farmSecret);
}

PeerInfo MonitorApp::buildLocalPeerInfo() const
{
    PeerInfo info;
    info.node_id = m_identity.nodeId();
    info.secret_fingerprint = farmSecretFingerprint();
    info.hostname = m_identity.systemInfo().hostname;
    info.os = getOS();
    info.app_version = APP_VERSION;
    info.gpu_name = m_identity.systemInfo().gpuName;
    info.cpu_cores = m_identity.systemInfo().cpuCores;
    info.ram_mb = m_identity.systemInfo().ramMB;
    info.node_state = (m_nodeState == NodeState::Active) ? "active" : "stopped";
    info.tags = m_config.tags;
    // Advertised feature capabilities — additive tokens, one per feature
    // that peers must not assume across versions. Old builds omit the
    // field entirely and are treated as the v0 feature set.
    info.capabilities = {"job_edit", "edit_result", "edit_epoch",
                         "edit_commands", "chunk_stop"};
    info.is_local = true;
    info.is_alive = true;

    // Render state
    if (m_renderCoordinator.isRendering())
    {
        info.render_state = "rendering";
        info.active_job = m_renderCoordinator.currentJobId();
        info.active_chunk = m_renderCoordinator.currentChunkLabel();
    }
    else
    {
        info.render_state = "idle";
    }

    // Agent health
    info.agent_health = m_agentSupervisor.agentHealth();
    if (m_agentSupervisor.agentHealthEnum() != AgentHealth::Ok)
    {
        info.alert_reason = "Agent " + m_agentSupervisor.agentHealth();
        if (!m_agentSupervisor.isAgentConnected())
            info.alert_reason += " (disconnected)";
    }

    // Readiness gating
    info.ready_for_work = m_agentSupervisor.readyForWork();

    // Endpoint
    info.endpoint = advertisedIp() + ":" + std::to_string(m_config.http_port);

    return info;
}

// --- UDP multicast ---

void MonitorApp::handleUdpMessages()
{
    auto messages = m_udpNotify.poll();
    for (const auto& msg : messages)
    {
        std::string type = msg.value("t", "");
        if (type == "hb")
        {
            std::string nodeId = msg.value("n", "");
            if (nodeId.empty() || nodeId == m_identity.nodeId())
                continue;

            std::string ip = msg.value("ip", "");
            uint16_t port = msg.value("port", uint16_t(8420));
            std::string st = msg.value("st", "active");
            std::string rs = msg.value("rs", "idle");
            std::string job = msg.value("job", "");
            std::string chunk = msg.value("chunk", "");
            int pri = msg.value("pri", 100);
            std::string ah = msg.value("ah", "ok");
            std::string ar = msg.value("ar", "");
            bool rfw = msg.value("rfw", true);

            m_peerManager.processUdpHeartbeat(nodeId, ip, port, st, rs, job, chunk, pri, ah, ar, rfw);
        }
        else if (type == "bye")
        {
            std::string nodeId = msg.value("n", "");
            if (!nodeId.empty() && nodeId != m_identity.nodeId())
                m_peerManager.processUdpGoodbye(nodeId);
        }
    }
}

void MonitorApp::sendUdpHeartbeat()
{
    std::string localIp = advertisedIp();

    nlohmann::json hb = {
        {"t", "hb"},
        {"from", m_identity.nodeId()},
        {"n", m_identity.nodeId()},
        {"ip", localIp},
        {"port", m_config.http_port},
        {"st", (m_nodeState == NodeState::Active) ? "active" : "stopped"},
        {"rs", m_renderCoordinator.isRendering() ? "rendering" : "idle"},
        // "pri" (leader-election priority) dropped — election ignores
        // it; old receivers default missing "pri" to 100.
        {"ah", m_agentSupervisor.agentHealth()},
        {"rfw", m_agentSupervisor.readyForWork()},
    };

    if (m_renderCoordinator.isRendering())
    {
        hb["job"] = m_renderCoordinator.currentJobId();
        hb["chunk"] = m_renderCoordinator.currentChunkLabel();
    }

    if (m_agentSupervisor.agentHealthEnum() != AgentHealth::Ok)
    {
        std::string reason = "Agent " + m_agentSupervisor.agentHealth();
        if (!m_agentSupervisor.isAgentConnected())
            reason += " (disconnected)";
        hb["ar"] = reason;
    }

    m_udpNotify.send(hb);
}

// --- Background HTTP worker ---

void MonitorApp::postToLeaderAsync(const std::string& endpoint, const std::string& body,
                                    std::function<void(bool, const std::string&)> callback,
                                    const std::string& method)
{
    std::string leaderEp = getLeaderEndpoint();
    if (leaderEp.empty())
    {
        if (callback) callback(false, "");
        return;
    }

    auto [host, port] = parseEndpoint(leaderEp);
    if (host.empty())
    {
        if (callback) callback(false, "");
        return;
    }

    HttpRequest req;
    req.host = host;
    req.port = port;
    req.method = method;
    req.endpoint = endpoint;
    req.body = body;
    req.callback = std::move(callback);

    {
        std::lock_guard<std::mutex> lock(m_httpQueueMutex);
        m_httpQueue.push(std::move(req));
    }
}

void MonitorApp::postToLeaderAsyncEx(const std::string& endpoint, const std::string& body,
                                     std::function<void(int status, const std::string& response)> statusCallback,
                                     const std::string& method)
{
    std::string leaderEp = getLeaderEndpoint();
    if (leaderEp.empty())
    {
        if (statusCallback) statusCallback(0, "");
        return;
    }

    auto [host, port] = parseEndpoint(leaderEp);
    if (host.empty())
    {
        if (statusCallback) statusCallback(0, "");
        return;
    }

    HttpRequest req;
    req.host = host;
    req.port = port;
    req.method = method;
    req.endpoint = endpoint;
    req.body = body;
    req.statusCallback = std::move(statusCallback);

    {
        std::lock_guard<std::mutex> lock(m_httpQueueMutex);
        m_httpQueue.push(std::move(req));
    }
}

void MonitorApp::startHttpWorker()
{
    m_httpWorkerRunning.store(true);
    m_httpWorkerThread = std::thread(&MonitorApp::httpWorkerLoop, this);
}

void MonitorApp::stopHttpWorker()
{
    m_httpWorkerRunning.store(false);
    if (m_httpWorkerThread.joinable())
        m_httpWorkerThread.join();
}

void MonitorApp::httpWorkerLoop()
{
    auto reportCooldown = std::chrono::steady_clock::time_point{};
    auto lastFrameFlush = std::chrono::steady_clock::now();

    while (m_httpWorkerRunning.load())
    {
        // 1. Process one-off requests from queue (job controls, submissions)
        {
            std::unique_lock<std::mutex> lock(m_httpQueueMutex);
            while (!m_httpQueue.empty())
            {
                auto req = std::move(m_httpQueue.front());
                m_httpQueue.pop();
                lock.unlock();

                int status = 0;   // 0 = no response
                std::string response;
                try
                {
                    httplib::Client cli(req.host, req.port);
                    // Command POSTs are rare, user-initiated, and off
                    // the UI thread — allow for home-VPN latency
                    // instead of the LAN-tuned 500ms.
                    cli.set_connection_timeout(3);
                    cli.set_read_timeout(10);
                    auto hdrs = authHeaders(m_farmSecret);

                    httplib::Result res = (req.method == "DELETE")
                        ? cli.Delete(req.endpoint, hdrs)
                        : cli.Post(req.endpoint, hdrs, req.body, "application/json");

                    if (res)
                    {
                        status = res->status;
                        response = res->body;
                    }
                }
                catch (...) {}

                if (req.callback)
                    req.callback(status == 200, response);
                if (req.statusCallback)
                    req.statusCallback(status, response);

                lock.lock();
            }
        }

        auto now = std::chrono::steady_clock::now();

        // 2. Flush completion reports (respecting cooldown)
        if (now >= reportCooldown && m_farmRunning)
        {
            if (flushCompletionReports())
                reportCooldown = now + std::chrono::seconds(5);
        }

        // 3. Flush frame reports every 2s (respecting cooldown)
        auto frameElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastFrameFlush).count();
        if (frameElapsed >= 2000 && now >= reportCooldown && m_farmRunning)
        {
            if (flushFrameReports())
                reportCooldown = now + std::chrono::seconds(5);
            lastFrameFlush = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool MonitorApp::flushCompletionReports()
{
    std::vector<PendingReport> batch;
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        if (m_pendingReports.empty()) return false;
        batch = std::move(m_pendingReports);
        m_pendingReports.clear();
    }

    std::string leaderEp = getLeaderEndpoint();
    if (leaderEp.empty())
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingReports.insert(m_pendingReports.begin(), batch.begin(), batch.end());
        return false;
    }

    auto [host, port] = parseEndpoint(leaderEp);
    if (host.empty())
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingReports.insert(m_pendingReports.begin(), batch.begin(), batch.end());
        return false;
    }

    bool anyFailed = false;
    std::vector<PendingReport> unsent;

    for (auto& report : batch)
    {
        if (anyFailed)
        {
            unsent.push_back(std::move(report));
            continue;
        }

        try
        {
            httplib::Client cli(host, port);
            // Reports are buffered + retried, but a home-VPN node with
            // >500ms RTT would never land one — give them room.
            cli.set_connection_timeout(2);
            cli.set_read_timeout(5);

            nlohmann::json body = {
                {"node_id", m_identity.nodeId()},
                {"job_id", report.jobId},
                {"frame_start", report.frameStart},
                {"frame_end", report.frameEnd},
            };
            if (report.editEpoch >= 0)
                body["edit_epoch"] = report.editEpoch;

            std::string endpoint;
            if (report.state == "completed")
            {
                body["elapsed_ms"] = report.elapsedMs;
                body["exit_code"] = report.exitCode;
                endpoint = "/api/dispatch/complete";
            }
            else
            {
                body["error"] = report.error;
                endpoint = "/api/dispatch/failed";
            }

            auto res = cli.Post(endpoint, authHeaders(m_farmSecret), body.dump(), "application/json");
            if (!res || res->status != 200)
            {
                anyFailed = true;
                unsent.push_back(std::move(report));
            }
            else if (res->body.find("\"stale\"") != std::string::npos)
            {
                // The leader rejected the report as pre-edit (epoch
                // fence). Drop it — retrying can never succeed — and
                // never blacklist anyone over it. (200, not an error
                // status: a non-200 would requeue forever.)
                MonitorLog::instance().warn("render",
                    "Leader fenced stale report for " + report.jobId +
                    " f" + std::to_string(report.frameStart) +
                    "-" + std::to_string(report.frameEnd) + " — dropped");
            }
        }
        catch (...)
        {
            anyFailed = true;
            unsent.push_back(std::move(report));
        }
    }

    if (!unsent.empty())
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        // Prepend unsent reports before any new reports that arrived during flush
        unsent.insert(unsent.end(),
            std::make_move_iterator(m_pendingReports.begin()),
            std::make_move_iterator(m_pendingReports.end()));
        m_pendingReports = std::move(unsent);
    }

    return anyFailed;
}

bool MonitorApp::flushFrameReports()
{
    std::vector<PendingFrameReport> batch;
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        if (m_pendingFrameReports.empty()) return false;
        batch = std::move(m_pendingFrameReports);
        m_pendingFrameReports.clear();
    }

    std::string leaderEp = getLeaderEndpoint();
    if (leaderEp.empty())
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingFrameReports.insert(m_pendingFrameReports.begin(), batch.begin(), batch.end());
        return false;
    }

    auto [host, port] = parseEndpoint(leaderEp);
    if (host.empty())
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingFrameReports.insert(m_pendingFrameReports.begin(), batch.begin(), batch.end());
        return false;
    }

    // Group by (job, epoch) so a stale verdict applies to a whole batch
    std::map<std::pair<std::string, int64_t>, std::vector<int>> byJob;
    for (const auto& fr : batch)
        byJob[{fr.jobId, fr.editEpoch}].push_back(fr.frame);

    bool allSent = true;
    for (auto& [key, frames] : byJob)
    {
        const std::string& jobId = key.first;
        const int64_t epoch = key.second;
        try
        {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(2);
            cli.set_read_timeout(5);

            nlohmann::json body = {
                {"node_id", m_identity.nodeId()},
                {"job_id", jobId},
                {"frames", frames},
            };
            if (epoch >= 0)
                body["edit_epoch"] = epoch;

            auto res = cli.Post("/api/dispatch/frame-complete",
                authHeaders(m_farmSecret), body.dump(), "application/json");
            if (!res || res->status != 200)
            {
                allSent = false;
            }
            else if (res->body.find("\"stale\"") != std::string::npos)
            {
                // The job was edited while we render it — this is the
                // self-fence that stops a node that missed its abort
                // push from writing the rest of the chunk into the
                // shared output dir. Same cross-thread abandon the
                // /api/render/abort route performs.
                if (m_renderCoordinator.isRendering()
                    && m_renderCoordinator.currentJobId() == jobId
                    && m_renderCoordinator.currentEditEpoch() == epoch)
                {
                    MonitorLog::instance().warn("render",
                        "Leader fenced our frame reports for " + jobId +
                        " (job edited) — abandoning current render");
                    abandonJobRender(jobId, "stale edit epoch");
                }
                // Batch is dropped either way — it can never land.
            }
        }
        catch (...)
        {
            allSent = false;
        }
    }

    if (!allSent)
    {
        std::lock_guard<std::mutex> lock(m_reportMutex);
        m_pendingFrameReports.insert(m_pendingFrameReports.begin(), batch.begin(), batch.end());
    }

    return !allSent;
}

} // namespace MR
