---
title: Job templates
nav_order: 6
---

# Job templates

A job template is a JSON file that tells minRender how to launch a command-line renderer. It defines the executable, the argument structure, how to parse progress from stdout, and sensible defaults for the submission form.

Your own templates live in `{farm}/templates/` (`{farm}` is the `MinRender-v2` folder in the sync root). The example template that ships with minRender is in `{farm}/templates/examples/` and is overridden by a user template with the same `template_id`. The templates the Blender add-on submits through are in `{farm}/templates/plugins/`; they do not show in the New Job form.

minRender never deletes a template from the sync root. The leader rewrites the shipped ones when a newer minRender joins the farm, so keep your changes in copies under `templates/`, not in the shipped files.

---

## Minimal example

```json
{
    "_version": 1,
    "template_id": "blender-5.3-mine",
    "name": "Blender 5.3 (mine)",

    "cmd": {
        "windows": "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe",
        "linux": "/usr/bin/blender",
        "macos": "/Applications/Blender.app/Contents/MacOS/Blender",
        "label": "Blender Executable",
        "editable": true
    },

    "flags": [
        { "flag": "-b", "value": null, "info": "Background mode" },
        { "flag": "", "value": "", "info": "Scene File", "editable": true, "required": true, "type": "file", "filter": "blend" },
        { "flag": "-o", "value": null, "info": "Output path flag" },
        { "flag": "", "value": "", "info": "Output Path", "editable": true, "required": true, "type": "output" },
        { "flag": "-s", "value": null },
        { "flag": "", "value": "{chunk_start}" },
        { "flag": "-e", "value": null },
        { "flag": "", "value": "{chunk_end}" },
        { "flag": "-a", "value": null, "info": "Render animation" }
    ],

    "job_defaults": { "frame_start": 1, "frame_end": 100, "chunk_size": 10 },
    "progress": { "patterns": [], "error_patterns": [] },
    "output_detection": { "stdout_regex": null, "validation": "exit_code_only" },
    "process": { "working_dir": null },
    "environment": {},
    "tags_required": ["blend"]
}
```

This produces a command line like:

```
blender.exe -b scene.blend -o //output/path -s 1 -e 10 -a
```

---

## Top-level fields

| Field | Type | Description |
|---|---|---|
| `_version` | `int` | Always `1`. Reserved for future schema changes. |
| `template_id` | `string` | Unique identifier. Used to match user templates against examples. |
| `name` | `string` | Display name shown in the New Job form. |
| `cmd` | object | Executable paths per OS (see below). |
| `flags` | array | Ordered list of command-line arguments (see below). |
| `frame_padding` | `string` | Frame number format used in `default_pattern` tokens. e.g. `"####"` (Blender), `"[####]"` (After Effects). |
| `job_defaults` | object | Pre-fills the submission form (see below). |
| `progress` | object | Regex patterns for parsing the renderer's output (see below). |
| `output_detection` | object | How to find rendered file paths in stdout (see below). |
| `process` | object | Process control settings (see below). |
| `environment` | `object` | Extra environment variables passed to the renderer. Key-value string pairs. |
| `tags_required` | `string[]` | Nodes must have ALL of these tags to be eligible for this template's jobs. |

---

## `cmd` — executable

Defines the renderer's executable path for each operating system.

```json
"cmd": {
    "windows": "C:/Program Files/Blender Foundation/Blender 5.3/blender.exe",
    "linux": "/usr/bin/blender",
    "macos": "/Applications/Blender.app/Contents/MacOS/Blender",
    "label": "Blender Executable",
    "editable": true
}
```

| Field | Type | Description |
|---|---|---|
| `windows` | `string` | Path on Windows. |
| `linux` | `string` | Path on Linux. |
| `macos` | `string` | Path on macOS. |
| `label` | `string` | A label for the path. |
| `editable` | `bool` | Reserved. The New Job form has no field for the executable: the template's path for each system is used. To use another path, copy the template and change it. |

At least one OS path must be non-empty. Leave unsupported platforms as `""`.

---

## `flags[]` — command-line arguments

The flags array is the core of the template. It's an ordered list that defines every argument passed to the renderer. minRender reads it top to bottom and builds the command line in order.

### How flags map to arguments

Each entry can produce a **flag name**, a **value**, or both:

| `flag` | `value` | What it produces | Use case |
|---|---|---|---|
| `"-b"` | `null` | `-b` | Standalone switch |
| `"-F"` | `"EXR"` | `-F EXR` | Flag with a fixed value |
| `""` | `"scene.blend"` | `scene.blend` | Positional argument |
| `"-o"` | `null` | `-o` | Flag name only (value is the next entry) |

