---
title: Install
nav_order: 2
---

# Install

Install minRender on every node that will participate in the farm. Nodes can be Windows or macOS and work together in one farm.

## Windows

Install minRender from the [Microsoft Store](https://apps.microsoft.com/detail/9PN4VXDZ3THW). The Store is the only Windows channel and delivers the updates:

<a href="https://apps.microsoft.com/detail/9PN4VXDZ3THW?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200" style="border: 1px solid #707070; border-radius: 4.5px;"/>
</a>

For farm nodes you can install from a terminal without opening the Store:

```
winget install --id 9PN4VXDZ3THW --source msstore --accept-package-agreements
```

### Requirements

- **Windows 10 version 2004 (build 19041) or later, or Windows 11**, x64 (any edition).
- Blender installed on each node that should render (or whatever renderer your own [templates](templates.md) launch).

The Store package declares firewall rules for HTTP port **8420** and UDP port **4243** and registers a startup task, so minRender starts hidden in the tray when you sign in (toggle it in Task Manager → Startup).

{: .note }
> On render nodes, turn the Store's automatic app updates off and upgrade the farm when it is idle:
> `winget upgrade --id 9PN4VXDZ3THW --source msstore --accept-package-agreements`

### Where minRender keeps its data

Everything minRender stores on the machine lives in one per-user folder, `%USERPROFILE%\minRender` — that is `C:\Users\<you>\minRender`:

| Item | What it is |
|---|---|
| `config.json` | this node's settings: sync root, ports, tags, path mappings |
| `node_id.txt` | this node's identity on the farm |
| `minrender.db` | the queue, while this node is the leader |
| `submissions\` | hand-off folder: the Blender add-on drops job files here and minRender picks them up |
| `staging\` | frames in flight while a chunk renders with local staging on; emptied when the chunk finishes |
| `window.json` | the window's size and panel sizes |
| `no-farm\` | scratch state while the node has no farm (see below) |

The folder is deliberately outside `AppData`, which the Store package keeps private, so the Blender add-on (which runs inside Blender) can read the settings and write submissions, and so a node that is reinstalled or upgraded keeps its identity.

### Uninstall

Uninstall from **Settings → Apps** (or `winget uninstall --id 9PN4VXDZ3THW`) like any Store app. That removes the program, its startup task and firewall rules. The data folder above is left in place so your settings and node identity survive a reinstall. To remove it too, delete `%USERPROFILE%\minRender`.

---

## macOS

Download the latest release [here](https://github.com/cbkow/MinRender/releases/latest/). Requires **macOS 13 or later**.

1. Open the `.dmg` file.
2. Drag **minRender** to your **Applications** folder.
3. Launch from Applications or Spotlight.

minRender keeps its data in `~/Library/Application Support/MinRender` (the same files as in the table above).

### Updates

Once installed in **Applications**, minRender keeps itself up to date — it checks once a day and offers to download and install new versions in place. You can also check any time from **minRender → Check for Updates…** in the menu bar, or from the menu-bar icon's menu.

---

> minRender, like all render farm coordinators, lets you launch executables remotely. That behavior is dangerous outside a trusted LAN — keep it inside one, test your flow thoroughly, and practice safe computing.

## Point each node at the shared folder

A new installation starts with **NO FARM** in its title row. Click **Settings** (top right), and browse to or paste a shared directory that **every** node can reach — the **sync root**. This can be:

- an SMB share on a NAS,
- a synced folder via LucidLink, Dropbox, Synology Drive, Resilio, Syncthing, or similar.

Press **Save**; minRender restarts itself on the farm.

![minRender Settings](/images/mr1-settings.png)

Repeat on every node. As soon as two or more nodes share the same sync root, they discover each other and elect a leader.

{: .note }
> **No farm.** If the sync root is not set, or is not reachable when minRender starts (a share that is not mounted yet, a
> sync client that has not started), minRender does not give up: it shows **NO FARM** and why, looks for the folder
> every few seconds, and joins the farm by itself as soon as the folder is there.

## What gets created in the sync root

Inside the sync root minRender makes one folder, `MinRender-v2`:

| Subfolder | Purpose |
|---|---|
| `nodes/` | The phonebook — each node writes its own entry and its own log here. |
| `jobs/` | Per-chunk render logs (the renderer's own output) for each job. |
| `state/` | The leader's snapshot of the queue, for the next leader. |
| `templates/examples/` | Example templates that ship with minRender. Override one by dropping your own template with the same `template_id` into `templates/`. |
| `templates/plugins/` | The templates the submission add-on uses. |
| `plugins/blender/` | The Blender add-on — see [Submit a job](submit.md). |
| `farm.json` | The farm's shared secret. Every node of one farm holds the same one. |

## Next

Configure the node's [tags, path mappings and local staging](configure.md), then [submit your first job](submit.md).
