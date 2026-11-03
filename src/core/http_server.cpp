#include "core/http_server.h"
#include "core/peer_info.h"
#include "core/monitor_log.h"
#include "core/platform.h"
#include "core/path_mapping.h"
#include "monitor/monitor_app.h"
#include "monitor/dispatch_manager.h"
#include "monitor/database_manager.h"

#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

namespace MR {

namespace {

// Length-independent compare so a wrong token can't be recovered byte by byte
// from response timing. Length is not secret (the secret is fixed-width hex).
bool constantTimeEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

// Path-segment validation lives in core/platform.h (isSafePathSegment) so
// MonitorApp applies the identical rule to ids that arrive through the Qt
// UI. The route regexes use [^/]+, which still admits "." and ".." — enough
// to walk out of the intended directory once httplib has percent-decoded
// the path — so every id that becomes a path component is checked here.

} // namespace

void HttpServer::init(MonitorApp* app)
{
    m_app = app;
}

bool HttpServer::requireLeader(httplib::Response& res)
{
    if (!m_app->isLeader())
    {
        nlohmann::json body = {{"error", "not_leader"}};
        auto peers = m_app->peerManager().getPeerSnapshot();
        for (const auto& p : peers)
        {
            if (p.is_leader)
            {
                body["leader_endpoint"] = p.endpoint;
                break;
            }
        }
        res.status = 503;
        res.set_content(body.dump(), "application/json");
        return false;
    }
    return true;
}

void HttpServer::setupRoutes()
{
    // GET /api/status -- this node's PeerInfo as JSON
    m_server.Get("/api/status", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }

        PeerInfo info = m_app->buildLocalPeerInfo();
        nlohmann::json j = info;
        res.set_content(j.dump(), "application/json");
    });

    // GET /api/peers -- list of known peers (from PeerManager snapshot)
    m_server.Get("/api/peers", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }

        auto peers = m_app->peerManager().getPeerSnapshot();
        nlohmann::json j = peers;
        res.set_content(j.dump(), "application/json");
    });

    // --- Remote node control (every node) ---

    // POST /api/node/stop -- remotely stop rendering on this node
    m_server.Post("/api/node/stop", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        m_app->setNodeState(NodeState::Stopped);
        MonitorLog::instance().info("farm", "Remotely stopped by peer");
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/node/start -- remotely resume rendering on this node
    m_server.Post("/api/node/start", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        m_app->setNodeState(NodeState::Active);
        MonitorLog::instance().info("farm", "Remotely started by peer");
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/node/restart -- remotely restart the entire monitor app via sidecar
    m_server.Post("/api/node/restart", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        MonitorLog::instance().info("farm", "Remote app restart requested");
        if (m_app->launchRestartSidecar())
            res.set_content(R"({"status":"ok"})", "application/json");
        else
        {
            res.status = 500;
            res.set_content(R"({"error":"restart_failed"})", "application/json");
        }
    });

    // --- Remote agent control (every node) ---

    // POST /api/agent/restart -- remotely restart agent on this node
    m_server.Post("/api/agent/restart", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        MonitorLog::instance().info("agent", "Remote agent restart requested");
        m_app->agentSupervisor().shutdownAgent();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (m_app->agentSupervisor().spawnAgent())
        {
            m_app->agentSupervisor().resetHealth();
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        else
        {
            res.status = 500;
            res.set_content(R"({"error":"agent_spawn_failed"})", "application/json");
        }
    });

    // --- Worker endpoint (every node) ---

    // POST /api/dispatch/assign -- receives assignment from leader
    m_server.Post("/api/dispatch/assign", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }

        // If this node is currently rendering, reject
        if (m_app->renderCoordinator().isRendering())
        {
            res.status = 409;
            res.set_content(R"({"error":"busy"})", "application/json");
            return;
        }

        // If stopped, reject
        if (m_app->nodeState() == NodeState::Stopped)
        {
            res.status = 409;
            res.set_content(R"({"error":"stopped"})", "application/json");
            return;
        }

        try
        {
            auto body = nlohmann::json::parse(req.body);
            auto manifest = body.at("manifest").get<JobManifest>();
            // job_id becomes jobs/<id>/stdout/<node>/ on the share.
            if (!isSafePathSegment(manifest.job_id))
            {
                res.status = 400;
                res.set_content(R"({"error":"invalid_job_id"})", "application/json");
                return;
            }
            int frameStart = body.at("frame_start").get<int>();
            int frameEnd = body.at("frame_end").get<int>();
            // Absent on old leaders — reports then omit it and get the
            // legacy (pre-epoch) guards.
            int64_t editEpoch = body.value("edit_epoch", int64_t(-1));

            ChunkRange chunk{frameStart, frameEnd};
            m_app->renderCoordinator().queueDispatch(manifest, chunk, editEpoch);

            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/render/abort -- leader push after a job edit reset this
    // node's in-flight chunk: abandon the render (no failure bookkeeping)
    // and drop any queued dispatches for the job.
    m_server.Post("/api/render/abort", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app)
        {
            res.status = 503;
            return;
        }
        try
        {
            auto body = nlohmann::json::parse(req.body);
            const std::string jobId = body.at("job_id").get<std::string>();
            m_app->abandonJobRender(jobId, body.value("reason", "job edited"));
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // --- Leader-only endpoints ---

    // POST /api/jobs -- submit a new job
    m_server.Post("/api/jobs", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            auto manifest = body.at("manifest").get<JobManifest>();
            // The id names the job's directory on the share for its whole
            // life (stdout logs, deleteJob's remove_all), so reject
            // anything that is not a plain slug at the door.
            if (!isSafePathSegment(manifest.job_id))
            {
                res.status = 400;
                res.set_content(R"({"error":"invalid_job_id"})", "application/json");
                return;
            }
            int priority = body.value("priority", 50);

            SubmitRequest sr;
            sr.manifest = std::move(manifest);
            sr.priority = priority;
            m_app->dispatchManager().queueSubmission(std::move(sr));

            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // GET /api/jobs -- list all jobs with progress
    m_server.Get("/api/jobs", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        auto json = m_app->getCachedJobsJson();
        res.set_content(json, "application/json");
    });

    // GET /api/jobs/:id -- single job detail + chunks
    m_server.Get(R"(/api/jobs/([^/]+))", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        std::string jobId = req.matches[1];
        auto json = m_app->getCachedJobDetailJson(jobId);
        if (json.empty())
        {
            res.status = 404;
            res.set_content(R"({"error":"not_found"})", "application/json");
            return;
        }
        res.set_content(json, "application/json");
    });

    // POST /api/jobs/:id/pause
    m_server.Post(R"(/api/jobs/([^/]+)/pause)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->pauseJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/resume
    m_server.Post(R"(/api/jobs/([^/]+)/resume)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->resumeJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/cancel
    m_server.Post(R"(/api/jobs/([^/]+)/cancel)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->cancelJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/archive
    m_server.Post(R"(/api/jobs/([^/]+)/archive)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->archiveJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // DELETE /api/jobs/:id — also removes jobs/<id> from the share, so the
    // id must be a plain segment (MonitorApp::deleteJob re-checks).
    m_server.Delete(R"(/api/jobs/([^/]+))", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        if (!isSafePathSegment(jobId))
        {
            res.status = 400;
            res.set_content(R"({"error":"invalid_path"})", "application/json");
            return;
        }
        m_app->deleteJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/move -- reorder within the equal-priority group.
    // Body: {"target": "<jobId>", "position": "before"|"after"}
    m_server.Post(R"(/api/jobs/([^/]+)/move)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        try
        {
            auto body = nlohmann::json::parse(req.body);
            std::string target = body.at("target").get<std::string>();
            bool before = body.value("position", "before") == "before";
            if (!m_app->moveJob(jobId, target, before))
            {
                res.status = 409;
                res.set_content(R"({"error":"move_rejected"})", "application/json");
                return;
            }
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/jobs/:id/priority -- change priority after submission.
    // Body: {"priority": N}
    m_server.Post(R"(/api/jobs/([^/]+)/priority)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        try
        {
            auto body = nlohmann::json::parse(req.body);
            int priority = body.at("priority").get<int>();
            m_app->setJobPriority(jobId, priority);
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/jobs/:id/retry-failed -- retry only failed chunks
    m_server.Post(R"(/api/jobs/([^/]+)/retry-failed)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->retryFailedChunks(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // POST /api/jobs/:id/requeue -- reset every chunk and flip the job
    // back to active. The worker-side MonitorApp::requeueJob forwards
    // here via postToLeaderAsync; the Qt JobDetailPanel Requeue button
    // was a silent no-op on workers until this endpoint existed.
    m_server.Post(R"(/api/jobs/([^/]+)/requeue)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        m_app->requeueJob(jobId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // GET /api/jobs/:id/manifest -- full stored manifest (for job editing
    // from worker nodes)
    m_server.Get(R"(/api/jobs/([^/]+)/manifest)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        const std::string manifest = m_app->getJobManifestJson(jobId);
        if (manifest.empty())
        {
            res.status = 404;
            res.set_content(R"({"error":"not_found"})", "application/json");
            return;
        }
        try
        {
            nlohmann::json resp = {{"manifest", nlohmann::json::parse(manifest)}};
            res.set_content(resp.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 500;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/jobs/:id/edit -- apply an edited manifest to an existing job.
    // Body: {manifest: {...}, mode: "continue"|"restart"|"startover", priority: N}
    m_server.Post(R"(/api/jobs/([^/]+)/edit)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];
        try
        {
            auto body = nlohmann::json::parse(req.body);
            const std::string manifest = body.at("manifest").dump();
            const std::string mode     = body.value("mode", "continue");
            const int priority         = body.value("priority", 50);

            using Code = MonitorApp::EditResult::Code;
            const auto r = m_app->applyEditLocally(jobId, manifest, mode, priority);
            switch (r.code)
            {
            case Code::Applied:
                res.set_content(R"({"status":"ok"})", "application/json");
                break;
            case Code::JobNotFound:
                res.status = 404;
                res.set_content(R"({"error":"job_not_found"})", "application/json");
                break;
            case Code::InvalidManifest:
                res.status = 400;
                res.set_content(
                    nlohmann::json{{"error", "invalid_manifest"},
                                   {"detail", r.message}}.dump(),
                    "application/json");
                break;
            case Code::DbUnavailable:
            default:
                res.status = 503;
                res.set_content(R"({"error":"db_unavailable"})", "application/json");
                break;
            }
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/jobs/:id/resubmit -- create new job from existing manifest
    // Optional JSON body with frame_start, frame_end, chunk_size for chunk resubmit
    m_server.Post(R"(/api/jobs/([^/]+)/resubmit)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string jobId = req.matches[1];

        std::string newId;
        if (!req.body.empty())
        {
            try
            {
                auto body = nlohmann::json::parse(req.body);
                if (body.value("incomplete_only", false))
                {
                    newId = m_app->resubmitIncomplete(jobId);
                }
                else if (body.contains("frame_start"))
                {
                    int frameStart = body.at("frame_start").get<int>();
                    int frameEnd = body.at("frame_end").get<int>();
                    int chunkSize = body.at("chunk_size").get<int>();
                    newId = m_app->resubmitChunkAsJob(jobId, frameStart, frameEnd, chunkSize);
                }
                else
                {
                    newId = m_app->resubmitJob(jobId);
                }
            }
            catch (const std::exception& e)
            {
                res.status = 400;
                nlohmann::json err = {{"error", e.what()}};
                res.set_content(err.dump(), "application/json");
                return;
            }
        }
        else
        {
            newId = m_app->resubmitJob(jobId);
        }

        if (newId.empty())
        {
            res.status = 404;
            res.set_content(R"({"error":"resubmit_failed"})", "application/json");
            return;
        }
        nlohmann::json resp = {{"status", "ok"}, {"job_id", newId}};
        res.set_content(resp.dump(), "application/json");
    });

    // POST /api/chunks/reassign -- reassign a chunk to pending or to a specific node
    m_server.Post("/api/chunks/reassign", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            int64_t chunkId = body.at("chunk_id").get<int64_t>();
            std::string targetNode = body.value("target_node", "");

            m_app->reassignChunk(chunkId, targetNode);
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/chunks/stop -- move a chunk to the terminal 'stopped'
    // state and abort whoever is rendering it (capability: chunk_stop)
    m_server.Post("/api/chunks/stop", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            int64_t chunkId = body.at("chunk_id").get<int64_t>();
            m_app->stopChunk(chunkId);
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/nodes/:id/unsuspend -- clear failure tracking for a node
    m_server.Post(R"(/api/nodes/([^/]+)/unsuspend)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        std::string nodeId = req.matches[1];
        m_app->unsuspendNode(nodeId);
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // GET /api/nodes/suspended -- list of suspended nodes with failure counts
    m_server.Get("/api/nodes/suspended", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!requireLeader(res)) return;
        auto suspended = m_app->dispatchManager().failureTracker().getSuspended();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& [nodeId, record] : suspended)
        {
            arr.push_back({
                {"node_id", nodeId},
                {"failure_count", record.failure_count},
                {"first_failure_ms", record.first_failure_ms},
                {"last_failure_ms", record.last_failure_ms},
            });
        }
        res.set_content(arr.dump(), "application/json");
    });

    // POST /api/dispatch/frame-complete -- worker reports per-frame completions
    m_server.Post("/api/dispatch/frame-complete", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            std::string nodeId = body.value("node_id", "");
            std::string jobId = body.at("job_id").get<std::string>();
            auto frames = body.at("frames").get<std::vector<int>>();
            int64_t editEpoch = body.value("edit_epoch", int64_t(-1));

            // Epoch fence: a push from before a job edit is answered
            // 200 + stale (a non-200 would make old flushers retry
            // forever). The stale verdict also tells a new worker to
            // abandon the fenced render locally.
            if (!m_app->databaseManager().isJobEpochCurrent(jobId, editEpoch))
            {
                res.set_content(R"({"status":"stale","stale":true})", "application/json");
                return;
            }

            for (int frame : frames)
            {
                FrameReport fr;
                fr.node_id = nodeId;
                fr.job_id = jobId;
                fr.frame = frame;
                fr.edit_epoch = editEpoch;
                m_app->dispatchManager().queueFrameCompletion(std::move(fr));
            }

            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/dispatch/complete -- worker reports chunk completion
    m_server.Post("/api/dispatch/complete", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            CompletionReport report;
            report.node_id = body.value("node_id", "");
            report.job_id = body.at("job_id").get<std::string>();
            report.frame_start = body.at("frame_start").get<int>();
            report.frame_end = body.at("frame_end").get<int>();
            report.elapsed_ms = body.value("elapsed_ms", int64_t(0));
            report.exit_code = body.value("exit_code", 0);
            report.edit_epoch = body.value("edit_epoch", int64_t(-1));

            // Fence pre-check (see frame-complete): stale reports get
            // 200 + stale so the sender drops them instead of retrying.
            if (!m_app->databaseManager().isChunkCurrent(
                    report.job_id, report.frame_start, report.frame_end,
                    report.node_id, report.edit_epoch))
            {
                res.set_content(R"({"status":"stale","stale":true})", "application/json");
                return;
            }

            m_app->dispatchManager().queueCompletion(std::move(report));
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/dispatch/failed -- worker reports chunk failure
    m_server.Post("/api/dispatch/failed", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!requireLeader(res)) return;

        try
        {
            auto body = nlohmann::json::parse(req.body);
            FailureReport report;
            report.node_id = body.value("node_id", "");
            report.job_id = body.at("job_id").get<std::string>();
            report.frame_start = body.at("frame_start").get<int>();
            report.frame_end = body.at("frame_end").get<int>();
            report.error = body.value("error", std::string("Unknown"));
            report.edit_epoch = body.value("edit_epoch", int64_t(-1));

            // Fence pre-check (see frame-complete): a stale failure
            // must not blacklist the node or burn a retry.
            if (!m_app->databaseManager().isChunkCurrent(
                    report.job_id, report.frame_start, report.frame_end,
                    report.node_id, report.edit_epoch))
            {
                res.set_content(R"({"status":"stale","stale":true})", "application/json");
                return;
            }

            m_app->dispatchManager().queueFailure(std::move(report));
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // ---------------------------------------------------------------
    // Tauri UI support endpoints
    // ---------------------------------------------------------------

    // GET /api/jobs/:id/task-output -- list chunk log files for a job
    m_server.Get(R"(/api/jobs/([^/]+)/task-output)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app || !m_app->isFarmRunning()) { res.status = 503; return; }

        std::string jobId = req.matches[1];
        if (!isSafePathSegment(jobId))
        {
            res.status = 400;
            res.set_content(R"({"error":"invalid_path"})", "application/json");
            return;
        }
        auto stdoutDir = m_app->farmPath() / "jobs" / jobId / "stdout";

        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::is_directory(stdoutDir, ec))
        {
            res.set_content("[]", "application/json");
            return;
        }

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& nodeEntry : fs::directory_iterator(stdoutDir, ec))
        {
            if (!nodeEntry.is_directory()) continue;
            std::string nodeId = nodeEntry.path().filename().string();

            for (const auto& fileEntry : fs::directory_iterator(nodeEntry.path(), ec))
            {
                if (!fileEntry.is_regular_file()) continue;
                std::string fname = fileEntry.path().filename().string();
                if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".log")
                    continue;

                std::string stem = fname.substr(0, fname.size() - 4);
                auto underPos = stem.rfind('_');
                if (underPos == std::string::npos) continue;

                std::string rangeStr = stem.substr(0, underPos);
                int64_t timestampMs = 0;
                try { timestampMs = std::stoll(stem.substr(underPos + 1)); }
                catch (...) { continue; }

                arr.push_back({
                    {"node_id", nodeId},
                    {"range", rangeStr},
                    {"timestamp_ms", timestampMs},
                    {"path", fileEntry.path().string()}
                });
            }
        }

        res.set_content(arr.dump(), "application/json");
    });

    // GET /api/jobs/:id/task-output/:node/:filename -- read a chunk log file
    m_server.Get(R"(/api/jobs/([^/]+)/task-output/([^/]+)/([^/]+))",
        [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app || !m_app->isFarmRunning()) { res.status = 503; return; }

        std::string jobId = req.matches[1];
        std::string nodeId = req.matches[2];
        std::string filename = req.matches[3];

        if (!isSafePathSegment(jobId) || !isSafePathSegment(nodeId) ||
            !isSafePathSegment(filename))
        {
            res.status = 400;
            res.set_content(R"({"error":"invalid_path"})", "application/json");
            return;
        }

        auto logPath = m_app->farmPath() / "jobs" / jobId / "stdout" / nodeId / filename;

        std::ifstream ifs(logPath, std::ios::in);
        if (!ifs.is_open())
        {
            res.status = 404;
            res.set_content(R"({"error":"not_found"})", "application/json");
            return;
        }

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        res.set_content(content, "text/plain");
    });

    // GET /api/nodes/:id/log -- read a remote node's monitor log
    m_server.Get(R"(/api/nodes/([^/]+)/log)", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app || !m_app->isFarmRunning()) { res.status = 503; return; }

        std::string nodeId = req.matches[1];
        if (!isSafePathSegment(nodeId))
        {
            res.status = 400;
            res.set_content(R"({"error":"invalid_path"})", "application/json");
            return;
        }

        int maxLines = 500;
        if (req.has_param("max_lines"))
        {
            try { maxLines = std::stoi(req.get_param_value("max_lines")); }
            catch (...) {}
        }

        auto lines = MonitorLog::readNodeLog(m_app->farmPath(), nodeId, maxLines);
        nlohmann::json j = lines;
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/farm/scan-cleanup -- scan for cleanup items. Implementation
    // is MonitorApp::scanFarmCleanup so the Qt FarmCleanupDialog can call
    // the same code directly without a localhost round-trip.
    m_server.Post("/api/farm/scan-cleanup", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app || !m_app->isFarmRunning()) { res.status = 503; return; }
        res.set_content(m_app->scanFarmCleanup().dump(), "application/json");
    });

    // POST /api/farm/cleanup -- execute cleanup actions on selected items.
    // Delegates to MonitorApp::executeFarmCleanup for the same reason.
    m_server.Post("/api/farm/cleanup", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app || !m_app->isFarmRunning()) { res.status = 503; return; }
        try
        {
            auto body = nlohmann::json::parse(req.body);
            std::string action = body.at("action").get<std::string>();
            auto ids = body.at("ids").get<std::vector<std::string>>();
            int count = m_app->executeFarmCleanup(action, ids);
            nlohmann::json resp = {{"status", "ok"}, {"count", count}};
            res.set_content(resp.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // GET /api/config -- return full sidecar config
    m_server.Get("/api/config", [this](const httplib::Request&, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }
        nlohmann::json j = m_app->config();
        // Also include node identity info
        j["node_id"] = m_app->identity().nodeId();
        j["hostname"] = m_app->identity().systemInfo().hostname;
        j["cpu_cores"] = m_app->identity().systemInfo().cpuCores;
        j["ram_mb"] = m_app->identity().systemInfo().ramMB;
        j["gpu_name"] = m_app->identity().systemInfo().gpuName;
        j["farm_running"] = m_app->isFarmRunning();
        j["farm_path"] = m_app->farmPath().string();
        j["farm_error"] = m_app->farmError();
        j["is_leader"] = m_app->isLeader();
        res.set_content(j.dump(), "application/json");
    });

    // POST /api/config -- update sidecar config (partial update)
    m_server.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }

        try
        {
            auto body = nlohmann::json::parse(req.body);
            auto& cfg = m_app->config();
            bool needsRestart = false;

            if (body.contains("sync_root"))
            {
                std::string newRoot = body["sync_root"].get<std::string>();
                if (newRoot != cfg.sync_root) { cfg.sync_root = newRoot; needsRestart = true; }
            }
            if (body.contains("http_port"))
            {
                uint16_t p = body["http_port"].get<uint16_t>();
                if (p != cfg.http_port) { cfg.http_port = p; needsRestart = true; }
            }
            if (body.contains("ip_override"))
            {
                std::string v = body["ip_override"].get<std::string>();
                // The advertised endpoint is fixed at farm start, so a
                // new override only reaches endpoint.json after a restart.
                if (v != cfg.ip_override) { cfg.ip_override = v; needsRestart = true; }
            }
            if (body.contains("udp_enabled"))
            {
                bool v = body["udp_enabled"].get<bool>();
                if (v != cfg.udp_enabled) { cfg.udp_enabled = v; needsRestart = true; }
            }
            if (body.contains("udp_port"))
            {
                uint16_t p = body["udp_port"].get<uint16_t>();
                if (p != cfg.udp_port) { cfg.udp_port = p; needsRestart = true; }
            }
            if (body.contains("tags"))
                cfg.tags = body["tags"].get<std::vector<std::string>>();
            if (body.contains("staging_enabled"))
                cfg.staging_enabled = body["staging_enabled"].get<bool>();
            if (body.contains("show_notifications"))
                cfg.show_notifications = body["show_notifications"].get<bool>();
            if (body.contains("path_mappings"))
                cfg.path_mappings = body["path_mappings"].get<std::vector<PathMapping>>();

            m_app->saveConfig();
            MonitorLog::instance().info("config", "Config updated via API");

            // Flag for deferred farm restart on the main thread.
            // Cannot restart here — we're on the HTTP handler thread,
            // and stopFarm() joins threads including the HTTP server.
            if (needsRestart)
                m_app->requestFarmRestart();

            nlohmann::json resp = {{"status", "ok"}, {"restarted", needsRestart}};
            res.set_content(resp.dump(), "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /api/config/path-mappings -- accept path mappings from Tauri UI
    m_server.Post("/api/config/path-mappings", [this](const httplib::Request& req, httplib::Response& res)
    {
        if (!m_app) { res.status = 503; return; }

        try
        {
            auto body = nlohmann::json::parse(req.body);
            auto mappings = body.get<std::vector<PathMapping>>();
            m_app->config().path_mappings = std::move(mappings);
            m_app->saveConfig();
            MonitorLog::instance().info("config",
                "Updated " + std::to_string(m_app->config().path_mappings.size()) + " path mapping(s)");
            res.set_content(R"({"status":"ok"})", "application/json");
        }
        catch (const std::exception& e)
        {
            res.status = 400;
            nlohmann::json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });
}

bool HttpServer::start(const std::string& bindAddress, uint16_t port)
{
    if (m_running.load())
        return true;

    setupRoutes();

    // Auth middleware. The server binds 0.0.0.0 on a farm LAN and exposes
    // endpoints that launch processes (/api/dispatch/assign, /api/node/restart,
    // /api/agent/restart), so every /api/ route requires the shared farm
    // secret. Two deliberate design points:
    //
    //  - Fail CLOSED. An empty secret (unreadable farm.json, api_secret
    //    stripped) used to skip installing this handler entirely, leaving the
    //    whole API open to the LAN. Now it denies everything instead.
    //  - Only GET /api/status is exempt, because peer discovery has to reach
    //    it before a node can know the secret is right. It reports hostname
    //    and hardware, nothing that grants control.
    //
    // /api/config used to be exempt "for the local Tauri UI"; that UI is gone
    // (the Qt AppBridge calls MonitorApp in-process) and the exemption let any
    // LAN host read the farm layout and rewrite sync_root, ports, and path
    // mappings unauthenticated. Removed — do not reintroduce a path prefix
    // exemption here without an authenticated caller that needs it.
    m_server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) -> httplib::Server::HandlerResponse
    {
        // Only protect /api/ routes
        if (req.path.rfind("/api/", 0) != 0)
            return httplib::Server::HandlerResponse::Unhandled;

        if (req.method == "GET" && req.path == "/api/status")
            return httplib::Server::HandlerResponse::Unhandled;

        if (m_apiSecret.empty())
        {
            res.status = 503;
            res.set_content(R"({"error":"auth_unavailable"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        auto it = req.headers.find("Authorization");
        if (it == req.headers.end() ||
            !constantTimeEquals(it->second, "Bearer " + m_apiSecret))
        {
            res.status = 401;
            res.set_content(R"({"error":"unauthorized"})", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    if (m_apiSecret.empty())
    {
        MonitorLog::instance().error("http",
            "No farm api_secret available — API will refuse all authenticated "
            "requests (503). Check farm.json is readable.");
    }

    // The pre-routing auth handler runs before httplib reads the body, so
    // an unauthenticated request is refused before any of it is buffered.
    // The cap bounds what an authenticated caller can make the node hold;
    // nothing legitimate is close (job manifests and frame lists).
    m_server.set_payload_max_length(16 * 1024 * 1024);

    m_port = port;

    // Try to bind before launching thread
    if (!m_server.bind_to_port(bindAddress, port))
    {
        MonitorLog::instance().error("http", "Failed to bind HTTP server to " +
            bindAddress + ":" + std::to_string(port));
        return false;
    }

    m_running.store(true);
    m_thread = std::thread([this]()
    {
        MonitorLog::instance().info("http", "HTTP server listening on port " +
            std::to_string(m_port));
        m_server.listen_after_bind();
        m_running.store(false);
    });

    return true;
}

void HttpServer::stop()
{
    if (!m_running.load())
        return;

    m_server.stop();
    if (m_thread.joinable())
        m_thread.join();

    m_running.store(false);
    MonitorLog::instance().info("http", "HTTP server stopped");
}

} // namespace MR
