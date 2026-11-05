# MinRender.py — Farm submitter for Cinema 4D (version 0.5.11, tracks the minRender app)
# Install via: Script > Script Manager > Import Script
# Or copy to your C4D scripts folder and run from the Script menu.
#
# Reads the active document's render settings, lets you pick a Take,
# and submits a render job to MinRender via the local submissions dropbox.
# The running MinRender Monitor picks up submissions and routes them to the leader.

import c4d
import json
import os
import platform

# Tracks the minRender app version. 0.5.8: reads the data folder from
# %USERPROFILE%\minRender (falls back to the pre-0.5.8 AppData folder).
PLUGIN_VERSION = "0.5.11"
import socket
import time


# -- Config helpers ------------------------------------------------------------

def get_config_dir():
    s = platform.system()
    if s == "Windows":
        # minRender 0.5.8+ keeps its data in %USERPROFILE%\minRender (a folder
        # outside AppData, so the Microsoft Store build and this plug-in see
        # the same files). Fall back to the pre-0.5.8 %LOCALAPPDATA%\MinRender
        # while an older app version is installed.
        new = os.path.join(os.environ.get("USERPROFILE", ""), "minRender")
        old = os.path.join(os.environ.get("LOCALAPPDATA", ""), "MinRender")
        if os.path.isfile(os.path.join(new, "config.json")) or not os.path.isdir(old):
            return new
        return old
    elif s == "Darwin":
        return os.path.expanduser("~/Library/Application Support/MinRender")
    xdg = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
    return os.path.join(xdg, "MinRender")


def get_submissions_dir():
    d = os.path.join(get_config_dir(), "submissions")
    os.makedirs(d, exist_ok=True)
    return d


def read_config():
    p = os.path.join(get_config_dir(), "config.json")
    if not os.path.isfile(p):
        return None
    try:
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def get_farm_path():
    config = read_config()
    if not config or not config.get("sync_root"):
        return None
    farm = os.path.join(config["sync_root"], "MinRender-v2")
    return farm if os.path.isdir(farm) else None


# -- Template ID ---------------------------------------------------------------

TEMPLATE_ID = "cinema4d-2026-plugin"


# -- Takes enumeration ---------------------------------------------------------

def collect_takes(doc):
    """Return [(take_object, display_name)] for the Take dropdown."""
    result = []
    td = doc.GetTakeData()
    if not td:
        return result

    def walk(take, depth=0):
        if not take:
            return
        indent = "  " * depth
        result.append((take, indent + take.GetName()))
        child = take.GetDown()
        while child:
            walk(child, depth + 1)
            child = child.GetNext()

    walk(td.GetMainTake())
    return result


# -- C4D format ID to -oformat string mapping ----------------------------------

FORMAT_MAP = {
    1103: "TIFF",
    1101: "TGA",
    1100: "BMP",
    1104: "IFF",
    1102: "JPG",
    1023671: "EXR",
    1023540: "HDR",
    1023737: "DPX",
    1125: "AVI",
}


def format_id_to_string(fid):
    return FORMAT_MAP.get(fid, "")


# -- Dialog --------------------------------------------------------------------

ID_TAKE_COMBO      = 10001
ID_OVERRIDE_RANGE  = 10002
ID_FRAME_START     = 10003
ID_FRAME_END       = 10004
ID_OVERRIDE_OUTPUT = 10005
ID_OUTPUT_PATH     = 10006
ID_CHUNK_SIZE      = 10007
ID_PRIORITY        = 10008
ID_SUBMIT_BTN      = 10009
ID_STATUS_TEXT      = 10010
ID_SCENE_LABEL     = 10011
ID_RANGE_LABEL     = 10012


