---
title: Submit a job
nav_order: 4
---

# Submit a job

You can submit a job two ways: from minRender itself, or from the Blender add-on shipped in the sync root's `plugins/` folder.

## From minRender

Click **New Job** at the top right of the window and pick a template. minRender ships one for the form, **Blender 5.3 — Studio (Simple)**; your own templates appear beside it.

![New Job](/images/mr1-newjob.png)

Fill out the form. Fields whose label is amber are required. **Browse** opens a file picker for the scene file. Press **Submit**; the new job is selected in the list.

- **Start / End / Chunk** — the frame range, and how many frames go to a node at a time.
- **Priority** defaults to 50 — lower numbers render sooner. It isn't locked in at submission: you can change it later from the job detail, or drag jobs of equal priority into a custom order. See [job order & priority](monitor.md#job-order--priority).
- If a path lies outside every [path mapping](configure.md#path-mappings), an amber line under the field says so. The job is still submitted; only machines that have that exact path can render it.

{: .note }
> It's easy to create your own templates with only the fields you care about. Navigate to your sync root and duplicate one of the files under `templates/examples/`, drop the copy into `templates/`, strip flags you don't want, and prefill defaults. See [Job templates](templates.md) for the full reference.

## From Blender

The add-on builds the same submission the form does, but pulls the frame range, the output path and the scene from Blender.

Use **Edit → Preferences → Add-ons → Install from Disk** in Blender and point it at `plugins/blender/MinRender.py` in the sync root's `MinRender-v2` folder. The minRender submitter appears in the Render Properties panel. It collects your output path and frame range, and you can adjust them before pressing **Submit to Farm**.

![Blender submitter](/images/blender_RQADDXgy9f.png)

The add-on hands the job to the minRender running on the same machine, so minRender must be running there (hidden in the tray is fine).

{: .note }
> The add-on file in the sync root is kept current by the farm: the leader writes the newest version there. Blender does
> not pick that up by itself — **after upgrading minRender, install the add-on again** on every workstation that
> submits. Its version, shown in Blender's add-on list, matches the minRender version it came with.

## Other renderers

minRender launches any command-line renderer a [template](templates.md) describes. Templates and submission plug-ins for After Effects and Cinema 4D shipped with minRender 0.5; they are not part of 1.0 and remain on the [`0.5.x` branch](https://github.com/cbkow/MinRender/tree/0.5.x) as a starting point.

## Next

Once a job is submitted, [watch it render](monitor.md).
