# minRender

A lightweight render farm coordinator for small VFX teams and freelancers.

![minRender](docs/images/mr001.jpg)

- **Self-healing mesh** — every node can act as coordinator. If the leader drops, another takes over automatically.
- **Blender, out of the box** — a submission add-on for Blender and the templates it uses ship with the application. JSON templates define how to launch any command-line renderer.
- **Simple setup** — install, point every node at a shared folder, done. A node whose share is not there yet waits for it and joins by itself.
- **Fast discovery** — UDP multicast for LAN, falls back to a file-system phonebook for VPNs and complex networks.
- **HTTP coordination** — job dispatch, progress tracking, and completion reporting over an HTTP mesh.
- **Local staging** — opt-in render-to-local-then-copy mode to prevent file corruption from cloud sync tools.
- **Live queue control** — drag jobs to reorder the queue, change priority mid-flight, edit a running job, and cancel kills the renderer's whole process tree on every node, instantly.
- **Resilient** — each node keeps a SQLite snapshot of the leader's state. If the leader drops, a new one picks up where it left off.
- **Windows and macOS**, in one farm.

## Install + docs

Full manual: **[minrender.com](https://minrender.com)** — install, configuration, job submission, monitoring, and the job template reference.

### Windows

<a href="https://apps.microsoft.com/detail/9PN4VXDZ3THW?referrer=appbadge&mode=full" target="_blank" rel="noopener noreferrer">
  <img src="https://get.microsoft.com/images/en-us%20dark.svg" width="200"/>
</a>

Requires Windows 10 (version 2004 or later) or Windows 11, x64. The Microsoft Store is the only Windows channel and delivers the updates.

### macOS

Signed and notarized `.dmg` from [GitHub Releases](https://github.com/cbkow/MinRender/releases/latest). The application keeps itself up to date.

## Source

minRender 1.0 is written in Rust on the same platform as QCView and minNotes. Its source, build and packaging scripts
are in **[cbkow/minplatform](https://github.com/cbkow/minplatform)** (`apps/mr`); a release is the tag
`minrender-v<version>` there. This repository holds the site, the manual and the macOS update feed.

minRender 0.5.x (C++ / Qt, with its After Effects and Cinema 4D plug-ins and templates) is frozen on the
[`0.5.x`](https://github.com/cbkow/MinRender/tree/0.5.x) branch. A 1.0 node works in one farm with 0.5.10 and 0.5.11
nodes, so a farm can be upgraded one machine at a time.

## License

minRender is free software under the GNU General Public License, version 3 or later. See [LICENSE](LICENSE).
