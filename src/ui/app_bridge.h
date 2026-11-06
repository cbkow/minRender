#pragma once

#include "core/config.h"
#include "ui/models/chunks_model.h"
#include "ui/models/jobs_model.h"
#include "ui/models/log_model.h"
#include "ui/models/nodes_model.h"
#include "ui/models/templates_model.h"

#include <QColor>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <memory>

namespace MR {

class MonitorApp;

// Qt-facing facade for MonitorApp. Lives on the main thread, owned by
// main_qt.cpp, exposed to QML via setContextProperty("appBridge", ...).
// Read properties reflect MonitorApp state; write properties mutate
// MonitorApp::config() in-memory. Changes don't touch disk until
// saveSettings() is invoked explicitly (no auto-save — matches the plan).
class AppBridge : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool    farmRunning        READ farmRunning        NOTIFY farmRunningChanged)
    Q_PROPERTY(QColor  accentColor        READ accentColor        NOTIFY accentColorChanged)

    // Font family names resolved after QFontDatabase::addApplicationFont
    // registers the bundled .ttfs. Set once at startup, constant for the
    // life of the app.
    Q_PROPERTY(QString interFamily    READ interFamily    CONSTANT)
    Q_PROPERTY(QString monoFamily     READ monoFamily     CONSTANT)
    Q_PROPERTY(QString symbolsFamily  READ symbolsFamily  CONSTANT)
    Q_PROPERTY(QString phosphorFamily READ phosphorFamily CONSTANT)

    Q_PROPERTY(MR::JobsModel*      jobsModel      READ jobsModel      CONSTANT)
    Q_PROPERTY(MR::NodesModel*     nodesModel     READ nodesModel     CONSTANT)
    Q_PROPERTY(MR::LogModel*       logModel       READ logModel       CONSTANT)
    Q_PROPERTY(MR::ChunksModel*    chunksModel    READ chunksModel    CONSTANT)
    Q_PROPERTY(MR::TemplatesModel* templatesModel READ templatesModel CONSTANT)

    Q_PROPERTY(QString currentJobId READ currentJobId WRITE setCurrentJobId
               NOTIFY currentJobIdChanged)

    // True while a drag-reorder is in flight in the jobs panel. Suspends
    // pushing backend snapshots into JobsModel so a 2s refresh can't
    // reset the ListView mid-drag. Row data may go briefly stale; the
    // first refresh after release catches up.
    Q_PROPERTY(bool jobsRefreshPaused READ jobsRefreshPaused
               WRITE setJobsRefreshPaused NOTIFY jobsRefreshPausedChanged)

    // True while the JobDetailPanel should show the submission form.
    // Set by New Job actions; cleared by SubmissionForm's submit/cancel
    // or by selecting an existing job.
    Q_PROPERTY(bool submissionMode READ submissionMode WRITE setSubmissionMode
               NOTIFY submissionModeChanged)
    Q_PROPERTY(QString editJobId READ editJobId NOTIFY editJobChanged)
    Q_PROPERTY(QVariantMap editSeed READ editSeed NOTIFY editJobChanged)

    // Full snapshot of the currently-selected job as a QVariantMap:
    // {jobId, name, state, progress, totalChunks, doneChunks,
    //  failedChunks, renderingChunks, priority, createdAt}. Empty map
    // when nothing is selected or the jobId no longer exists in the
    // cache. Emits on every jobs refresh so the JobDetailPanel can
    // re-bind without manual model lookups.
    Q_PROPERTY(QVariantMap currentJob READ currentJob NOTIFY currentJobChanged)

    // Log viewer source selection. Values:
    //   ""           — local MonitorLog ring buffer (default)
    //   "__task__"   — per-chunk stdout for the selected job
    //   <nodeId>     — that peer's log file on the shared farm
    // Refreshed on a 3 s timer while non-empty.
    Q_PROPERTY(QString      logSourceId     READ logSourceId     WRITE setLogSourceId NOTIFY logSourceIdChanged)
    Q_PROPERTY(QStringList  remoteLogLines  READ remoteLogLines  NOTIFY remoteLogLinesChanged)
    // Dropdown model for the log viewer. Shape:
    //   [{id:"",         label:"Monitor Log"},
    //    {id:"__task__", label:"Task Output"},
    //    {id:<selfId>,   label:"<host> (this node)"},
    //    {id:<peerId>,   label:"<hostname>"}, …]
    // Rebuilt in refresh() when the peer set changes.
    Q_PROPERTY(QVariantList logSources      READ logSources      NOTIFY logSourcesChanged)

    // Task Output state — only meaningful when logSourceId == "__task__"
    // and currentJobId is set. taskOutputChunks is a list of chunk log
    // file descriptors (one entry per {nodeId, chunk-range, timestamp});
    // selecting one via selectedTaskChunkIndex populates taskOutputLines
    // with the file's contents.
    Q_PROPERTY(QVariantList taskOutputChunks       READ taskOutputChunks       NOTIFY taskOutputChunksChanged)
    Q_PROPERTY(int          selectedTaskChunkIndex READ selectedTaskChunkIndex WRITE setSelectedTaskChunkIndex NOTIFY selectedTaskChunkIndexChanged)
    Q_PROPERTY(QStringList  taskOutputLines        READ taskOutputLines        NOTIFY taskOutputLinesChanged)

    // "This Node" descriptors — stable for the life of the process except
    // for isLeader and nodeState, which flip with leadership / tray toggles.
    Q_PROPERTY(QString thisNodeId       READ thisNodeId       CONSTANT)
    Q_PROPERTY(QString thisNodeHostname READ thisNodeHostname CONSTANT)
    Q_PROPERTY(QString thisNodeGpu      READ thisNodeGpu      CONSTANT)
    Q_PROPERTY(int     thisNodeCpuCores READ thisNodeCpuCores CONSTANT)
    Q_PROPERTY(qint64  thisNodeRamMb    READ thisNodeRamMb    CONSTANT)
    Q_PROPERTY(bool    thisNodeIsLeader READ thisNodeIsLeader NOTIFY thisNodeIsLeaderChanged)
    Q_PROPERTY(bool    thisNodeActive   READ thisNodeActive   NOTIFY thisNodeActiveChanged)
    // Leader-assigned suspension for THIS node. Mirrored from the leader
    // by MonitorApp so a benched node can see and clear its own state
    // instead of needing someone at another machine to do it.
    Q_PROPERTY(bool    thisNodeSuspended       READ thisNodeSuspended       NOTIFY thisNodeSuspensionChanged)
    Q_PROPERTY(int     thisNodeSuspendFailures READ thisNodeSuspendFailures NOTIFY thisNodeSuspensionChanged)

    Q_PROPERTY(QString syncRoot           READ syncRoot           WRITE setSyncRoot           NOTIFY syncRootChanged)
    Q_PROPERTY(QString tagsCsv            READ tagsCsv            WRITE setTagsCsv            NOTIFY tagsCsvChanged)
    Q_PROPERTY(int     httpPort           READ httpPort           WRITE setHttpPort           NOTIFY httpPortChanged)
    Q_PROPERTY(QString ipOverride         READ ipOverride         WRITE setIpOverride         NOTIFY ipOverrideChanged)
    Q_PROPERTY(bool    udpEnabled         READ udpEnabled         WRITE setUdpEnabled         NOTIFY udpEnabledChanged)
    Q_PROPERTY(int     udpPort            READ udpPort            WRITE setUdpPort            NOTIFY udpPortChanged)
    Q_PROPERTY(bool    showNotifications  READ showNotifications  WRITE setShowNotifications  NOTIFY showNotificationsChanged)
    Q_PROPERTY(bool    stagingEnabled     READ stagingEnabled     WRITE setStagingEnabled     NOTIFY stagingEnabledChanged)

    // Farm secret fingerprint (see core/sha256.h) plus the count of live
    // peers reporting a different one. A non-zero mismatch count is what
    // turns an opaque wall of 401s into "that node has the wrong secret".
    Q_PROPERTY(QString farmSecretFingerprint READ farmSecretFingerprint NOTIFY farmSecretFingerprintChanged)
    Q_PROPERTY(int     secretMismatchCount   READ secretMismatchCount   NOTIFY secretMismatchCountChanged)