class MinRenderDialog(c4d.gui.GeDialog):

    def __init__(self):
        super().__init__()
        self.takes = []
        self.scene_frame_start = 0
        self.scene_frame_end = 90
        self.scene_output = ""

    def CreateLayout(self):
        self.SetTitle("MinRender " + PLUGIN_VERSION)

        doc = c4d.documents.GetActiveDocument()
        rd = doc.GetActiveRenderData()
        fps = doc.GetFps()

        # Cache scene values
        self.scene_frame_start = rd[c4d.RDATA_FRAMEFROM].GetFrame(fps)
        self.scene_frame_end = rd[c4d.RDATA_FRAMETO].GetFrame(fps)
        self.scene_output = rd[c4d.RDATA_PATH] or ""

        # Resolve relative output path
        if self.scene_output and not os.path.isabs(self.scene_output):
            doc_path = doc.GetDocumentPath()
            if doc_path:
                self.scene_output = os.path.join(doc_path, self.scene_output)

        # -- Scene info --
        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=1)
        self.GroupBorderSpace(6, 6, 6, 6)
        doc_name = doc.GetDocumentName()
        doc_path = doc.GetDocumentPath()
        if doc_path:
            self.AddStaticText(ID_SCENE_LABEL, c4d.BFH_LEFT, name=doc_name)
        else:
            self.AddStaticText(ID_SCENE_LABEL, c4d.BFH_LEFT, name="(file not saved)")
        self.GroupEnd()

        self.AddSeparatorH(0, c4d.BFH_SCALEFIT)

        # -- Take dropdown --
        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=2)
        self.GroupBorderSpace(6, 4, 6, 4)
        self.AddStaticText(0, c4d.BFH_LEFT, initw=80, name="Take:")
        self.AddComboBox(ID_TAKE_COMBO, c4d.BFH_SCALEFIT)
        self.GroupEnd()

        self.takes = collect_takes(doc)
        td = doc.GetTakeData()
        current_take = td.GetCurrentTake() if td else None
        current_idx = 0

        for i, (take, display_name) in enumerate(self.takes):
            self.AddChild(ID_TAKE_COMBO, i, display_name)
            if take == current_take:
                current_idx = i
        self.SetInt32(ID_TAKE_COMBO, current_idx)

        self.AddSeparatorH(0, c4d.BFH_SCALEFIT)

        # -- Frame range --
        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=2)
        self.GroupBorderSpace(6, 4, 6, 4)
        self.AddCheckbox(ID_OVERRIDE_RANGE, c4d.BFH_LEFT, initw=0, inith=0,
                         name="Override Range")
        range_str = "Scene: {} \u2013 {} ({} frames)".format(
            self.scene_frame_start, self.scene_frame_end,
            self.scene_frame_end - self.scene_frame_start + 1)
        self.AddStaticText(ID_RANGE_LABEL, c4d.BFH_LEFT, name=range_str)
        self.GroupEnd()

        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=4)
        self.GroupBorderSpace(6, 0, 6, 4)
        self.AddStaticText(0, c4d.BFH_LEFT, initw=40, name="Start:")
        self.AddEditNumberArrows(ID_FRAME_START, c4d.BFH_LEFT, initw=80)
        self.AddStaticText(0, c4d.BFH_LEFT, initw=40, name="End:")
        self.AddEditNumberArrows(ID_FRAME_END, c4d.BFH_LEFT, initw=80)
        self.GroupEnd()

        self.AddSeparatorH(0, c4d.BFH_SCALEFIT)

        # -- Output path --
        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=1)
        self.GroupBorderSpace(6, 4, 6, 4)
        self.AddCheckbox(ID_OVERRIDE_OUTPUT, c4d.BFH_LEFT, initw=0, inith=0,
                         name="Override Output Path")
        self.AddEditText(ID_OUTPUT_PATH, c4d.BFH_SCALEFIT)
        self.GroupEnd()

        self.AddSeparatorH(0, c4d.BFH_SCALEFIT)

        # -- Chunk size & Priority --
        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=4)
        self.GroupBorderSpace(6, 4, 6, 4)
        self.AddStaticText(0, c4d.BFH_LEFT, initw=80, name="Chunk Size:")
        self.AddEditNumberArrows(ID_CHUNK_SIZE, c4d.BFH_LEFT, initw=80)
        self.AddStaticText(0, c4d.BFH_LEFT, initw=60, name="Priority:")
        self.AddEditNumberArrows(ID_PRIORITY, c4d.BFH_LEFT, initw=80)
        self.GroupEnd()

        self.AddSeparatorH(0, c4d.BFH_SCALEFIT)

        # -- Status & Submit --
        self.GroupBegin(0, c4d.BFH_SCALEFIT, cols=1)
        self.GroupBorderSpace(6, 4, 6, 6)
        self.AddStaticText(ID_STATUS_TEXT, c4d.BFH_SCALEFIT, name="")
        self.AddButton(ID_SUBMIT_BTN, c4d.BFH_SCALEFIT, inith=30,
                       name="Submit to Farm")
        self.GroupEnd()

        return True

    def InitValues(self):
        self.SetInt32(ID_FRAME_START, self.scene_frame_start)
        self.SetInt32(ID_FRAME_END, self.scene_frame_end)
        self.Enable(ID_FRAME_START, False)
        self.Enable(ID_FRAME_END, False)

        self.SetString(ID_OUTPUT_PATH, self.scene_output)
        self.Enable(ID_OUTPUT_PATH, False)

        self.SetInt32(ID_CHUNK_SIZE, 10, min=1, max=10000)
        self.SetInt32(ID_PRIORITY, 50, min=1, max=100)

        # Check farm connection on open
        farm = get_farm_path()
        if not farm:
            config = read_config()
            if not config:
                self.SetString(ID_STATUS_TEXT, "MinRender config not found.")
            elif not config.get("sync_root"):
                self.SetString(ID_STATUS_TEXT, "Sync root not set in Monitor.")
            else:
                self.SetString(ID_STATUS_TEXT, "Farm not initialized.")
            self.Enable(ID_SUBMIT_BTN, False)

        return True

    def Command(self, id, msg):
        if id == ID_OVERRIDE_RANGE:
            enabled = self.GetBool(ID_OVERRIDE_RANGE)
            self.Enable(ID_FRAME_START, enabled)
            self.Enable(ID_FRAME_END, enabled)

        elif id == ID_OVERRIDE_OUTPUT:
            enabled = self.GetBool(ID_OVERRIDE_OUTPUT)
            self.Enable(ID_OUTPUT_PATH, enabled)

        elif id == ID_SUBMIT_BTN:
            self._submit()

        return True

    def _submit(self):
        # -- Validate farm --
        farm = get_farm_path()
        if not farm:
            c4d.gui.MessageDialog("Farm not connected.\nIs MinRender Monitor running?")
            return

        doc = c4d.documents.GetActiveDocument()
        rd = doc.GetActiveRenderData()
        fps = doc.GetFps()

        doc_path = doc.GetDocumentPath()
        doc_name = doc.GetDocumentName()
        if not doc_path:
            c4d.gui.MessageDialog("Save the file before submitting.")
            return

        scene_filepath = os.path.join(doc_path, doc_name)

        # -- Save document --
        c4d.documents.SaveDocument(
            doc, scene_filepath,
            c4d.SAVEDOCUMENTFLAGS_NONE, c4d.FORMAT_C4DEXPORT)

        # -- Frame range --
        if self.GetBool(ID_OVERRIDE_RANGE):
            frame_start = self.GetInt32(ID_FRAME_START)
            frame_end = self.GetInt32(ID_FRAME_END)
        else:
            frame_start = rd[c4d.RDATA_FRAMEFROM].GetFrame(fps)
            frame_end = rd[c4d.RDATA_FRAMETO].GetFrame(fps)

        if frame_start > frame_end:
            c4d.gui.MessageDialog("Start frame must be <= end frame.")
            return

        # -- Output path --
        if self.GetBool(ID_OVERRIDE_OUTPUT):
            output_path = self.GetString(ID_OUTPUT_PATH)
        else:
            output_path = self.scene_output

        if not output_path:
            c4d.gui.MessageDialog("No output path set.\n"
                                  "Set one in Render Settings or use the override.")
            return

        # -- Take --
        take_name = ""
        take_idx = self.GetInt32(ID_TAKE_COMBO)
        if 0 <= take_idx < len(self.takes):
            take_obj, _ = self.takes[take_idx]
            td = doc.GetTakeData()
            if td:
                main_take = td.GetMainTake()
                # Only pass -take if user selected something other than Main
                if take_obj != main_take:
                    take_name = take_obj.GetName()

        # -- Output format (from scene, for reference only) --
        format_str = format_id_to_string(rd[c4d.RDATA_FORMAT])

        # -- Build overrides --
        overrides = {
            "scene_file": scene_filepath,
            "output_path": output_path,
        }
        if take_name:
            overrides["take_name"] = take_name
        if format_str:
            overrides["output_format"] = format_str

        # -- Job name --
        job_name = os.path.splitext(doc_name)[0]
        if take_name:
            job_name += " - " + take_name

        chunk_size = self.GetInt32(ID_CHUNK_SIZE)
        priority = self.GetInt32(ID_PRIORITY)
        hostname = socket.gethostname()
        ts = int(time.time() * 1000)

        submission = {
            "_version": 1,
            "template_id": TEMPLATE_ID,
            "job_name": job_name,
            "submitted_by_host": hostname,
            "submitted_at_ms": ts,
            "overrides": overrides,
            "frame_start": frame_start,
            "frame_end": frame_end,
            "chunk_size": chunk_size,
            "priority": priority,
        }

        # -- Write submission JSON --
        submissions_dir = get_submissions_dir()
        filename = "{:013d}.{}.json".format(ts, hostname)
        out_path = os.path.join(submissions_dir, filename)

        try:
            tmp_path = out_path + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(submission, f, indent=2)
            os.replace(tmp_path, out_path)
        except Exception as e:
            c4d.gui.MessageDialog("Failed to write submission:\n{}".format(e))
            return

        self.SetString(ID_STATUS_TEXT, "Submitted: " + job_name)


# -- Entry point (Script Manager) ----------------------------------------------

def main():
    dlg = MinRenderDialog()
    dlg.Open(c4d.DLG_TYPE_MODAL_RESIZEABLE, defaultw=420, defaulth=320)


if __name__ == "__main__":
    main()
