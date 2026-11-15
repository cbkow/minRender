---
title: Privacy
permalink: /privacy/
nav_order: 9
---

# Privacy policy

minRender is a render-farm manager that runs on machines you own, on your
own network. It is built so that nothing about you or your work leaves that
network.

## What the app does with data

- **No accounts, no sign-in, no telemetry.** The app never contacts us and
  has no analytics, crash reporting, or usage tracking of any kind.
- **Job data stays on your network.** Job names, file paths, render logs,
  and node information are written to the shared folder you choose and to
  each machine's local application data. They are exchanged only between
  your own render nodes over your LAN or VPN.
  If the shared folder you choose is a synced folder (Dropbox, Synology
  Drive and the like), that service stores what is in it — including the
  farm's shared secret — under its own terms.
- **Network use.** Nodes talk to each other over HTTP on a port you
  configure and discover each other with local multicast. Those connections
  are confined to your network.
- **Update check.** The macOS build checks a file on our website for new
  versions and downloads an update when you ask it to. That request carries
  no personal information. The Windows build does not do this; the
  Microsoft Store handles updates.
- **Render applications.** minRender launches the render software you have
  installed (Blender, or whatever your templates launch). Its own network
  behaviour and privacy terms are its makers', not ours.

## Data we hold

None. We operate no servers that receive data from the app.

## Changes

If this ever changes, this page will be updated and the change noted in the
release notes.

## Contact

Questions: open an issue at
[github.com/cbkow/MinRender](https://github.com/cbkow/MinRender/issues).

_Last updated 2026-11-15._