public:
    explicit AppBridge(MonitorApp* monitor, QObject* parent = nullptr);
    ~AppBridge() override;

    bool farmRunning() const;
    QColor accentColor() const { return m_accentColor; }

    QString interFamily()    const { return m_interFamily; }
    QString monoFamily()     const { return m_monoFamily; }
    QString symbolsFamily()  const { return m_symbolsFamily; }
    QString phosphorFamily() const { return m_phosphorFamily; }

    // main_qt.cpp calls these after QFontDatabase::addApplicationFont so
    // the resolved family names (which may differ slightly from the file
    // name — e.g. "Inter" vs "Inter_18pt") are pinned at a single place.
    void setInterFamily(const QString& v)    { m_interFamily    = v; }
    void setMonoFamily(const QString& v)     { m_monoFamily     = v; }
    void setSymbolsFamily(const QString& v)  { m_symbolsFamily  = v; }
    void setPhosphorFamily(const QString& v) { m_phosphorFamily = v; }

    JobsModel*      jobsModel()      const { return m_jobsModel.get(); }
    NodesModel*     nodesModel()     const { return m_nodesModel.get(); }
    LogModel*       logModel()       const { return m_logModel.get(); }
    ChunksModel*    chunksModel()    const { return m_chunksModel.get(); }
    TemplatesModel* templatesModel() const { return m_templatesModel.get(); }

    QString currentJobId() const { return m_currentJobId; }
    void setCurrentJobId(const QString& jobId);

    bool submissionMode() const { return m_submissionMode; }
    QString editJobId() const { return m_editJobId; }
    QVariantMap editSeed() const { return m_editSeed; }
    void setSubmissionMode(bool on);

    bool jobsRefreshPaused() const { return m_jobsRefreshPaused; }
    void setJobsRefreshPaused(bool paused);

    QVariantMap currentJob() const;

    QString     logSourceId()   const { return m_logSourceId; }
    void        setLogSourceId(const QString& id);
    QStringList remoteLogLines() const { return m_remoteLogLines; }
    QVariantList logSources()   const { return m_logSources; }

    QVariantList taskOutputChunks()        const { return m_taskOutputChunks; }
    int          selectedTaskChunkIndex()  const { return m_selectedTaskChunkIndex; }
    void         setSelectedTaskChunkIndex(int i);
    QStringList  taskOutputLines()         const { return m_taskOutputLines; }

    // Reveal the currently-viewed log's file in Finder/Explorer — the
    // selected task chunk's stdout log, a peer's monitor-<date>.log, or
    // our own (all live on the farm share). jobActionFailed toast when
    // no file exists for the current source.
    Q_INVOKABLE void revealCurrentLog();

    QString thisNodeId() const;
    QString thisNodeHostname() const;
    QString thisNodeGpu() const;
    int     thisNodeCpuCores() const;
    qint64  thisNodeRamMb() const;
    bool    thisNodeIsLeader() const;
    bool    thisNodeActive() const;
    bool    thisNodeSuspended() const;
    int     thisNodeSuspendFailures() const;

    Q_INVOKABLE void toggleNodeActive();
    Q_INVOKABLE void unsuspendNode(const QString& nodeId);
    // Self-service: clear this node's own suspension (proxied to the
    // leader when this node isn't it).
    Q_INVOKABLE void unsuspendThisNode();

    // Peer remote-control actions — forwarded to MonitorApp which
    // queues HTTP to the peer's endpoint (no blocking).
    Q_INVOKABLE void setPeerNodeActive(const QString& nodeId, bool active);
    Q_INVOKABLE void restartPeerApp(const QString& nodeId);
    Q_INVOKABLE void writePeerRestartSignal(const QString& nodeId);
    Q_INVOKABLE void forgetPeer(const QString& nodeId);

    // Farm cleanup — async: the scan walks the farm share (seconds on a
    // slow SMB link), so both run on a worker thread and report back
    // via farmCleanupScanReady with MonitorApp::scanFarmCleanup's JSON
    // shape (six item lists the dialog renders with checkboxes).
    // requestFarmCleanupAction applies one of {archive, delete_jobs,
    // delete_dirs, remove_peers} to the selected ids, then rescans in
    // the same pass. cleanupBusy gates the dialog's buttons meanwhile.
    Q_INVOKABLE void requestFarmCleanupScan();
    Q_INVOKABLE void requestFarmCleanupAction(const QString& action,
                                              const QStringList& ids);
    Q_PROPERTY(bool cleanupBusy READ cleanupBusy NOTIFY cleanupBusyChanged)
    bool cleanupBusy() const { return m_cleanupBusy; }

    // Job controls — forwarded to MonitorApp. Each takes the slug /
    // job_id exposed by JobsModel. No-op when jobId is empty.
    Q_INVOKABLE void pauseJob(const QString& jobId);
    Q_INVOKABLE void resumeJob(const QString& jobId);
    Q_INVOKABLE void cancelJob(const QString& jobId);
    Q_INVOKABLE void deleteJob(const QString& jobId);
    Q_INVOKABLE void requeueJob(const QString& jobId);
    Q_INVOKABLE void archiveJob(const QString& jobId);
    Q_INVOKABLE void retryFailedChunks(const QString& jobId);

    // Dispatch-order controls. moveJob reorders jobId next to targetId;
    // when their priorities differ the moved job adopts the target's, so a
    // drag can cross group boundaries. setJobPriority sets it directly.
    Q_INVOKABLE void moveJob(const QString& jobId, const QString& targetId, bool before);
    Q_INVOKABLE void setJobPriority(const QString& jobId, int priority);

    // Reveals the job's output_dir in the platform file manager. The
    // manifest stores paths in canonical Windows form; on macOS we
    // translate to /Volumes/... via MR::fromCanonicalPath before handing
    // off to MR::openFolderInExplorer. No-op (and a log warning) if the
    // job has no output_dir or the resolved path doesn't exist locally
    // — typical when path mappings aren't configured for this host.
    Q_INVOKABLE void openJobOutput(const QString& jobId);

    Q_INVOKABLE void requestSubmissionMode();

    // Chunk-level actions on the currently-selected job.
    // reassignChunk: targetNodeId="" lets the dispatcher pick. Also the
    //   resume path for stopped chunks (back to pending).
    // stopChunk: terminal 'stopped' state + abort push to the renderer;
    //   frames leave the grid/progress, job can complete "partial".
    // resubmitChunkAsJob: returns the new job's slug ("" on failure)
    //   so the caller can switch the JobDetail to it.
    Q_INVOKABLE void reassignChunk(qint64 chunkId,
                                   const QString& targetNodeId = QString());
    Q_INVOKABLE void stopChunk(qint64 chunkId);
    Q_PROPERTY(bool leaderSupportsChunkStop READ leaderSupportsChunkStop
               NOTIFY leaderSupportsChunkStopChanged)
    bool leaderSupportsChunkStop() const;
    Q_INVOKABLE QString resubmitChunkAsJob(const QString& jobId,
                                           int frameStart, int frameEnd,
                                           int chunkSize);
    // Opens the submission form seeded from jobId's stored manifest with
    // the frame range preset to the chunk — "edit, then submit as a new
    // job". Same async fetch/failure surface as openJobEditor.
    Q_INVOKABLE void openChunkResubmitEditor(const QString& jobId,
                                             int frameStart, int frameEnd);
    // Resubmit-mode submit for template-gone jobs: mutates the seed's
    // stored manifest (flags 1:1, like applyJobEdit's raw path) into a
    // new job. Template-found resubmits go through plain submitJob.
    Q_INVOKABLE void submitManifestResubmit(const QString& jobName,
                                            const QStringList& flagValues,
                                            int frameStart, int frameEnd,
                                            int chunkSize, int priority);

    // Returns a QVariantMap describing a template by id: {name, templateId,
    // dcc, path, frameStart, frameEnd, chunkSize, maxRetries, flags:[…]}
    // where each flag is a map of {flag, value, info, help, editable,
    // required, type, filter, id}. Empty map if not found. Used by the
    // submission form to render per-template dynamic inputs.
    Q_INVOKABLE QVariantMap templateById(const QString& templateId) const;

    // Bake a manifest + submit. On leader the submission is queued
    // directly on the DispatchManager; on workers it goes out via
    // postToLeaderAsync. Either way, one of submissionSucceeded /
    // submissionFailed eventually fires on the UI thread.
    Q_INVOKABLE void submitJob(const QString& templateId,
                               const QString& jobName,
                               const QStringList& flagValues,
                               int frameStart, int frameEnd,
                               int chunkSize, int priority);
    // True while a submission round-trips to the leader — the form's
    // Submit button gates on it (same pattern as editBusy).
    Q_PROPERTY(bool submitBusy READ submitBusy NOTIFY submitBusyChanged)
    bool submitBusy() const { return m_submitBusy; }

    // --- Job editing ---
    // openJobEditor fetches the job's manifest off the UI thread (HTTP
    // on workers), resolves its template, and exposes an edit seed for
    // the form via editSeed/editJobId; fetch failures surface through
    // jobActionFailed. applyJobEdit re-bakes (or, when the template is
    // gone, mutates) the manifest and hands it to MonitorApp::editJob
    // with the chosen mode: "continue" | "restart" | "startover".
    // editApplied fires only after the leader acknowledges the edit;
    // submissionFailed carries validation AND leader errors (reusing
    // the form's error banner — the dialog stays open on failure).
    // editBusy is true while an apply round-trip is in flight.
    // --- Frame preview ---
    // previewSupported: compiled with the vendored image libs?
    // jobPreviewInfo: resolve the selected job's newest rendered frame
    // to a local file ({found, file, frame, isExr}) by globbing the
    // path-mapped output_dir against the output flag's padded pattern.
    // exrLayers: layer names for the contact sheet.
    Q_PROPERTY(bool previewSupported READ previewSupported CONSTANT)
    bool previewSupported() const;
    Q_INVOKABLE QVariantMap jobPreviewInfo(const QString& jobId);
    // Same resolution restricted to a frame range (frame pin: fs==fe,
    // chunk pin: chunk bounds). Needs a #-padded output pattern; jobs
    // without one can only follow "latest".
    Q_INVOKABLE QVariantMap rangePreviewInfo(const QString& jobId,
                                             int frameStart, int frameEnd);
    Q_INVOKABLE QStringList exrLayers(const QString& filePath);

    // --- Preview pin ---
    // The grid / chunk table (Job Detail) pin the preview (Node panel)
    // through here. pinPreview refuses — returns false, state unchanged
    // — when no file for the range exists on disk yet (not rendered, or
    // rendered but still in a node's local staging). -1/-1 = follow
    // latest. Cleared on job switch, by the preview's Latest button, and
    // by its 60 s auto-return timer.
    Q_PROPERTY(int previewPinStart READ previewPinStart NOTIFY previewPinChanged)
    Q_PROPERTY(int previewPinEnd   READ previewPinEnd   NOTIFY previewPinChanged)
    int previewPinStart() const { return m_previewPinStart; }
    int previewPinEnd() const { return m_previewPinEnd; }
    Q_INVOKABLE bool pinPreview(int frameStart, int frameEnd);
    Q_INVOKABLE void clearPreviewPin();

    Q_INVOKABLE void openJobEditor(const QString& jobId);
    Q_INVOKABLE void closeJobEditor();
    Q_INVOKABLE void applyJobEdit(const QStringList& flagValues,
                                  int frameStart, int frameEnd,
                                  int chunkSize, int priority,
                                  int maxRetries, int timeoutSeconds,
                                  const QString& mode);
    // False while the leader runs a build without the "job_edit"
    // capability (or no leader is reachable) — Edit buttons gate on it.
    Q_PROPERTY(bool leaderSupportsJobEdit READ leaderSupportsJobEdit
               NOTIFY leaderSupportsJobEditChanged)
    bool leaderSupportsJobEdit() const;
    Q_PROPERTY(bool editBusy READ editBusy NOTIFY editBusyChanged)
    bool editBusy() const { return m_editBusy; }

    QString syncRoot() const;
    void setSyncRoot(const QString& v);

    QString tagsCsv() const;
    void setTagsCsv(const QString& v);

    int httpPort() const;
    void setHttpPort(int v);

    QString ipOverride() const;
    void setIpOverride(const QString& v);

    bool udpEnabled() const;
    void setUdpEnabled(bool v);

    int udpPort() const;
    void setUdpPort(int v);

    bool showNotifications() const;
    void setShowNotifications(bool v);

    bool stagingEnabled() const;
    void setStagingEnabled(bool v);

    QString farmSecretFingerprint() const;
    int secretMismatchCount() const { return m_secretMismatchCount; }

    // Polled from main_qt.cpp's 50 ms tick. Emits *Changed signals only
    // when the underlying MonitorApp state actually shifted.
    void refresh();

    // Revert all in-memory edits back to what's currently on disk. Used
    // by the Settings panel's Cancel button; a no-op after saveSettings.
    Q_INVOKABLE void revertSettings();

    Q_INVOKABLE void saveSettings();
    Q_INVOKABLE void requestRestart();

    // Convert a file:// QUrl (as produced by FolderDialog / FileDialog) to
    // the platform-native path. QUrl::toLocalFile() handles UNC shares
    // (file://server/share/path → \\server\share\path), Windows drive
    // letters, and percent-encoded characters — all things naive string
    // stripping in QML gets wrong.
    Q_INVOKABLE QString urlToLocalPath(const QUrl& url) const;
    // A local path → file URL for QML pickers (min-paths: percent-encoded; `file:///C:/…`, `file://host/share/…`,
    // `file:///…` — the QML seed used to build "file:///" + path with no UNC, POSIX or encoding handling).
    Q_INVOKABLE QString localPathToUrl(const QString& path) const;

    // Whether the current config.sync_root resolves to an existing
    // directory. Lets the Settings panel tell the user their path is
    // broken before they hit Save.
    Q_INVOKABLE bool syncRootIsValid() const;

    // Path mapping accessors for the Path Mappings dialog. Returns/accepts
    // a JSON array of {win, mac, lin, enabled, label} entries. Mutates
    // MonitorApp::config().path_mappings in-memory and persists to disk
    // via saveConfig() (mappings are infrequently edited; no separate
    // dirty flag like the main settings dialog uses).
    Q_INVOKABLE QString pathMappingsJson() const;
    Q_INVOKABLE void    setPathMappings(const QString& json);

