import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import MinRenderUi 1.0

// Standalone submission form, Loader-mounted inside Main.qml's
// New Job and Edit Job modal dialogs. submitted/cancelled/failed
// signals give the wrapper a clean lifecycle hook.
//
// Edit mode: set editSeed (from appBridge.editSeed) and the form
// prefills from the job's stored manifest — template locked, job name
// locked, minRender job settings (retries/timeout) exposed — and the
// footer swaps Submit for the three apply modes (continue / stop &
// apply / start over). appBridge.applyJobEdit closes the editor on
// success; failures land in the same error banner as submissions.
Item {
    id: root

    signal submitted(string jobId)
    signal cancelled
    signal failed(string reason)

    property var editSeed: null
    // resubmitMode: seeded like edit mode but submits a NEW job scoped
    // to a chunk's frames (name editable, single Submit button).
    readonly property bool resubmitMode:
        !!editSeed && editSeed.resubmit === true
    readonly property bool editMode:
        !!editSeed && !!editSeed.jobId && !resubmitMode

    // Frame range / chunk size edits restructure the chunk table, so
    // only Start over can honor them.
    readonly property bool structuralChange:
        editMode && (frameStartField.value !== editSeed.frameStart
                     || frameEndField.value !== editSeed.frameEnd
                     || chunkSizeField.value !== editSeed.chunkSize)

    function applyEdit(mode) {
        errorBanner.text = ""
        appBridge.applyJobEdit(currentFlagValues,
                               frameStartField.value, frameEndField.value,
                               chunkSizeField.value, priorityField.value,
                               retriesField.value, timeoutField.value, mode)
    }

    Component.onCompleted: {
        if (!editMode && !resubmitMode)
            return
        currentTemplateId = editSeed.templateId
        currentTemplate = { flags: editSeed.flags }
        const vals = []
        for (let i = 0; i < editSeed.flags.length; ++i)
            vals.push(editSeed.flags[i].value || "")
        currentFlagValues = vals
        jobNameField.text = resubmitMode ? editSeed.suggestedName
                                         : editSeed.jobId
        frameStartField.value = editSeed.frameStart
        frameEndField.value   = editSeed.frameEnd
        chunkSizeField.value  = editSeed.chunkSize
        priorityField.value   = editSeed.priority
        retriesField.value    = editSeed.maxRetries
        timeoutField.value    = editSeed.timeoutSeconds
    }

    implicitWidth: 640
    implicitHeight: column.implicitHeight + 32

    // Currently selected template — refreshed when the picker changes.
    // Starts null; flag fields and frame defaults update once a template
    // is chosen.
    property var currentTemplate: null
    property var currentFlagValues: []
    property string currentTemplateId: ""

    function refreshTemplate(templateId) {
        currentTemplateId = templateId
        if (templateId.length === 0) {
            currentTemplate = null
            currentFlagValues = []
            return
        }
        const t = appBridge.templateById(templateId)
        currentTemplate = t
        if (t && t.flags) {
            const initial = []
            for (let i = 0; i < t.flags.length; ++i)
                initial.push(t.flags[i].value || "")
            currentFlagValues = initial
            frameStartField.value = t.frameStart
            frameEndField.value   = t.frameEnd
            chunkSizeField.value  = t.chunkSize
            priorityField.value   = t.priority
        } else {
            currentFlagValues = []
        }
    }

    // Listen for results from the bridge.
    Connections {
        target: appBridge
        function onSubmissionSucceeded(jobId) { root.submitted(jobId) }
        function onSubmissionFailed(reason)   { root.failed(reason); errorBanner.text = reason }
    }

    // Shared file picker for type=file flag rows. Each row sets the
    // pending-index + filter before opening; onAccepted writes back
    // to that row's flag value.
    FileDialog {
        id: flagFilePicker
        property int targetIndex: -1

        onAccepted: {
            if (targetIndex < 0) return
            const local = appBridge.urlToLocalPath(selectedFile)
            if (root.currentFlagValues && local.length > 0)
                root.currentFlagValues[targetIndex] = local
            // Force repeater rows to re-read their text property from
            // currentFlagValues. Reassigning the array triggers the
            // Repeater's model binding.
            const copy = root.currentFlagValues.slice()
            root.currentFlagValues = copy
            targetIndex = -1
        }
    }

    // Content lives in a ScrollView so templates with many flag fields,
    // long descriptions, or small panel heights don't push the Save /
    // Cancel footer off-screen. The footer is anchored to the panel
    // bottom so it stays pinned while the form scrolls.
    ScrollView {
        id: scrollArea
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            bottom: footer.top
            leftMargin: 16
            // Extra right margin reserves space for the vertical scrollbar
            // so it doesn't overlay the TextFields when content is tall
            // enough to scroll. Matches the AlwaysOn policy below.
            rightMargin: 24
            topMargin: 16
            bottomMargin: 8
        }
        clip: true
        contentWidth: availableWidth
        // AsNeeded so an empty / short form doesn't show a scroll track.
        // The 24px rightMargin on the ScrollView's anchors reserves more
        // than enough room for the thumb when it appears.
        ScrollBar.vertical: MrScrollBar {}
        ScrollBar.horizontal: MrScrollBar { policy: ScrollBar.AlwaysOff }

        ColumnLayout {
            id: column
            width: scrollArea.availableWidth
            spacing: 12

        // --- Template picker (locked to a label in edit mode) ---
        SectionHeader { text: qsTr("Template") }
        ComboBox {
            id: templatePicker
            visible: !root.editMode && !root.resubmitMode
            Layout.fillWidth: true
            model: appBridge.templatesModel
            textRole: "name"
            valueRole: "templateId"
            currentIndex: -1
            onActivated: root.refreshTemplate(currentValue || "")
        }
        Label {
            readonly property bool seeded: root.editMode || root.resubmitMode
            visible: seeded
            Layout.fillWidth: true
            text: seeded
                  ? (root.editSeed.templateFound
                     ? root.editSeed.templateName
                     : qsTr("%1 (template missing — editing raw values)")
                           .arg(root.editSeed.templateName))
                  : ""
            color: seeded && !root.editSeed.templateFound
                   ? Theme.warn : Theme.textPrimary
            font.pixelSize: Theme.fontSizeBase
            elide: Text.ElideRight
        }

        // --- Job name (the slug is the job's identity — locked in edit mode) ---
        SectionHeader { text: qsTr("Job name") }
        TextField {
            id: jobNameField
            Layout.fillWidth: true
            placeholderText: qsTr("e.g. shot_010_v003")
            readOnly: root.editMode
        }

        // --- Frame range / chunk / priority ---
        SectionHeader { text: qsTr("Range") }
        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 12
            rowSpacing: 4

            Label { text: qsTr("Start") }
            Label { text: qsTr("End") }
            Label { text: qsTr("Chunk") }
            Label { text: qsTr("Priority") }

            SpinBox { id: frameStartField; from: 0;   to: 1000000; editable: true; value: 1 }
            SpinBox { id: frameEndField;   from: 0;   to: 1000000; editable: true; value: 250 }
            SpinBox { id: chunkSizeField;  from: 1;   to: 10000;   editable: true; value: 1 }
            SpinBox { id: priorityField;   from: 1;   to: 999;     editable: true; value: 50 }
        }

        Label {
            visible: root.structuralChange
            Layout.fillWidth: true
            text: qsTr("Frame range / chunk size changed — only Start over can apply this.")
            color: Theme.warn
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }

        // --- Job settings (edit mode only) — minRender-owned values,
        // always editable regardless of what the template exposes.
        SectionHeader { visible: root.editMode; text: qsTr("Job settings") }
        GridLayout {
            visible: root.editMode
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 12
            rowSpacing: 4

            Label { text: qsTr("Max retries") }
            Label { text: qsTr("Timeout (s, 0 = none)") }

            SpinBox { id: retriesField; from: 0; to: 99;      editable: true; value: 3 }
            SpinBox { id: timeoutField; from: 0; to: 1000000; editable: true; value: 0 }
        }

        // --- Flags (dynamic) ---
        SectionHeader {
            text: qsTr("Flags")
            visible: currentTemplate && currentTemplate.flags && currentTemplate.flags.length > 0
        }

        Repeater {
            id: flagRepeater
            model: currentTemplate ? currentTemplate.flags : []

            delegate: ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                required property var modelData
                required property int index

                visible: modelData.editable === true || modelData.required === true

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: (modelData.info && modelData.info.length > 0)
                              ? modelData.info
                              : (modelData.flag.length > 0 ? modelData.flag : qsTr("(positional)"))
                        color: modelData.required ? Theme.warn : Theme.textPrimary
                        font.pixelSize: 11
                        Layout.preferredWidth: 140
                    }

                    TextField {
                        id: flagInput
                        Layout.fillWidth: true
                        readOnly: !modelData.editable
                        text: (root.currentFlagValues && root.currentFlagValues[index] !== undefined)
                              ? root.currentFlagValues[index] : ""
                        onTextChanged: {
                            if (root.currentFlagValues
                                && root.currentFlagValues[index] !== text)
                            {
                                root.currentFlagValues[index] = text
                            }
                        }
                    }

                    // File picker for flags the template marks as type=file
                    // (e.g. Scene File in the C4D template). filter is
                    // turned into a Qt name-filter string: "Filter (*.ext)".
                    FlatButton {
                        visible: modelData.type === "file"
                        iconName: "folder-open"
                        text: qsTr("Browse")
                        variant: "neutral"
                        onClicked: {
                            flagFilePicker.targetIndex = index
                            flagFilePicker.nameFilters = modelData.filter
                                && modelData.filter.length > 0
                                ? [modelData.filter + " (*." + modelData.filter + ")",
                                   "All files (*)"]
                                : ["All files (*)"]
                            // Seed the picker near the current value's folder
                            // if one is set.
                            const cur = flagInput.text
                            if (cur && cur.length > 0) {
                                // A file URL for the picker (min-paths: encoded, UNC + POSIX aware).
                                flagFilePicker.currentFile = appBridge.localPathToUrl(cur)
                            }
                            flagFilePicker.open()
                        }
                    }
                }

                Label {
                    visible: modelData.help && modelData.help.length > 0
                    Layout.fillWidth: true
                    text: modelData.help
                    color: Theme.textMuted
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                    leftPadding: 148
                }
            }
        }

            // --- Error banner (filled on submissionFailed) ---
            Rectangle {
                Layout.fillWidth: true
                visible: errorBanner.text.length > 0
                color: Qt.darker(Theme.error, 3.0)
                radius: Theme.radiusSmall
                implicitHeight: errorBanner.implicitHeight + 12

                Label {
                    id: errorBanner
                    anchors.fill: parent
                    anchors.margins: 6
                    color: Theme.error
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.fontSizeBase
                }
            }
        }
    }

    // --- Submit / Cancel (pinned footer) ---
    RowLayout {
        id: footer
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            leftMargin: 16
            rightMargin: 16
            bottomMargin: 16
        }
        spacing: 8

        Item { Layout.fillWidth: true }

        FlatButton {
            text: qsTr("Cancel")
            variant: "neutral"
            onClicked: root.cancelled()
        }

        BusyIndicator {
            visible: !root.editMode && appBridge.submitBusy
            running: visible
            implicitWidth: 22
            implicitHeight: 22
        }
        FlatButton {
            visible: !root.editMode
            iconName: "paper-plane-tilt"
            text: root.resubmitMode ? qsTr("Submit as new job") : qsTr("Submit")
            variant: "primary"
            enabled: jobNameField.text.length > 0 && currentTemplate !== null
                     && !appBridge.submitBusy
            onClicked: {
                errorBanner.text = ""
                if (root.resubmitMode && !root.editSeed.templateFound) {
                    // No template to re-bake through — mutate the
                    // parent's stored manifest instead.
                    appBridge.submitManifestResubmit(
                        jobNameField.text,
                        currentFlagValues,
                        frameStartField.value,
                        frameEndField.value,
                        chunkSizeField.value,
                        priorityField.value)
                    return
                }
                appBridge.submitJob(
                    currentTemplateId,
                    jobNameField.text,
                    currentFlagValues,
                    frameStartField.value,
                    frameEndField.value,
                    chunkSizeField.value,
                    priorityField.value)
            }
        }

        // --- Edit-mode apply buttons ---
        // All three disable while an apply round-trip is in flight
        // (editBusy) — the leader ack, not the click, closes the dialog.
        BusyIndicator {
            visible: root.editMode && appBridge.editBusy
            running: visible
            implicitWidth: 22
            implicitHeight: 22
        }
        FlatButton {
            visible: root.editMode
            iconName: "play"
            text: qsTr("Apply to next chunks")
            variant: "primary"
            enabled: !root.structuralChange && !appBridge.editBusy
            tooltipText: qsTr("In-flight chunks finish with the old settings; every chunk dispatched from now on uses the new ones.")
            onClicked: root.applyEdit("continue")
        }
        FlatButton {
            visible: root.editMode
            iconName: "arrow-clockwise"
            text: qsTr("Stop chunks && apply")
            variant: "neutral"
            enabled: !root.structuralChange && !appBridge.editBusy
            tooltipText: qsTr("Kills in-flight renders and requeues them with the new settings. Completed chunks keep their output.")
            onClicked: editConfirm.openFor("restart")
        }
        FlatButton {
            visible: root.editMode
            iconName: "arrows-counter-clockwise"
            text: qsTr("Start over")
            variant: "danger"
            enabled: !appBridge.editBusy
            tooltipText: qsTr("Discards ALL progress — every chunk, including completed frames, re-renders with the new settings.")
            onClicked: editConfirm.openFor("startover")
        }
    }

    // Destructive apply modes route through a confirm, matching the
    // Cancel/Delete convention in the jobs panel.
    Dialog {
        id: editConfirm
        property string mode: ""

        modal: true
        anchors.centerIn: parent
        width: 380
        title: mode === "startover" ? qsTr("Start over?") : qsTr("Stop in-flight chunks?")
        standardButtons: Dialog.Ok | Dialog.Cancel

        function openFor(m) { mode = m; open() }
        onAccepted: root.applyEdit(mode)

        Label {
            anchors.fill: parent
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBase
            text: editConfirm.mode === "startover"
                  ? qsTr("All chunks — including completed frames — are reset and re-rendered with the new settings. In-flight renders are killed immediately.")
                  : qsTr("In-flight renders are killed and requeued with the new settings. Chunks that already completed keep the output they rendered with the old settings.")
        }
    }
}