The common pattern is a **flag/value pair** across two entries: a non-editable flag name (e.g. `-o` with `value: null`) followed by an editable positional value (e.g. `flag: ""` with the user-provided path). This keeps the flag name fixed while letting the user fill in the value.

### Flag entry fields

| Field | Type | Default | Description |
|---|---|---|---|
| `flag` | `string` | | The flag name (e.g. `"-b"`, `"-o"`). Empty string `""` for positional arguments. |
| `value` | `string?` | `null` | The value. `null` = no value (standalone flag). `""` = user fills in. Any string = fixed or runtime token. |
| `info` | `string` | `""` | Label shown in the New Job form. |
| `editable` | `bool` | `false` | Whether the user can modify this value at submission time. Only flags that are `editable` or `required` show in the form. |
| `required` | `bool` | `false` | If `true` and `editable`, submission is refused while it is empty. The field's label is amber. |
| `type` | `string` | `""` | `"file"` = shows a file picker. `"output"` = output path (used for output directory tracking and local staging). `""` = plain text input. |
| `filter` | `string` | `""` | File extension filter for the file picker (e.g. `"blend"`, `"c4d"`, `"aep"`). Only relevant when `type` is `"file"`. |
| `id` | `string` | `""` | Identifier for cross-referencing. Other flags' `default_pattern` can reference this value via `{flag:id}`. |
| `default_pattern` | `string?` | `null` | A pattern for the value (see Pattern tokens below). **Not applied by the New Job form in 1.0** — the field starts with `value`; the key is kept for a later version. |

### Skipping empty optional flags

If a non-required editable positional value is left empty by the user, both it **and** its preceding standalone flag are omitted from the final command line. This lets templates include optional flags (like `-take` in the Cinema 4D example below) that disappear cleanly when unused.

### Runtime tokens

These tokens in flag values are replaced at dispatch time with the actual frame range for the chunk:

| Token | Replaced with |
|---|---|
| `{chunk_start}` | First frame of the chunk |
| `{chunk_end}` | Last frame of the chunk |
| `{frame}` | Alias for `{chunk_start}` |

### Pattern tokens (for `default_pattern`)

The tokens a `default_pattern` may use. The New Job form in 1.0 does not fill a field from its pattern yet; the Blender add-on takes the output path from the scene instead.

| Token | Source |
|---|---|
| `{project_dir}` | Parent directory of the first `type: "file"` flag's value |
| `{file_name}` | Filename stem of the first `type: "file"` flag's value |
| `{frame_pad}` | The template's `frame_padding` value (e.g. `####`) |
| `{flag:some_id}` | Value of the flag with `"id": "some_id"` |
| `{date:YYYYMMDD}` | Current date |
| `{date:YYYY}`, `{date:MM}`, `{date:DD}` | Year, month, day |
| `{time:HHmm}`, `{time:HH}`, `{time:mm}` | Time |

Example: a Blender output `default_pattern`:
```
{project_dir}\..renders\{file_name}\{file_name}_{frame_pad}
```
With a scene file at `D:\projects\shot01\shot01.blend`, this resolves to:
```
D:\projects\renders\shot01\shot01_####
```

---

## `job_defaults` — submission defaults

Pre-fills the New Job form. The range, chunk size and priority can be changed there; `max_retries` and `timeout_seconds` are taken as they are and can be changed afterwards with **Edit**.