signals:
    void farmRunningChanged();
    void accentColorChanged();
    void syncRootChanged();
    void tagsCsvChanged();
    void httpPortChanged();
    void ipOverrideChanged();
    void udpEnabledChanged();
    void udpPortChanged();
    void showNotificationsChanged();
    void stagingEnabledChanged();
    void farmSecretFingerprintChanged();
    void secretMismatchCountChanged();
    void currentJobIdChanged();
    void submissionModeChanged();
    void jobsRefreshPausedChanged();
    void currentJobChanged();
    void thisNodeIsLeaderChanged();
    void thisNodeActiveChanged();
    void thisNodeSuspensionChanged();
    void logSourceIdChanged();
    void remoteLogLinesChanged();
    void logSourcesChanged();
    void taskOutputChunksChanged();
    void selectedTaskChunkIndexChanged();
    void taskOutputLinesChanged();

    void submissionSucceeded(const QString& jobId);
    void submissionFailed(const QString& reason);
    void editApplied(const QString& jobId);
    void editJobChanged();
    void editBusyChanged();
    void submitBusyChanged();
    void leaderSupportsJobEditChanged();
    void leaderSupportsChunkStopChanged();
    // Transient failure of a job action outside a dialog (e.g. the
    // Edit button's manifest fetch) — surfaced as a toast in Main.qml.
    void jobActionFailed(const QString& reason);
    // Edit accepted onto the farm-share command queue (leader was
    // unreachable) — informational toast, distinct from editApplied.
    void editQueued(const QString& message);
    void cleanupBusyChanged();
    // Fired on the UI thread when an async cleanup scan (or an action's
    // follow-up rescan) completes; results uses scanFarmCleanup's shape.
    void farmCleanupScanReady(const QVariantMap& results);
    void previewPinChanged();

    void pathMappingsChanged();

