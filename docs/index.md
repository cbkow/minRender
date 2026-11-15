---
title: Home
permalink: /
nav_order: 1
---

# minRender

A lightweight render farm coordinator for small VFX teams and freelancers.

![minRender](/images/mr1-window.png)

## What it does

- **Self-healing mesh** — every node can act as coordinator. If the leader drops, another takes over automatically.
- **Blender, out of the box** — a submission add-on for Blender and the templates it uses ship with the application. JSON templates define how to launch any command-line renderer.
- **Simple setup** — install, point every node at a shared folder, done. A node whose shared folder is not there yet waits for it and joins by itself.
- **Fast discovery** — UDP multicast for LAN, falls back to a file-system phonebook for VPNs and complex networks.
- **HTTP coordination** — job dispatch, progress tracking, and completion reporting over an HTTP mesh.
- **Made for sync folders too** — the shared folder can be an SMB share or a synced folder (Dropbox, Synology Drive, LucidLink, Syncthing…). See [Shared folders](configure.md#the-shared-folder-on-sync-storage).
- **Local staging** — opt-in render-to-local-then-copy mode, so many machines never write half-finished frames into one synced folder.
- **Live queue control** — drag jobs to reorder the queue, change priority mid-flight, edit a running job, and cancel kills the renderer's whole process tree on every node, instantly.
- **Resilient** — the leader keeps a snapshot of the queue in the shared folder. If the leader drops, a new one picks up where it left off. Worst case: frames rendered in the last 30 seconds get re-rendered.
- **Windows 10/11 and macOS 13+**, in one farm.

## Get going

1. [**Install**](install.md) minRender on every node.
2. [**Configure**](configure.md) your sync root, tags, path mappings and local staging.
3. [**Submit a job**](submit.md) from minRender or from Blender.
4. [**Watch it render**](monitor.md).

Coming from minRender 0.5? Read [Upgrading from 0.5](upgrade.md) first. For deeper customization, the
[job template reference](templates.md) covers how to wire up any command-line renderer.
