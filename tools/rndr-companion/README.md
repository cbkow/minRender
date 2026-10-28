# minRender RNDR companion

Runs the OTOY Render Network client on a Windows farm node while minRender
is idle, stops it the instant a render starts, and brings it back after a
cooldown. It is the dual-mode supervisor that lived inside minRender until
0.5.5, moved out of the app: the shipped binary no longer launches, kills or
even knows about the RNDR client, and this script never touches the farm.

**Not shipped with minRender.** It is a PowerShell script you install on
the nodes that should run RNDR, nothing else.

## How it follows the node

minRender 0.5.6+ writes its state to `HKCU\Software\MinRender\Node`
(`RenderState`, `NodeState`, `Pid`, `UpdatedMs`) on every transition. The
companion blocks on a registry change notification for that key, so a
render start reaches it immediately rather than on a poll. The `Pid` is
checked against the process table; a dead or zero `Pid` means minRender is
not running.

If the key is absent it polls
`GET http://127.0.0.1:<http_port>/api/status` once a second instead. The
port comes from minRender's `config.json`. The key is absent on app
versions before 0.5.6, and also on **Microsoft Store installs**: the Store
package keeps its registry writes virtualized (only
`%USERPROFILE%\minRender` is shared with other software), so the key is
visible to minRender alone and the companion runs on the HTTP poll. The
only practical difference is up to a second of extra latency before a
render start is noticed.

When minRender is unreachable the companion holds steady: it launches
nothing and leaves a running client alone, except that the DCC gate still
stops the client when a creative app appears. When the app comes back, the
startup settle applies before any launch, exactly as on a fresh start.

## minRender keepalive

Optional (`app_keepalive`, or `install.ps1 -KeepAlive`). A Microsoft Store
auto-update closes minRender and does not start it again, so a node sits
off the farm until someone logs in to it. With the keepalive on, the
companion relaunches minRender whenever no `minRender.exe` runs in its
session: after an update, a crash, or a manual quit.

- The Store package is started through the app activation manager with
  `app_args` (`--minimized`), the same way Start launches it. A loose build
  is started from `app_path` instead.
- Nothing is launched in the first `app_startup_grace_seconds` (90) after
  the companion starts, so the package's own startup task wins at logon,
  nor until minRender has been gone `app_relaunch_delay_seconds` (30),
  which covers the package being serviced mid-update.
- A failed launch, or one that dies within 5 minutes, doubles the wait
  before the next, up to `app_backoff_max_seconds` (600).
- **To keep minRender closed on purpose**, pause the keepalive:
  `reg add HKCU\Software\MinRender\Companion /v KeepAlive /t REG_DWORD /d 0 /f`
  (1 to resume). `Enabled` only pauses RNDR, not the keepalive.

`install.ps1 -KeepAlive -NoRndr` installs the companion as a keepalive only
(`rndr_enabled = false`), for nodes that do not run RNDR.

## State machine

Same as the retired in-process version.

| State | Leaves when |
|---|---|
| Inactive | after the startup settle, no creative app, launch or adopt the client → Running; render in progress → RenderActive; creative app present → kill any client that is already running (one left over from a previous companion run, or started by hand) |
| Running | render starts → kill → RenderActive; creative app appears → kill → Inactive; watchdog relaunches a dead client and kills a hung one after a grace period |
| RenderActive | render finishes → Cooldown |
| Cooldown | cooldown elapsed → launch/adopt → Running; render → RenderActive; creative app → Inactive |

The optional DCC gate (`dcc_gate`) treats a creative app running from a
Program Files install as "someone is working here": the client is stopped
while it runs and for `dcc_settle_seconds` after it closes.

## Install

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

Registers a logon task for the current user, in the interactive session,
hidden, with no run-time limit and restart-on-failure, then starts it. Run
`uninstall.ps1` to remove it; that disables the companion first so it stops
the client itself before the task is torn down.

If the node had the DCC gate on in the app itself (0.5.4/0.5.5:
`rndr_dcc_gate` / `rndr_block_processes`), the installer turns `dcc_gate`
on with the same block list. It reads the current `config.json` and the
pre-0.5.8 `%LOCALAPPDATA%\MinRender\config.json`; apps from 0.5.6 on drop
those keys whenever they save, so the old folder's copy is usually the one
that still has them. This happens once per user
(`HKCU\Software\MinRender\Companion\DccGateMigrated`) and never turns
the gate off, so re-running the installer keeps your own edits. Nodes
installed before this check: re-run `install.ps1`.

Roll it out **before** a dual-mode node upgrades to 0.5.6; after the upgrade
nothing else stops RNDR during renders.

**Upgrading a node to 0.5.8:** minRender's data folder moves from
`%LOCALAPPDATA%\MinRender` to `%USERPROFILE%\minRender`, and the first 0.5.8
launch copies the old folder (companion script, config, `rn\` client)
across. Re-run `install.ps1` after the upgrade: it moves the scheduled task
to the new folder's copy of the script and repoints a `rndr_path` that still
names the old folder. Until then the task keeps running the old copy, which
still works because the old folder stays in place. Do this **before**
deleting the old folder.

## Configuration

`%USERPROFILE%\minRender\rndr-companion.json` (created with defaults by
the installer; the task re-reads it on restart):

| key | default | |
|---|---|---|
| `rndr_path` | `%USERPROFILE%\minRender\rn\rndrclient.exe` | launcher; it spawns `TCPSVCS.EXE` |
| `startup_delay_seconds` | 120 | settle after minRender comes up |
| `cooldown_seconds` | 120 | after a render, before relaunch |
| `dcc_gate` | false | hold off while a creative app runs |
| `dcc_settle_seconds` | 600 | after the creative app closes |
| `block_processes` | `blender.exe`, `AfterFX.exe` | exact image names |
| `warmup_seconds` / `watchdog_seconds` / `hang_grace_seconds` | 15 / 5 / 15 | client health |
| `http_port` | from config.json | fallback polling only |
| `rndr_enabled` | true | false = keepalive only |
| `app_keepalive` | false | relaunch minRender when it is not running |
| `app_package` | `cbkow.minRender` | Store package identity name |
| `app_path` | empty | loose build `minRender.exe`; overrides `app_package` |
| `app_args` | `--minimized` | |
| `app_startup_grace_seconds` / `app_relaunch_delay_seconds` / `app_backoff_max_seconds` | 90 / 30 / 600 | keepalive timing |

## Operating it

- **Pause / resume** without touching the task:
  `reg add HKCU\Software\MinRender\Companion /v Enabled /t REG_DWORD /d 0 /f`
  (1 to resume). Pausing stops the client; the companion keeps running idle.
- **Log:** `%USERPROFILE%\minRender\rndr-companion.log`, one line per
  transition, truncated when it passes 2 MB.
- **Status:** `reg query HKCU\Software\MinRender\Companion` shows `State`,
  `Rndr`, `App` (with the keepalive) and `LastSeen` (ms since epoch, written every tick); `reg query
  HKCU\Software\MinRender\Node` shows what the app last published.
- **Dry run:** `powershell -File rndr-companion.ps1 -Once` evaluates the
  state machine once and prints the decision.
- The task runs only when the user is logged on and ends with the session.