private:
    QVariantMap previewInfoForRange(const QString& jobId,
                                    int frameStart, int frameEnd);

    // Job-edit async plumbing. finishOpenJobEditor/finishApplyJobEdit
    // run on the UI thread after the manifest fetch thread completes.
    // resubmit=true seeds the form for "submit chunk as new job":
    // frame range overridden to the chunk, name editable, and the raw
    // manifest stashed in the seed for the template-gone submit path.
    void finishOpenJobEditor(const QString& jobId,
                             const std::string& manifestJson,
                             bool resubmit = false,
                             int frameStartOverride = -1,
                             int frameEndOverride = -1);
    void finishApplyJobEdit(const std::string& storedJson,
                            const std::string& fetchError,
                            const QStringList& flagValues,
                            int frameStart, int frameEnd,
                            int chunkSize, int priority,
                            int maxRetries, int timeoutSeconds,
                            const QString& mode);
    void setEditBusy(bool busy);
    bool m_editBusy = false;
    bool m_editorOpening = false;
    bool m_lastLeaderSupportsJobEdit = false;
    bool m_lastLeaderSupportsChunkStop = false;

    void setCleanupBusy(bool busy);
    bool m_cleanupBusy = false;

    void setSubmitBusy(bool busy);
    bool m_submitBusy = false;

    // Snapshot of MonitorApp::config() taken at construction and after each
    // saveSettings(). revertSettings() copies this back, emitting *Changed
    // for every property so bound QML rebinds.
    void takeSnapshot();
    void emitAllSettingsChanged();

    MonitorApp* m_monitor;
    QColor m_accentColor;
    QString m_interFamily    = QStringLiteral("sans-serif");
    QString m_monoFamily     = QStringLiteral("monospace");
    QString m_symbolsFamily  = QStringLiteral("Segoe UI Symbol");
    QString m_phosphorFamily = QStringLiteral("Phosphor");
    Config m_snapshot;
    // Drives the 3 s ChunksModel refresh while m_currentJobId is set.
    void refreshChunks();
    // Drives the 3 s log refresh while m_logSourceId is non-empty.
    // Dispatches to refreshRemoteLog or refreshTaskOutput based on the
    // sentinel value of logSourceId.
    void refreshLogSource();
    void refreshRemoteLog();
    void refreshTaskOutput();
    void reloadSelectedTaskChunk();
    // Rebuild m_logSources from MonitorApp's peer snapshot. Called in
    // refresh(); no-op when the snapshot's peer-id/hostname list hasn't
    // changed since the last build.
    void rebuildLogSources();

    std::unique_ptr<JobsModel>      m_jobsModel;
    std::unique_ptr<NodesModel>     m_nodesModel;
    std::unique_ptr<LogModel>       m_logModel;
    std::unique_ptr<ChunksModel>    m_chunksModel;
    std::unique_ptr<TemplatesModel> m_templatesModel;
    QTimer                          m_chunksTimer;
    QString                         m_currentJobId;

    // Log viewer source + fetch state
    QTimer                          m_remoteLogTimer;   // ticks at 3 s while logSourceId non-empty
    QString                         m_logSourceId;      // "" = local, "__task__" = job logs, else peer id
    QStringList                     m_remoteLogLines;   // remote-peer mode buffer
    QVariantList                    m_logSources;       // ComboBox model
    QVariantList                    m_taskOutputChunks; // task mode chunk list
    int                             m_selectedTaskChunkIndex = -1;
    QStringList                     m_taskOutputLines;  // task mode selected chunk contents

    bool m_submissionMode   = false;
    QString m_editJobId;
    QVariantMap m_editSeed;
    int m_previewPinStart = -1;
    int m_previewPinEnd   = -1;
    bool m_jobsRefreshPaused = false;
    bool m_lastFarmRunning  = false;
    bool m_lastIsLeader     = false;
    bool m_lastNodeActive   = true;
    bool m_lastThisNodeSuspended = false;
    int  m_lastThisNodeSuspendFailures = 0;
    QString m_lastSecretFingerprint;
    int     m_secretMismatchCount = 0;
};

} // namespace MR
