---
title: Upgrading from 0.5
permalink: /upgrade/
nav_order: 8
---

# Upgrading from minRender 0.5

minRender 1.0 is the same farm, rebuilt: the same shared folder, the same settings file, the same node identities. A 1.0 node works in one farm with 0.5.10 and 0.5.11 nodes, so you can upgrade one machine at a time.

## What to do

1. **Windows nodes:** with the farm idle, `winget upgrade --id 9PN4VXDZ3THW --source msstore --accept-package-agreements` (or update from the Store). The node keeps its settings and its identity.
2. **macOS nodes:** minRender 0.5 offers the update itself (**Check for Updates…**). macOS 13 or later is required from 1.0.
3. **Blender add-on:** install the add-on again on each workstation, from `plugins/blender/MinRender.py` in the sync root (see [Submit a job](submit.md#from-blender)). The old add-on keeps working through the old template until you do.

## What changed

- **Blender only, out of the box.** The After Effects and Cinema 4D templates and plug-ins are not part of 1.0. minRender never deletes files from your sync root, so the old ones are still there; remove these by hand once no workstation uses them:
  - `templates/examples/after-effects-*.json`, `templates/examples/cinema4d-*.json`, and the older Blender example
  - `templates/plugins/after-effects-plugin.json`, `templates/plugins/cinema4d-*-plugin.json`
  - `templates/plugins/blender-5.2-plugin.json` and `templates/examples/blender-5.2-*.json` — only after every workstation has the new add-on
  - `plugins/afterEffects/`, `plugins/cinema4d/`
- **Blender 5.3.** The shipped templates point at Blender 5.3 on Windows (`…\Blender Foundation\Blender 5.3\blender.exe`).
- **One program.** There is no separate render agent any more; the "agent" log is gone. The kill is the same: Cancel, Stop and Exit end the renderer's whole process tree at once.
- **No farm is not the end.** A node that starts before its shared folder is there waits for it and joins by itself.
- **Edit a job**, **submit a chunk as a separate job**, **Farm Cleanup**, **EXR layers** in the preview, and a log picker that scales to large farms.
- **Exit and Stop never ask.** Exit from the tray, and Stop on a node, end a render at once.
- **Sync folders.** The farm's own folder is now written so that sync tools do not produce "conflicted" copies in it — see [Configure](configure.md#the-shared-folder-on-sync-storage).

## The RNDR companion

The optional RNDR companion for Windows keeps working: it only asks the local minRender whether it is rendering.
