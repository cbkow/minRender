---
title: Monitor a job
nav_order: 5
---

# Monitor a job

Click any job in the **Jobs** list to load it into the **Job Detail** tab below. Progress and the frame grid update live from the leader as chunks come back from the nodes. The gaps between the panels can be dragged; the sizes are kept.

![minRender](/images/mr1-detail.png)

## Job order & priority

The Jobs list is in **dispatch order**: priority first (lower number = renders sooner, default 50, the **Pri** column), then queue order within each priority group.

- **Drag to reorder** — drag a job's row up or down; a line shows where it will land. Dropped among jobs of another priority, the job takes that priority. The new order is live: the next chunk dispatched anywhere on the farm comes from the new front of the queue.
- **Change priority** — select a job and edit the **priority** field in the job detail. It takes effect on the next dispatch; no requeue needed.

## Actions on jobs

Selecting one or more jobs lights the strip above the list — **Edit**, **Pause**, **Resume**, **Retry failed**, **Requeue**, **Archive**, **Cancel**, **Delete** — applied to every selected job. Right-click a job for the same commands. Cancel and Delete ask first; the others act at once.

Shift-click and Cmd/Ctrl-click select several jobs; the arrow keys move through the list, `Cmd/Ctrl+A` selects all, `Return` or a double-click opens **Edit**, `Delete` deletes.

**Cancel is immediate and farm-wide.** It stops dispatch and kills any in-flight render of that job on every node — the whole renderer process tree, not just the launcher. Cancelled jobs keep their records and can be requeued later. **Pause**, by contrast, is gentle: in-flight chunks finish, only new dispatch stops.

## Edit a job

**Edit** opens the job's values in the same card as New Job, plus **Max retries** and **Timeout**. Three ways to apply:

| Button | What happens |
|---|---|
| **Apply to next chunks** | Chunks already rendering finish with the old settings; every chunk dispatched from now on uses the new ones. |
| **Stop chunks & apply** | Rendering chunks are killed and requeued with the new settings. Completed chunks keep their output. Asks first. |
| **Start over** | Discards all progress; every chunk renders again with the new settings. Asks first. Required when the frame range or chunk size changed. |

## The job detail

The header shows the job's state, its progress, the **priority** field and **Open output**, which reveals the job's render folder in the file manager.

### Frame grid colors

One cell per frame of the job's range, coloured by state. Hover a cell for its frame, chunk, state and node; click it to hold the preview on that frame.

| Color | Meaning |
|---|---|
| **grey** | Waiting — no node has this frame yet. |
| **blue** | Assigned — a node has the frame's chunk and is rendering. |
| **dark green** | The frame is rendered; its chunk is still running. |
| **bright green** | Done — the whole chunk is complete (with local staging, copied to your render folder). |
| **red** | The chunk containing this frame failed. Use **Retry failed** to queue it again. |

### Chunks

Under the grid, one row per chunk: its state, frames, the node rendering it, progress, duration and retries. Right-click a chunk (or several):

- **Return to queue** — take the chunk back from its node, or queue a failed or stopped chunk again.
- **Stop chunk** — set it aside; the job finishes without it and shows as **partial**.
- **Submit as separate job…** — a new job for just those frames, with a form to adjust it first.

### Preview

The **Preview** pane (bottom left) shows the latest frame the job has written. Clicking a frame or a chunk holds the preview there for a minute; **Latest** lets go. For EXR files **Layers** opens the file's layers as small pictures — pick one to preview that layer.

## Nodes

The left panel shows this node (with **Stop** / **Resume**: Stop ends a render on this node at once and takes no new work) and the **Peers**. Right-click a peer to **Stop**, **Start** or **Restart** it, or to **Show log**.

A node on which chunks keep failing is **suspended** by the leader: it shows red and gets no work until someone clicks **Unsuspend**. That keeps one broken machine from eating a job's retries.

## Logs

The **Logs** tab has three sources:

- **This node** — minRender's own log on this machine.
- **Peer…** — the log of another node, picked from the list.
- **Task Output** — the renderer's own output for the selected job. Pick a chunk on the left, read its output on the right.

These logs are also files in the sync root (under `MinRender-v2/nodes/<node>/` and `MinRender-v2/jobs/<job>/`), so you can open them in a text editor.

## Farm Cleanup

**Farm Cleanup** (top right) lists what can be cleared, in six sections with a checkbox per item: finished jobs (archive them), archived jobs (delete them), job folders no job owns, peers that have gone for good, and leftover staging folders on this machine. Nothing here asks twice.
