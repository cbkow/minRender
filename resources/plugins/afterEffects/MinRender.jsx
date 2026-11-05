// MinRender.jsx — Farm submitter for After Effects (version 0.5.11, tracks the minRender app)
// Install to: ScriptUI Panels folder for dockable panel, or run via File > Scripts
//
// Reads queued render queue items and submits each as a MinRender job
// via the local submissions dropbox. The running MinRender Monitor picks
// up submissions and routes them to the leader.

(function (thisObj) {

    var SCRIPT_NAME = "MinRender";
    // Tracks the minRender app version. 0.5.8: reads the data folder from
    // %USERPROFILE%\minRender (falls back to the pre-0.5.8 AppData folder).
    var SCRIPT_VERSION = "0.5.11";

    // -- Shared state ---------------------------------------------------------

    var items = [];
    var submissionsDir = "";
    var projectPath = "";

    // UI references (assigned in buildUI)
    var rootPanel = null;
    var statusText = null;
    var itemsGroup = null;
    var chunkInput = null;
    var priorityInput = null;
    var rsDropdown = null;
    var omDropdown = null;
    var submitBtn = null;

    // -- Helpers --------------------------------------------------------------

    function readTextFile(path) {
        var f = new File(path);
        if (!f.exists) return null;
        f.open("r");
        f.encoding = "UTF-8";
        var text = f.read();
        f.close();
        return text;
    }

    function padTimestamp(ts) {
        var s = ts.toString();
        while (s.length < 13) s = "0" + s;
        return s;
    }

    function isVideoFormat(path) {
        return /\.(mov|mp4|avi|mkv|wmv|m4v|webm|mxf)$/i.test(path);
    }

    function getHostname() {
        try {
            return system.callSystem("hostname").replace(/[\r\n]/g, "");
        } catch (e) {
            return "unknown";
        }
    }

    function toJSON(obj) {
        if (obj === null) return "null";
        if (typeof obj === "undefined") return "null";
        if (typeof obj === "number" || typeof obj === "boolean") return String(obj);
        if (typeof obj === "string") {
            return '"' + obj.replace(/\\/g, "\\\\").replace(/"/g, '\\"')
                             .replace(/\n/g, "\\n").replace(/\r/g, "\\r") + '"';
        }
        if (obj instanceof Array) {
            var arrParts = [];
            for (var i = 0; i < obj.length; i++) arrParts.push(toJSON(obj[i]));
            return "[" + arrParts.join(",") + "]";
        }
        if (typeof obj === "object") {
            var objParts = [];
            for (var key in obj) {
                if (obj.hasOwnProperty(key)) {
                    objParts.push(toJSON(key) + ":" + toJSON(obj[key]));
                }
            }
            return "{" + objParts.join(",") + "}";
        }
        return "null";
    }

    function refreshLayout() {
        rootPanel.layout.layout(true);
        if (rootPanel instanceof Window) rootPanel.layout.resize();
    }

    function setStatus(msg) {
        statusText.text = msg;
    }

    // -- Get local submissions directory --------------------------------------

    // -- minRender's data folder ----------------------------------------------
    // Windows, minRender 0.5.8+: %USERPROFILE%\minRender (a folder outside
    // AppData, so the Microsoft Store build and this script see the same
    // files). Falls back to the pre-0.5.8 %LOCALAPPDATA%\MinRender while an
    // older app version is installed. macOS: ~/Library/Application Support.
    function getDataDir() {
        if ($.os.indexOf("Windows") !== -1) {
            var newDir = Folder("~").fsName + "\\minRender";
            var oldDir = Folder.userData.parent.fsName + "\\Local\\MinRender";
            if (new File(newDir + "\\config.json").exists || !new Folder(oldDir).exists)
                return newDir;
            return oldDir;
        }
        return Folder.userData.fsName + "/MinRender";
    }

    function getSubmissionsDir() {
        var subDir = getDataDir() + ($.os.indexOf("Windows") !== -1 ? "\\submissions" : "/submissions");
        var folder = new Folder(subDir);
        if (!folder.exists) folder.create();
        return subDir;
    }

    // -- Scan render queue ----------------------------------------------------

    function scanRenderQueue() {
        items = [];

        // Clear previous item checkboxes
        while (itemsGroup.children.length > 0) {
            itemsGroup.remove(itemsGroup.children[0]);
        }

        // -- Find MinRender config --------------------------------------------

        var configPath = getDataDir() + ($.os.indexOf("Windows") !== -1 ? "\\config.json" : "/config.json");

        var configText = readTextFile(configPath);
        if (!configText) {
            setStatus("Config not found. Is MinRender installed?");
            submitBtn.enabled = false;
            refreshLayout();
            return;
        }

        var config;
        try {
            config = eval("(" + configText + ")");
        } catch (e) {
            setStatus("Failed to parse config.");
            submitBtn.enabled = false;
            refreshLayout();
            return;
        }

        var syncRoot = config.sync_root;
        if (!syncRoot) {
            setStatus("Sync root not configured in MinRender.");
            submitBtn.enabled = false;
            refreshLayout();
            return;
        }

        // Check farm exists (for template scanning validation)
        var farmDir = new Folder(syncRoot + "/MinRender-v2");
        if (!farmDir.exists) {
            setStatus("Farm not initialized.");
            submitBtn.enabled = false;
            refreshLayout();
            return;
        }

        // Set local submissions directory
        submissionsDir = getSubmissionsDir();

        // -- Validate project -------------------------------------------------

        if (!app.project.file) {
            setStatus("Save the project first.");
            submitBtn.enabled = false;
            refreshLayout();
            return;
        }

        projectPath = app.project.file.fsName;

        // -- Collect render queue items ---------------------------------------

        var rq = app.project.renderQueue;

        for (var i = 1; i <= rq.numItems; i++) {
            var rqi = rq.item(i);
            if (rqi.status !== RQItemStatus.QUEUED) continue;

            var comp = rqi.comp;
            var fps = comp.frameRate;

            var startFrame = Math.round(rqi.timeSpanStart * fps);
            var endFrame = Math.round((rqi.timeSpanStart + rqi.timeSpanDuration) * fps) - 1;
            if (endFrame < startFrame) endFrame = startFrame;

            for (var j = 1; j <= rqi.numOutputModules; j++) {
                var om = rqi.outputModule(j);
                var outputFile = om.file;
                if (!outputFile) continue;

                var outputPath = outputFile.fsName;
                var video = isVideoFormat(outputPath);

                items.push({
                    rqIndex: i,
                    omIndex: j,
                    compName: comp.name,
                    startFrame: startFrame,
                    endFrame: endFrame,
                    outputPath: outputPath,
                    isVideo: video,
                    totalFrames: endFrame - startFrame + 1
                });
            }
        }

        if (items.length === 0) {
            setStatus("No queued items in render queue.");
            submitBtn.enabled = false;
            refreshLayout();
            return;
        }

        // -- Populate item checkboxes -----------------------------------------

        for (var k = 0; k < items.length; k++) {
            var item = items[k];
            var label = item.compName;
            if (item.isVideo) label += " [VIDEO]";
            label += "  (" + item.startFrame + "-" + item.endFrame + ")";

            var cb = itemsGroup.add("checkbox", undefined, label);
            cb.value = true;
        }

        // -- Populate RS/OM template dropdowns --------------------------------

        rsDropdown.removeAll();
        rsDropdown.add("item", "(Render Queue)");
        omDropdown.removeAll();
        omDropdown.add("item", "(Render Queue)");

        try {
            var firstRqi = rq.item(items[0].rqIndex);
            var tpl = firstRqi.templates;
            if (tpl) for (var t = 0; t < tpl.length; t++) rsDropdown.add("item", tpl[t]);
        } catch (e) {}
        try {
            var firstOm = rq.item(items[0].rqIndex).outputModule(items[0].omIndex);
            var tpl2 = firstOm.templates;
            if (tpl2) for (var t2 = 0; t2 < tpl2.length; t2++) omDropdown.add("item", tpl2[t2]);
        } catch (e) {}

        rsDropdown.selection = 0;
        omDropdown.selection = 0;

        setStatus(items.length + " queued item" + (items.length > 1 ? "s" : "") + " found");
        submitBtn.enabled = true;
        refreshLayout();
    }

    // -- Submit jobs ----------------------------------------------------------

    function submitJobs() {
        if (items.length === 0) return;

        var chunkSize = parseInt(chunkInput.text, 10);
        if (isNaN(chunkSize) || chunkSize < 1) {
            alert("Chunk size must be a positive number.", SCRIPT_NAME);
            return;
        }

        var priority = parseInt(priorityInput.text, 10);
        if (isNaN(priority)) priority = 50;

        var rsTemplate = (rsDropdown.selection && rsDropdown.selection.index > 0) ? rsDropdown.selection.text : "";
        var omTemplate = (omDropdown.selection && omDropdown.selection.index > 0) ? omDropdown.selection.text : "";

        var hostname = getHostname();
        var submitted = 0;

        app.project.save();

        var cbs = itemsGroup.children;

        for (var i = 0; i < items.length; i++) {
            if (!cbs[i].value) continue;

            var item = items[i];
            var itemChunkSize = item.isVideo ? item.totalFrames : chunkSize;

            var projectName = app.project.file.name.replace(/\.[^.]+$/, "");
            var jobName = projectName + " - " + item.compName;
            if (item.omIndex > 1) jobName += " (OM" + item.omIndex + ")";

            var overrides = {
                "project_file": projectPath,
                "comp_name": item.compName,
                "output_path": item.outputPath
            };
            if (rsTemplate) overrides["rs_template"] = rsTemplate;
            if (omTemplate) overrides["om_template"] = omTemplate;

            var ts = new Date().getTime();
            var submission = {
                "_version": 1,
                "template_id": "after-effects-plugin",
                "job_name": jobName,
                "submitted_by_host": hostname,
                "submitted_at_ms": ts,
                "overrides": overrides,
                "frame_start": item.startFrame,
                "frame_end": item.endFrame,
                "chunk_size": itemChunkSize,
                "priority": priority
            };

            var json;
            if (typeof JSON !== "undefined") {
                json = JSON.stringify(submission, null, 2);
            } else {
                json = toJSON(submission);
            }

            var filename = padTimestamp(ts + i) + "." + hostname + ".json";
            var filePath = submissionsDir + "/" + filename;

            try {
                var outFile = new File(filePath);
                outFile.open("w");
                outFile.encoding = "UTF-8";
                outFile.write(json);
                outFile.close();
                submitted++;
            } catch (e) {
                alert("Failed to write submission:\n" + filePath + "\n\n" + e.message,
                      SCRIPT_NAME);
            }
        }

        if (submitted > 0) {
            setStatus("Submitted " + submitted + " job" + (submitted > 1 ? "s" : ""));

            // Clear items after successful submit
            items = [];
            while (itemsGroup.children.length > 0) {
                itemsGroup.remove(itemsGroup.children[0]);
            }
            submitBtn.enabled = false;
            refreshLayout();
        }
    }

    // -- Build UI -------------------------------------------------------------

    function buildUI(thisObj) {
        var panel = (thisObj instanceof Panel) ? thisObj : new Window("palette", SCRIPT_NAME + " " + SCRIPT_VERSION, undefined, { resizeable: true });
        if (panel === null) return null;

        rootPanel = panel;

        panel.orientation = "column";
        panel.alignChildren = ["fill", "top"];
        panel.margins = [10, 10, 10, 10];
        panel.spacing = 6;

        // Scan button
        var scanBtn = panel.add("button", undefined, "Scan Render Queue");
        scanBtn.helpTip = "Read queued render queue items from the current project";
        scanBtn.onClick = scanRenderQueue;

        // Status line
        statusText = panel.add("statictext", undefined, "Click Scan to read render queue");

        // Items group (dynamically populated by scan)
        itemsGroup = panel.add("group");
        itemsGroup.orientation = "column";
        itemsGroup.alignChildren = ["fill", "top"];
        itemsGroup.spacing = 2;

        // Settings
        var settingsGrp = panel.add("panel", undefined, "Settings");
        settingsGrp.alignChildren = ["fill", "top"];
        settingsGrp.margins = [10, 14, 10, 8];
        settingsGrp.spacing = 4;

        var chunkGrp = settingsGrp.add("group");
        chunkGrp.alignment = ["fill", "center"];
        chunkGrp.add("statictext", undefined, "Chunk Size:");
        chunkInput = chunkGrp.add("edittext", undefined, "10");
        chunkInput.characters = 6;
        chunkGrp.add("statictext", undefined, "(video = full range)");

        var priGrp = settingsGrp.add("group");
        priGrp.alignment = ["fill", "center"];
        priGrp.add("statictext", undefined, "Priority:");
        priorityInput = priGrp.add("edittext", undefined, "50");
        priorityInput.characters = 6;

        var rsGrp = settingsGrp.add("group");
        rsGrp.alignment = ["fill", "center"];
        rsGrp.add("statictext", undefined, "RS Template:");
        rsDropdown = rsGrp.add("dropdownlist", undefined, ["(Render Queue)"]);
        rsDropdown.selection = 0;

        var omGrp = settingsGrp.add("group");
        omGrp.alignment = ["fill", "center"];
        omGrp.add("statictext", undefined, "OM Template:");
        omDropdown = omGrp.add("dropdownlist", undefined, ["(Render Queue)"]);
        omDropdown.selection = 0;

        // Submit button
        submitBtn = panel.add("button", undefined, "Submit");
        submitBtn.enabled = false;
        submitBtn.onClick = submitJobs;

        panel.layout.layout(true);

        return panel;
    }

    // -- Launch ---------------------------------------------------------------

    var myPanel = buildUI(thisObj);

    if (myPanel !== null && myPanel instanceof Window) {
        scanRenderQueue();
        myPanel.center();
        myPanel.show();
    }

})(this);