```json
"job_defaults": {
    "frame_start": 1,
    "frame_end": 250,
    "chunk_size": 10,
    "priority": 50,
    "max_retries": 3,
    "timeout_seconds": null
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `frame_start` | `int` | `1` | Default start frame. |
| `frame_end` | `int` | `250` | Default end frame. |
| `chunk_size` | `int` | `1` | Frames per chunk. Higher = fewer dispatches, lower = finer distribution across nodes. |
| `priority` | `int` | `50` | Job priority (lower number = higher priority). |
| `max_retries` | `int` | `3` | How many times a failed chunk is retried before giving up. |
| `timeout_seconds` | `int?` | `null` | Kill the render process if a chunk takes longer than this. `null` = no timeout. |

---

## `progress` — stdout parsing

Tells minRender what to read in the renderer's output. **A chunk succeeds or fails by the renderer's exit code alone**; these patterns only feed the progress display.

```json
"progress": {
    "patterns": [
        {
            "regex": "Rendering (\\d+)\\s*/\\s*(\\d+) samples",
            "type": "fraction",
            "numerator_group": 1,
            "denominator_group": 2,
            "info": "Cycles sample progress"
        }
    ],
    "completion_pattern": {
        "regex": "Saved: '?(.+)'?",
        "info": "Blender logs 'Saved:' when a frame is written"
    },
    "error_patterns": [
        { "regex": "CUDA error.*", "info": "GPU failure" }
    ]
}
```

### `patterns[]` — progress tracking

Each pattern extracts a progress value from a line of output. minRender 1.0 keeps these with the job but does not show progress inside a frame; progress within a chunk comes from `completion_pattern`, frame by frame.

| Field | Type | Description |
|---|---|---|
| `regex` | `string` | Regex applied to each stdout line. |
| `type` | `string` | `"fraction"` (e.g. 5/100) or `"percentage"` (e.g. 50%). |
| `numerator_group` | `int` | Capture group for the numerator (fraction type). |
| `denominator_group` | `int` | Capture group for the denominator (fraction type). |
| `group` | `int` | Capture group for the percentage value (percentage type). |
| `info` | `string` | Description (for debugging/documentation). |

### `completion_pattern` — frame completion

Optional. When matched, signals that a frame has finished rendering. Used for per-frame progress tracking in multi-frame chunks.

### `error_patterns[]` — error detection

Each entry has `regex` and `info`. They are kept with the job and **not acted on** — as in 0.5, whose manual said otherwise. A renderer that prints an error and still exits with code 0 counts as a success; Blender's `--python-exit-code 1` in the shipped templates is there for that reason.

---

## `output_detection` — rendered file detection

Describes where the renderer names the files it wrote. It is kept with the job; **1.0 does not validate output files** (`exists_nonzero` is not acted on — as in 0.5). The preview finds frames by the job's output path instead.

```json
"output_detection": {
    "stdout_regex": "Saved: '?(.+\\.[a-zA-Z0-9]+)'?",
    "path_group": 1,
    "validation": "exists_nonzero",
    "info": "Blender prints 'Saved: <path>' after writing each frame"
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `stdout_regex` | `string?` | `null` | Regex to extract the output file path from stdout. `null` = no stdout detection. |
| `path_group` | `int` | `1` | Capture group containing the file path. |
| `validation` | `string` | `"exit_code_only"` | `"exit_code_only"` = trust the exit code — the only behaviour today. `"exists_nonzero"` is accepted and not acted on. |
| `info` | `string` | `""` | Description. |

---

## `process` — process control

```json
"process": {
    "working_dir": null
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `working_dir` | `string?` | `null` | Working directory for the process. `null` = inherit. Supports runtime tokens (`{chunk_start}`, etc.). |

On cancel, stop, exit or timeout, minRender always kills the renderer's entire process
tree immediately (Job Object on Windows, process group on macOS) — this is
not configurable. A `kill_method` field from older templates is ignored.

---

## Complete flag examples

### Blender

```json
"flags": [
    { "flag": "-b", "value": null, "info": "Background mode" },
    { "flag": "", "value": "", "info": "Scene File", "editable": true, "required": true, "type": "file", "filter": "blend" },
    { "flag": "-o", "value": null, "info": "Output path flag" },
    { "flag": "", "value": "", "info": "Output Path", "editable": true, "required": true, "type": "output",
      "default_pattern": "{project_dir}\\..\\renders\\{file_name}\\{file_name}_{frame_pad}" },
    { "flag": "-F", "value": "OPEN_EXR_MULTILAYER", "info": "Output Format" },
    { "flag": "-s", "value": null },
    { "flag": "", "value": "{chunk_start}" },
    { "flag": "-e", "value": null },
    { "flag": "", "value": "{chunk_end}" },
    { "flag": "-a", "value": null, "info": "Render animation" }
]
```

Produces: `blender -b scene.blend -o /renders/shot/shot_#### -F OPEN_EXR_MULTILAYER -s 1 -e 10 -a`

### Another renderer, with optional flags (Cinema 4D)

minRender 1.0 ships no Cinema 4D template; this shows how optional flags are written.

```json
"flags": [
    { "flag": "-nogui", "value": null },
    { "flag": "-render", "value": null },
    { "flag": "", "value": "", "info": "Scene File", "editable": true, "required": true, "type": "file", "filter": "c4d" },
    { "flag": "-take", "value": null },
    { "flag": "", "value": "", "info": "Take Name", "editable": true },
    { "flag": "-oimage", "value": null },
    { "flag": "", "value": "", "info": "Output Path", "editable": true, "required": true, "type": "output" },
    { "flag": "-frame", "value": null },
    { "flag": "", "value": "{chunk_start}" },
    { "flag": "", "value": "{chunk_end}" },
    { "flag": "", "value": "1", "info": "Frame step" }
]
```

With take left blank: `Commandline -nogui -render scene.c4d -oimage /renders/shot/shot -frame 1 10 1`
(The `-take` flag and its empty value are both omitted automatically.)
