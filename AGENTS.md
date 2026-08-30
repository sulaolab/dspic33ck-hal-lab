# Working conventions for AI agents

> **Scope.** Everything below describes the **maintainer's local bench** — several
> boards on one PC, each with a serial bridge in front of it. **None of it is
> needed to build or use this repository**; for that, see
> [`README.md`](README.md) and [`buildtools/README.md`](buildtools/README.md).
> The `serial-monitor` bridge it refers to is a separate, not-currently-published
> tool, so treat this file as a description of how the hardware results in
> `docs/` were obtained rather than as instructions you can follow. The one rule
> that generalises: **whatever holds your board's COM port, do not open that port
> from a second process.**

## Board serial / UART console — do NOT open the COM port

This board's UART console is owned **exclusively** by the `serial-monitor` bridge,
which lives once in a **sibling `serial-monitor/` directory** (this repo carries no
copy of it — it sits *beside* this repo, not inside: paths here are
`../serial-monitor/...` and the profile resolver reads `.serial-monitor.json` from
the cwd, so it must sit one level up).
A COM port can only be held by one process, so opening it directly —
`pyserial`, a serial terminal, any script — either fails with "port busy" or steals
the port and breaks the monitor and any human watching in Tera Term.

### This repo's monitor is NOT on 127.0.0.1

Several monitors run on this PC at the same time, one per board. They all listen on
port **8080** and are separated by **loopback alias**:

| Board | bind |
|---|---|
| **this repo (dsPIC33CK kit, `profile: ck`)** | **`http://127.0.0.5:8080`** |
| Sonora dsPIC33AK boards (`profile: sonora`) | `http://127.0.0.1:8080` |

So `127.0.0.1:8080` answers with a healthy `/status` **while being the wrong
board**. That is the failure mode to avoid, and it does not announce itself.

Two habits prevent it:

1. **Resolve, don't assume.** [`.serial-monitor.json`](.serial-monitor.json) in this
   repo declares the board; the resolver reads it by walking up from wherever you
   are:
   ```powershell
   python -m serial_monitor --show-config          # PYTHONPATH=../serial-monitor if needed
   pwsh ../serial-monitor/list-serial-monitor.ps1  # also says whether one is running
   ```
2. **Confirm before you send.** `GET /status` must report `profile: ck` and the COM
   port you expect:
   ```powershell
   Invoke-RestMethod http://127.0.0.5:8080/status | Select profile, port, connected
   ```

Never hardcode a base URL in this repo's docs or scripts.

### Talking to the board

- `GET /status` — check `connected` first (and `profile`, per above)
- `POST /command {"cmd":"..."}` — send one console line
- `POST /wait {"contains":"...","timeout":N}` — block until a marker appears
  (use this instead of sleeping; note it only matches bytes seen *after* the call)
- `GET /log?tail=N` — recent RX/TX lines

If it is **already running, use it as-is** — don't start a second one, don't restart
it. If it is unreachable, say so and start it yourself; starting the monitor is the
sanctioned path, not the forbidden direct COM open:

```powershell
pwsh ../serial-monitor/start-serial-monitor.ps1     # bind comes from .serial-monitor.json
```

**Only stop it if you started it**, and only ever scoped to this board:

```powershell
pwsh ../serial-monitor/stop-serial-monitor.ps1 -Profile ck
```

An unfiltered stop or `-All` also kills the monitors other people are using on
other boards. Prefer Ctrl-C in the monitor's own window (graceful: port released,
logs flushed). Logs live in `serial-monitor/serial_monitor/monitor_logs/ck/`, not
in this repo.

### Behaviour worth knowing

- **Every TCP client may type.** Any number of Tera Terms can be attached and all of
  them can drive the board; avoiding collisions is the operator's business. There is
  no single-writer lease.
- **Tera Term must use Service = Other** (`/T=0`) — Telnet rewrites a bare CR as
  `CR NUL` and a data `FF` as `FF FF`, which read as extra console input and
  corrupt an XMODEM transfer. Nothing blocks it, though: every client gets a
  banner on connect telling it to use Service = Other, and that is the whole
  enforcement. Detection was removed on 2026-08-09 after it disconnected a healthy
  transfer 85 blocks in, so a Telnet transfer now just fails and the board is not
  at fault.
- **While a transfer holds the transmit gate**, every writer is held off for its
  duration: TCP clients get a notice and `POST /command` returns **409**. It reopens
  for everyone the moment the transfer ends.

**Canonical instructions (full):**
[`../serial-monitor/AI_UART_ACCESS.md`](../serial-monitor/AI_UART_ACCESS.md).
Bind resolution in detail:
[`../serial-monitor/docs/binding-and-profiles.md`](../serial-monitor/docs/binding-and-profiles.md).
This file is a pointer — if the rule changes, change it there.
