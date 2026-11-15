---
title: Configure
nav_order: 3
---

# Configure

All of a node's settings are in one card: **Settings**, at the top right of the window (`Cmd+,` on macOS).
**Save restarts minRender; a render running on this node is ended** — it goes back to the queue.

![minRender Settings](/images/mr1-settings.png)

## Tags

Tags control what each node renders and influence leader election. Set them as comma-separated values (e.g. `blend, leader`).

| Tag | Purpose |
|---|---|
| `blend` | Enables Blender rendering on this node. |
| `leader` | Forces leadership priority. The mesh prefers this node as coordinator, but falls back to others if it drops out. |
| `noleader` | Prevents this node from becoming leader unless it's the only one alive. |

Templates have a `tags_required` field — a node only picks up a job if it carries every tag the template demands. The Blender templates require `blend`. Your own templates can require any tag you invent (`gpu`, `fast`, `ae`…).

## Path mappings

A farm with both Windows and macOS nodes needs to know that `P:\jobs` on one machine is `/Volumes/jobs` on another. Add one mapping per shared volume under **Path mappings**: a name, the Windows path and the macOS path. Every node needs the same mappings.

Paths in a job travel in their Windows form and each node translates them to its own. A path that lies under no mapping is sent exactly as typed, and minRender tells you so when you submit: only machines that have that exact path can render it.

## Local staging

With **Enable staging** on, minRender renders each chunk into `staging\` in this node's data folder and copies the finished frames to the output folder when the chunk is done.

Turn it on when your render output goes to a synced folder: many machines then never write half-finished frames into one folder at the same time. Leave it off on a real SMB share — staging adds a copy that isn't doing useful work there.

## The shared folder on sync storage

The sync root can live on a synced folder. Sync tools differ from a file server in one way that matters to a farm: when two machines create the same file or folder at the same moment, the tool keeps both, one of them under a "conflicted" name. minRender is built so that this does not happen in its own folder:

- every node writes only its own phonebook entry and its own log;
- the leader alone makes each job's folder, writes the templates and the add-on, and writes the queue's snapshot — whole, never half a file;
- a new leader checks the snapshot before it trusts it, and waits if it has not fully arrived;
- two nodes that set up the same new farm at the same moment settle on one shared secret by themselves.

Sync storage is still slower than a file server. Expect a new node to appear, and a submitted scene file to arrive on the render nodes, after your sync tool's usual delay.

## Networking

| Setting | What it does |
|---|---|
| **HTTP port** | The port nodes talk to each other on (default 8420). |
| **IP override** | The address this node tells the others. Leave empty unless the node has several networks and picks the wrong one. |
| **UDP multicast heartbeat** / **UDP port** | Fast discovery on a LAN (default port 4243). Where multicast does not pass — some Wi-Fi, VPNs — nodes still find each other through the phonebook in the shared folder. |

**Farm security** shows the fingerprint of the farm's shared secret. Every node of one farm shows the same fingerprint.

## The tray

Closing the window hides minRender to the system tray (the menu bar on macOS); the node keeps working. The tray icon's menu shows what the node is doing and has **Show Window**, **Stop Node** / **Resume Node**, on macOS **Check for Updates…**, and **Exit**.

**Exit ends minRender at once.** A render running on this node is killed and its chunk goes back to the queue — there is no question first, by design.

## Next

[Submit a job](submit.md) from minRender or from Blender.
