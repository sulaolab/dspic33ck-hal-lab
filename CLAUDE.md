# Project guidance for AI agents

This project's working conventions live in [`AGENTS.md`](AGENTS.md) — read it.
The point below is the one most easily gotten wrong.

> **Scope.** This describes the **maintainer's local bench** and is **not needed to
> build or use this repository** (for that: [`README.md`](README.md) and
> [`buildtools/README.md`](buildtools/README.md)). The `serial-monitor` bridge is a
> separate, not-currently-published tool. See the note at the top of
> [`AGENTS.md`](AGENTS.md).

## The monitor for this board is on 127.0.0.5, not 127.0.0.1

The UART console is owned **exclusively** by the `serial-monitor` bridge in a
sibling `serial-monitor/` directory (this repo has no copy — it sits **beside** this
repo, not inside: paths here are `../serial-monitor/...` and the profile resolver
reads `.serial-monitor.json` from the cwd, so it must sit one level up).
**Never open the COM port directly.**

Several monitors run on this PC at once — one per board, all on port 8080,
separated by loopback alias. This repo's board is **`http://127.0.0.5:8080`**
(`profile: ck`); `127.0.0.1:8080` is the Sonora dsPIC33AK board and will answer with
a perfectly healthy `/status` while being the **wrong board**.

So resolve the bind instead of assuming it, and confirm what you reached:

```powershell
python -m serial_monitor --show-config                                  # or:
pwsh ../serial-monitor/list-serial-monitor.ps1                          # is one running?
Invoke-RestMethod http://127.0.0.5:8080/status | Select profile, port    # must say ck
```

Already running → use it as-is. Unreachable → announce it and start it yourself
(`pwsh ../serial-monitor/start-serial-monitor.ps1`). Stop it only if you started it,
and only as `stop-serial-monitor.ps1 -Profile ck` — an unfiltered stop or `-All`
kills other people's boards too.

**Full instructions:**
[`../serial-monitor/AI_UART_ACCESS.md`](../serial-monitor/AI_UART_ACCESS.md)
