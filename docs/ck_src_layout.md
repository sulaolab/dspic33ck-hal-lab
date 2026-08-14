# `src/` layout: who owns what

Reorganised 2026-08-02. What changed and why, plus the one thing deliberately
left alone.

> **Namespace note (2026-08-09).** Six HAL modules -- `adc`, `dma`, `gpio`
> (with `pps`), `i2c`, `reset`, `uart` -- were renamed into the NORA namespace:
> `dspic33ck_<mod>*` -> `nora_<mod>*`, with the silicon-specific backend FILES
> carrying the `_dspic33ck` tag (`nora_i2c_dspic33ck_master.c`). The one declared
> exception -- `nora_dma.h` reaching the CK register layer and publishing
> `NORA_DMA_DSPIC33CK_STAT_*` -- was **closed on 2026-08-10**: the header is now
> the portable contract with no SFRs and no status-bit macros, and the measured
> inline SFR path moved to the opt-in sibling `nora_dma_dspic33ck_fast.h`
> (`_hot` names, each with an out-of-line twin).
> `timer` joined them on 2026-08-09 (`nora_tick_timer*`, `nora_high_res_timer*`),
> and `clock` on 2026-08-10 -- but that one was not a rename. `nora_clock.h` here
> is a VENDORED, byte-identical copy of the AK owner's contract header, and the CK
> files were rewritten to implement it: `nora_clock_dspic33ck.{h,c}` (backend),
> `nora_clock_dspic33ck_reg.{h,c}`, `nora_clock_device_dspic33ck.{h,c}`,
> `nora_clock_dspic33ck_bringup.{h,c}`. The contract is the reference and a CK-side
> improvement to it goes to the AK owner rather than into the copy.
> `spi_i2s_tdm` still carries the old prefix. Backend-internal
> silicon-specific names (SFR masks, register helpers, capability macros inside a
> backend `.c`) are **kept permanently** as `DSPIC33CK_*` -- they are backend
> names and that is the finished answer, not a rename waiting to happen: the goal is
> the public/backend boundary, not the absence of the string. Sentences below that describe the
> **current** tree use the new names; passages recording what a past step did
> keep the names that step used, because that is what those commits contain.

## The directories

```
src/
  main.c              the only main(). Four-phase sequence shared by both boards.
  profile_main.h      the hooks each board implements (see src/main.c).

  boards/<name>/      one board's pins, clock, fuses and its own app code
    dm330030/           DM330030 Curiosity      (dsPIC33CK256MP508)
    ev88g73a/           EV88G73A Curiosity Nano (dsPIC33CK64MC105)

  chip_drivers/       external chips. Board-independent.
  app/                board-independent services, demos, traps
  uart_app/           the console: parser, dispatcher, one file per module letter
  uart_platform/      the console's transport side: the output seam, stdio retarget
  hal_*/              the HAL proper, one directory per peripheral
```

## The split axis is chip-vs-board, not which-board

`chip_drivers/` is deliberately NOT divided per board. `wm8904.c` is built for
both targets, contains zero pin or port references, reaches I2C1 through a CMSIS
driver handle, and already has `wm8904_port.h` as the seam for board/lab choices.
Dividing it by board would have needed a `shared/` exception on day one.

`app/timer_1ms.c` sits in neither: no board-specific facts, not a chip driver.
What it is is a ten-client callback multiplexer, and since 2026-08-03 it is layered over
`app/timer_app.c` rather than over the tick-timer HAL directly — `timer_app` owns the
running time and the Timer1 vector for both boards, because the board that actually exists
excludes `timer_1ms.c` from its build.

## The console left `boards/ev88g73a/`, then left `app/` too

Both the console and the traps started in `boards/ev88g73a/`, and both turned out to
be board-independent once the right seams existed. The test applied to each module was
the same: grep it for a pin, a port, a board register. What came back was **four
values**, not code.

They landed in `app/` first, and eleven console files there made `app/` unreadable:
21 files covering three unrelated things. So the console got its own two directories,
named after `dspic33ak-audio-dsp-sonora`'s, which splits the same way:

```
uart_platform/           the transport side
  console_out.h            output+input seam; one small file per board implements it
  uart_platform_stdio.{c,h}  console UART init + the libc write() hook (printf)

uart_app/                the command side
  app_console.{c,h}        the line parser, vendored from sonora   
  console_dispatch.c       app_onmsg(): switch on the module letter, nothing else
  general_console.c        module 'g'  ?gv build ID, ?gh help
  diag_console.c           module 'd'  ?dt  (tests the HAL's high-res timer)
  system_console.c         module 's'  *sr / ?sr
  traps_console.c          module 'x'  ?xl, *xa / *xm / *xs
  console_task.{c,h}       poll loop, the one help text, the TX-drain wait

app/                     what is left: services, demos, diagnostics, and the traps
  app_traps.{c,h}          the eight trap vectors, the persistent latch, the report,
                           and the BOARD SEAM -- the non-text facts app/ and uart_app/
                           need from a board (was board_seam.h until 2026-08-06)
  gain_ctrl.{c,h}          Q31 mute/gain ramp
  dsp_load.{c,h}           block-ISR load vs the measured block period
  dma_selftest.{c,h}       RAM->RAM proof that the DMA controller moves data
  i2c_probe.{c,h}          does the I2C module work, with or without a device attached
  timer_app.{c,h}          the 1 ms RUNNING TIME: GetTicks(), and the one _T1Interrupt
  timer_1ms.{c,h}          the inherited demo's ten-client tick registry, now a hook
                           layered on timer_app (DM330030 only -- ex="true" on EV88G73A)
  demo_*  app_config.h
```

Two notes on where the boundary really is. First, `uart_platform/` is thin here (two
files) because CK's actual port code is elsewhere and belongs there: `hal_uart/` owns
the peripheral, and each board owns its `*_console_out.c`. Sonora's
`uart_platform/` is bigger only because it also carries per-UART device files that CK
does not have. Second, the traps stayed in `app/`: eight exception vectors are not a
UART concern. Only their console face (`traps_console.c`) is.

Names follow sonora where the file has a counterpart there — `app_console.*`,
`general_console.c`, `diag_console.c`, `system_console.c`, `uart_platform_stdio.*` —
so the same grep finds the same thing in both repos. Where there is no counterpart the
CK name stays: `console_dispatch.c` (sonora's switch lives inside its
`app_debug.c`, which CK has no equivalent of), `console_task.{c,h}`, `console_out.h`.
`traps_console.c` follows the *pattern* rather than a file, since there is no `'x'`
module upstream.

| deleted | replaced by |
|---|---|
| `boards/ev88g73a/ev88g73a_console.{c,h}` | `uart_app/console_task.*` + `uart_app/system_console.c` + `uart_app/traps_console.c` |
| `boards/ev88g73a/ev88g73a_traps.{c,h}` | `app/app_traps.{c,h}` |
| `boards/dm330030/traps.c` | `app/app_traps.{c,h}`, policy `APP_TRAPS_POLICY=2` |

What stayed on the boards is the seam and nothing else: `console_out.h`'s six
functions (`*_console_out.c`), the BOARD SEAM's four (the latched reset cause, and
the two addresses the trap tests need — where RAM ends and a stack pointer past
`SPLIM`, which are per-part, so only the board's linker script knows them).

Why functions rather than macros in a board header: macros would put
`#if defined(DSPIC33CK_BOARD_...)` back into the shared code, so every new board would
edit files in `app/` and `uart_app/` — the coupling the seam exists to remove. The cost
is that two constants arrive at runtime; both are used once, on a path that ends in a
trap.

The by-product is that DM330030 now has a console interpreter's worth of commands
compiled in and one unimplemented seam away from working: it answers
`console_in_ready()` with `false`, because its RX is on the HAL's ISR ring rather
than polled. That is a board fact, and it is all that is missing.

See [`ck_silicon_findings.md`](ck_silicon_findings.md) for the trap policy and why it has
no default.

## Four more modules left `boards/ev88g73a/` (2026-08-02)

Same test as the console: grep each file for a pin, a port, a board register. What came
back was values, and in two cases nothing at all.

| was | now | what was actually board-specific |
|---|---|---|
| `ev88g73a_gain_ctrl.{c,h}` | `app/gain_ctrl.{c,h}` | **nothing** — no pins, no SFRs, no HAL calls at all. Pure Q31 arithmetic over a caller-owned struct; only the names said EV88G73A |
| `ev88g73a_dsp_load.{c,h}` | `app/dsp_load.{c,h}` | **nothing** — it reads the transport HAL's load counters and the high-res timer. It was already shared between that board's two audio profiles, which was half the argument; the other half is that none of it is EV88G73A's |
| `ev88g73a_dma_selftest.{c,h}` | `app/dma_selftest.{c,h}` | the DMA **channel number**, which is not a board fact either but an allocation the application makes (the transport owns 0 and 1) — so it became the argument |
| `ev88g73a_i2c1_probe.{c,h}` | `app/i2c_probe.{c,h}` | four values (instance, two RP numbers, target address, bus rate) |

`boards/ev88g73a/` is down to 8 files from 15: the board adapter, its fuses, its console
output seam, its `main.c` profile, and the WM8904 audio module (which was next, and was
indeed a comparison against `app/demo_wm8904_audio.c` rather than a move — the two were
merged the same day, see below, leaving 6 files).

Two by-products worth naming:

**The pins stayed behind, properly.** `ev88g73a_i2c1_init()` — ASDA1/ASCL1 analog-off,
weak pull-ups, HAL init at 400 kHz — moved into `board_ev88g73a.c`, which is where a pin
number belongs. The shared probe reaches it through `i2c_probe_t.bus_init`, and the
WM8904 path calls it directly (same bus, same pins). So the probe module contains no pin
number at all, and the board contains no probe logic.

**The I2C status table moved into the HAL.** `nora_i2c_status_str()` now lives with
the enum in `hal_i2c/`, replacing a private eleven-case table inside the probe. Any
caller reporting an I2C result wants the same strings — `wm8904.c` prints bare integers
today — and a `switch` beside the enum stops compiling silently when a status is added,
which a caller's private copy does not.

Also, 61 calls to `ev88g73a_uart_write_*` across the four files became `console_out_*`,
i.e. they now go through the seam that already existed rather than around it. That was
the real defect class in this directory. What remained at that point was
`ev88g73a_wm8904_audio.c`'s own 22 calls plus `main.c`'s, left because those files are
board-owned, where naming the board's UART API is not a layering error the way it was in
shared code. That reasoning still holds — but the API they were naming has since gone
away entirely; see the next section.

Cost, measured: DM330030 grew 44,748 -> 48,804 bytes (16% -> 18%) because all four now
compile into both configurations while only EV88G73A calls them. Deliberate, and the
opposite call from the ADC HAL above — there the dead code landed on the 64 KB part,
where 852 bytes mattered; here it lands on the 256 KB part, and what it buys is
compile-and-link coverage of shared modules against the second device (the I2C, DMA and
transport HALs have per-part register tables). EV88G73A is unchanged at 45,465 (+3 bytes,
the DMA channel number now being printed rather than a literal).

Verification reach, stated rather than implied: `dma_selftest` and `i2c_probe` are
exercised on hardware (see the commits). `gain_ctrl` and `dsp_load` both run in the audio
block ISR, which needs a codec that is not attached, so they have build-and-link coverage
plus the parts observable without one — `dsp_load_reset()` runs at start-up and the
high-res timer it depends on reads sanely (`?dt`, and the boot line `load monitor on
SCCP1 @ Fcy 100000000 Hz`). The load figures themselves are unverified since the move.

## One WM8904 audio path for both boards (2026-08-02)

`app/wm8904_audio.{c,h}` replaces two files that were doing the same six-step bring-up:

| was | lines | the problem with where it lived |
|---|---|---|
| `boards/ev88g73a/ev88g73a_wm8904_audio.c` | 411 | about 45 lines of it were board facts; the rest was application |
| `app/demo_wm8904_audio.c` | 213 | **not board-independent at all** — it included `board.h` and called `board_mikrobus_a_*`, so it compiled only for DM330030 and was `ex="true"` in the other configuration |

So the tree had the same defect in both directions at once: application code sitting in
`boards/`, and board code sitting in `app/` behind a name that denied it. That is the
`bsp/` lesson (below) recurring, and the reason to look past "does it build".

Measured before merging, the two differed in seven places and agreed everywhere else —
identical bring-up order, identical TDM8/32-bit geometry, identical SPI polarity trio
(`SPIFE=0`/`CKP=1`/`CKE=0`), error messages nearly word for word. The seven became the
port and the config:

| difference | how it is expressed now |
|---|---|
| MCLK: DM must divide one out of REFO1, EV's codec self-clocks from its X1 | `port->mclk_init`, **NULL on EV** — a fact, not an omission. **[FALSE -- see the 2026-08-04 MCLK section; both are deleted]** |
| pins: EV has four RPs by direct jumper, DM has a mikroBUS-A helper | `port->configure_pins(role)` |
| I2C rate: 100 kHz probe vs 400 kHz audio on DM | `port->i2c_init` — the rate stays with the wiring, see below |
| role: EV switches at build time, DM is fixed slave | `cfg.dspic_is_master` (+ `cfg.brg` for the master case) |
| mute: EV has SW0 and a Q31 ramp, DM has neither | `port->mute_button_pressed` + `cfg.gain_ramp_ms` (either absent ⇒ plain-copy callback) |
| status cadence: EV's own report gate vs DM's screen-repaint loop | `cfg.status_period_ms` (**0 on EV** — its `profile_wait_next_tick()` already gates at 2 s, and two stacked periods multiply; **20000 on DM**, whose loop has no gate) |
| idle explanation: EV prints why nothing started, DM stays silent | `cfg.idle_report_period_ms` (0 = silent) |

**The board is no longer asked which side is master.** EV's old pin function was a `#if`
on the build switch and returned false for the role it had not been compiled for — the
board rejecting a decision it does not own. `ev88g73a_tdm_pins_init()` now switches on
the role it is handed, and the same wiring serves either topology. **At that stage** DM's
adapter still refused MASTER, but already for a stated wiring reason (REFO1 already supplies
the clock) rather than because of how it was compiled. That refusal is gone too:
`dm330030_tdm_pins_init(role)` passes the role straight to the shared pin helper and
supports both, and the profile states its *choice* beside `.dspic_is_master = false` in
`boards/dm330030/main.c` — see the later section on where that wrapper went.

**Two I2C rates on DM — SUPERSEDED, see "One codec, one set of names" below.** This
section used to argue for keeping `board_mikrobus_a_i2c1_init()` (100 kHz, the probe)
and `board_mikrobus_a_i2c1_audio_init()` (400 kHz, the audio path) as separate public
wrappers, on the grounds that each caller should keep the rate it was written with. The
evidence it cited actually points the other way and is left here for that reason: on the
AK fleet 400 kHz works where 100 kHz *fails*, in hardware. So the 100 kHz path was the
known-bad one, and there is now a single `dm330030_i2c1_init()` at 400 kHz. The rate is
still a board fact, which is why it is not a field in `wm8904_audio_config_t`.

Cost, measured, and it is not free:

| build | before | after | |
|---|---|---|---|
| EV88G73A default (audio on) | 45,417 | **46,290** | +873 (69%) — the port/config indirection, the plain-copy branch it does not use, and the wiring string |
| EV88G73A baseline (audio off) | 45,306 | **45,948** | +642 of pure dead code: the shared module has no internal `#if`, so it links even where no profile calls it |
| DM330030 default (audio off) | 49,029 | **51,282** | +2,253, same reason — the old file was `#if ENA_WM8904_AUDIO`'d down to nothing |
| DM330030 + `ENA_WM8904_AUDIO=1` | 50,406 | **51,519** | +1,113 |

The dead-code lines are the honest cost of one implementation: the module cannot gate
itself on `EV88G73A_ENABLE_WM8904_AUDIO`/`ENA_WM8904_AUDIO` without learning both boards'
switch names, which is the coupling it exists to remove. It is affordable today (EV's
tight case is the 69% default, and the baseline variant is a diagnostic build) and the
lever if it stops being affordable is an `ex="true"` in the configuration that does not
use it — the same call as the ADC HAL.

**Hardware-verified on EV88G73A** to the same depth as before the merge — with no codec
attached, that means the graceful-abort path: the new wiring line prints, the SCCP1 load
monitor comes up, `wm8904_init()` fails its ID readback, the module reports and stays
stopped, and the periodic explanation repeats. Measured cadence **39.8 s**, matching the
pre-merge behaviour exactly.

That cadence is worth recording, because the first merged build got it wrong at 10 s: the
predecessor's comment claimed "~10 s between reminders" while its countdown ticked on
reports (~2 s each), not on polls (~500 ms) — so it actually ran at 40 s, and the comment
had been wrong by 4x for as long as it existed. Behaviour kept, comment fixed.

Not verifiable on any hardware available: audio itself, in either role, on either board.
The codec is not wired and DM330030 has no board.

## EV88G73A's seven `ev88g73a_uart_*()` functions are gone (2026-08-02)

The survey that ended them, counting call sites rather than reading intentions:

| function | `main.c` | `wm8904_audio.c` | `console_out.c` |
|---|---|---|---|
| `..._write_string` | 23 | 22 | 2 |
| `..._write_u32` | 10 | 5 | 1 |
| `..._write_hex16` | 0 | 0 | **1 (only caller)** |
| `..._rx_ready` / `..._read_byte` / `..._tx_done` | 0 | 0 | **1 each (only caller)** |
| `..._uart_baud` | 1 | 0 | 0 |

Two findings, of different kinds:

**The seam was implemented twice.** Four of the seven functions existed solely to be
delegated to by `ev88g73a_console_out.c`, while the other three were called 45 times
from this board's own files — so the console's output had two names for one UART, and
the board's own code went around the seam the shared modules go through. DM330030 has
never had this, because its board code and its `console_out` implementation are both
`printf`.

**The formatting was in the wrong file.** `write_u32` (decimal digit generation) and
`write_hex16` (fixed-width hex) contain no board fact whatsoever. The only board fact
in the whole group was *which UART*, and that is now a value —
`EV88G73A_CONSOLE_UART_INST` in `board_ev88g73a.h` — used by exactly two files: the
transport and the peripheral bring-up.

So all seven were deleted, their bodies becoming `ev88g73a_console_out.c`'s
`console_out_*` / `console_in_*` directly on `hal_uart`, and the 61 call sites in
`main.c` and `ev88g73a_wm8904_audio.c` became `console_out_str/_u32`.
`ev88g73a_uart_baud()` did not move at all: it returned a static holding what the
board *asked* for (`EV88G73A_UART_BAUD_FAST/SAFE`), whereas
`nora_uart_get_baudrate()` returns what the HAL *applied* — and since the banner
prints that figure precisely so a garbled console can be diagnosed, the wish was the
wrong number to print.

`board_ev88g73a.c` keeps the UART **bring-up** (pins, PPS, clock-derived baud). It no
longer moves bytes.

Cost: EV88G73A 45,465 -> **45,417** bytes (-48; three forwarders and an accessor
removed, one two-byte scratch buffer in `console_out_char` gone). DM330030 unchanged
at 49,029. Built clean in the EV default, the no-audio baseline and the I2C1-probe
variant, plus DM330030.

**Hardware-verified on EV88G73A** (build `c6f203f-dirty-...-2fd9f4974f`), one command
per primitive rather than "the console still works":

| primitive | evidence |
|---|---|
| `console_out_str` / `_u32` | boot banner, `?gh` help, `?dt` sample figures |
| `console_out_char` | the per-character echo of every command typed |
| `console_out_hex16` | `?sr` -> `reset cause this boot: EXTR(MCLR)  RCON=0x0083` |
| `console_out_idle` | `*sr` -> the acknowledgement survived, then `Reset = SWR(software reset)`. This is the one that fails silently if `tx_done` is wrong: the ack is discarded by the reset and the command looks ignored |
| `console_in_ready` / `_read` | commands were received at all |
| HAL baud, replacing `ev88g73a_uart_baud()` | banner `Baud     = 230400` (a broken read would print 0) |

## `boards/dm330030/main.c` went back to being hooks (2026-08-02)

412 lines, of which about 380 were application code: the inherited Microchip
pot/RGB/button demo, its button debounce, and a private I2C probe. It is now 165
lines and holds the five profile hooks plus one config struct, matching
`boards/ev88g73a/main.c`.

| was, in `main.c` | now | why there |
|---|---|---|
| the pot/RGB/button demo, ~300 lines | `boards/dm330030/demo_rgb_pot_buttons.{c,h}` | **split, not promoted.** The move test returns three board APIs — `dm330030_led3_rgb_set_color()`, `dm330030_led_set()`/`dm330030_sw_pressed()`, `dm330030_pot_read()` — not values. `app/` would need a four-hook seam (`read_pot`/`set_rgb`/`sw_pressed`/`led_set`) whose only implementer would be this board, permanently: no other board here has an RGB LED, a pot and three buttons. A seam with one possible implementation removes no coupling |
| `ButtonDebounce()`, 77 lines | `app/button_debounce.{c,h}` | passes the test outright: no pin, no port, no board register. What stayed with the caller is which button and what a press means |
| `I2cWm8904Probe()`, 36 lines | **deleted** — `app/i2c_probe.c` already did it | it was the weaker of two copies (see below) |

**The tick registrations moved with the demo, and that was the real finding.** The
two `TIMER_RequestTick()` calls — the RGB soft-PWM modulator and the button sampler
— were sitting in `profile_bring_up()`, i.e. the demo's clients were registered by
the board's bring-up phase. The board owns the tick SOURCE
(`TIMER_SetConfiguration()`, still in `profile_bring_up()`); its clients belong to
whoever needs them.

**The probe deletion also fixed a defect** — a real one, though **not for the reason stated
here until 2026-08-03**. The private copy called `dspic33ck_i2c_init()` directly and never
went through the board's pin stage at all. This text used to add that "on MP508 those pins
really are analog-capable, unlike MC105", so the bus was driven through pins still configured
as analog inputs; measured against both DFP headers, that is wrong — `ANSELC` implements bits
0–3, 6 and 7 on **both** parts, so RC8/RC9 have no `ANSEL` bit on either and that particular
consequence never existed. The defect that did is the one that survives: a stage bypassed is
a stage whose *other* work (and its future work) is skipped. The
new board-side hook `board_mikrobus_a_i2c1_init()` (pins, then HAL at 100 kHz) is
what `i2c_probe_t.bus_init` reaches, exactly mirroring `ev88g73a_i2c1_init()`. Three
further differences all favoured the shared probe: it separates `ERR_NACK` (the
module passing with nothing attached) from the inconclusive TIMEOUT/BUS/COLLISION
bucket instead of printing one `probe failed (status=%d)` for all of them, it names
the status through `nora_i2c_status_str()`, and it reports which pins it drove.
The rate stayed 100 kHz rather than being unified with the 400 kHz the audio path
and EV88G73A use: this board's wiring has never been verified, and the measured fact
on the AK fleet is the counter-intuitive one (400 kHz works, 100 kHz fails), so the
rate is worth keeping as its own data point. Only `i2c_probe_run()` is wired, from
`profile_start()`, not `i2c_probe_poll()` — EV88G73A polls so a scope keeps getting
a fresh START/ADDR/NACK, while here the foreground is a full ANSI screen repaint.

**A missing call surfaced on the way through: `board_user_io_pins_init()` had no
caller anywhere in the tree.** LED1/LED2/RGB were never configured as outputs and
SW1..SW3 never got their pull-ups. It was masked for as long as the vendor headers
existed, because `LED1_On()` and `BUTTON_S1_IsPressed()` rewrote `TRIS` on every
call — the very defect `led_sw.h` was written to remove (see below). Removing them
took away the only configurer, and nothing failed loudly: the pins keep their reset
state, so the LEDs stay dark and the buttons read a floating pin. It is now called
from `SYSTEM_Initialize()`, beside `board_uart1_pins_init()`, with the same
`volatile bool g_*_pins_init_ok` observability. **Unverified on hardware** — this
board's HW verification is deferred, so this is a code-reading fix, and it is the
first thing to check when the board is next powered.

Two more corrections: a "Getting Started" comment pointing at a `readme.txt` that
does not exist in this repo, and "Required baud rate: 38400", which stopped being
true when the console baud came from the Clock HAL. The surviving copy of that note
sits in `demo_rgb_pot_buttons.c` where the printing is, and says 230400.

Cost, measured: DM330030 48,804 -> 49,029 bytes (18%, unchanged as a percentage);
the +225 is the debounce instances' indirection through function pointers where the
original had two inline counters. EV88G73A is byte-for-byte unchanged at 45,465,
because both new files are `ex="true"` in that configuration — the same call as the
ADC HAL and the opposite of the four modules above: here the dead code would land on
the 64 KB part, which is where bytes matter. Build coverage checked in all four DM
variants (baseline, `ENA_I2C_WM8904_PROBE`, `ENA_WM8904_AUDIO`,
`ENA_TDM_MASTER_LOOPBACK`) plus the EV88G73A default, no warnings.

## One board-layer design, two boards (2026-08-02)

`boards/ev88g73a/board_ev88g73a.c` and `boards/dm330030/board.c` were written by
different hands and solved the same problems along different axes. They are staying as
separate files — one board each, no merge — but the *shape* is now one design, taking
whichever side had the better answer.

| axis | EV88G73A had | DM330030 had | adopted |
|---|---|---|---|
| pin facts | macros inside the `.c`, unreachable | `board_pins.h`, one file, with wiring comments | **DM's** |
| bring-up order | inside `board_init()` among the details | isolated in `system.c` with the reasons | **DM's** |
| pin configuration | `config_digital_input()` then a separate `set_pull()` | one `dspic33ck_gpio_config_t` describing the pin | **DM's** |
| pin-config results | discarded, every call `(void)` | checked, returned as `bool` | **DM's** |
| bring-up evidence | `volatile <hal>_status_t` — the reason | `volatile bool` — only pass/fail | **EV's** |
| clock failure | LOCK + achieved Fosc checked, fall back to FRC, drop the baud | return value discarded, no fallback, fixed baud | **EV's** |
| reset cause | latched and cleared first thing | read live, cannot name a cause | **EV's** |
| `dma_global_init()` | called | **never called** | **EV's** |
| profile switches | in the board header | in `app/app_config.h` | **DM's** |

### Three defects the mismatch was hiding

**DM330030 never called `dspic33ck_dma_global_init()`.** Its WM8904 audio path drives
SPI1 through `hal_spi_i2s_tdm`, whose own note says "main() calls
`nora_dma_global_init()` once at startup" — and `board_ev88g73a.c` does. So the
SRAM-bus priority (`MSTRPR.DMAPR`) the transport assumes was left at its reset value.
Nothing failed loudly, which is why it survived: the effect is a timing margin, not a
compile error.

**DM330030 could not tell that its PLL had failed.** `CLOCK_Initialize()` discarded the
HAL's return, never looked at `LOCK` or the achieved Fosc, and the console baud was a
constant. An unlocked PLL runs at an undefined frequency, so the console would garble
exactly when the board most needed to explain itself. It now checks all three
conditions, falls back to the FRC, and drops the console to a rate the fallback clock can
resolve — the policy EV88G73A already had, and had learned the hard way.

That last part needed a mechanism change: `uart_platform_stdio_init()` now takes the
baud rate as an argument instead of holding a `230400` constant. The platform owns the
mechanism, the board owns the policy — which is the shape EV88G73A's own UART bring-up
already had, since only the board knows whether its clock arrived.

**EV88G73A discarded every pin-configuration result.** LED0, SW0 and the UART1 routing
were all `(void)`. They are checked now and published as
`g_ev88g73a_uart1_pins_init_ok` / `g_ev88g73a_user_io_pins_init_ok` — deliberately the
same names DM330030 uses, because two boards answering the same question should not need
two vocabularies. SW0 also became a single `dspic33ck_gpio_config_t` including its
pull-up, rather than an input config plus a separate `set_pull()` that could be
forgotten: that pull is load-bearing, and forgetting it in a second call is precisely how
the vendor LED/button headers went wrong.

### The pins header, and the copy it removed

`ev88g73a_pins.h` now holds every pin number this board uses, with no code and no policy
— rates, baud, clock targets and feature switches stay where their rationale is. The
motivating cost was concrete: `main.c`'s audio port describes its wiring in the boot
report, and with the RP numbers unreachable inside the `.c` that description had been
typed out by hand. `EV88G73A_AUDIO_WIRING_STR` now sits beside the numbers it describes.

### The agreed skeleton (Phase 3-4, NOT yet applied)

Three roles per board, divided by **when each matters** — a rule a later reader can
re-derive, which is why it is three and not five:

```
boards/<b>/
  <b>_pins.h        facts. Compile time only, zero lines of code
  <b>_board.c/.h    bring-up. Once, in order: routing, clock policy, BOARD SEAM impls
  <b>_io.c/.h       runtime. Called every iteration, owns no init
  config_bits.c     fuses (already identical in shape on both boards)
  <b>_console_out.c the console seam (left alone: the fleet greps it by this name)
  main.c            the profile, and now also its own build switches
```

Two decisions recorded with it:

- **A device that owns a state machine keeps its own file.** DM330030's `led3_rgb.c`
  drives a soft PWM from the 1 ms tick; `led_sw` and `pot` are plain accessors. So role
  three may split per device when the device has state — not otherwise.
- **`board_*` is reserved for the BOARD SEAM (`app/app_traps.h`).** DM330030's private functions shared
  that prefix (`board_uart1_pins_init()`), so nothing in the name said which was the
  shared contract and which was one board's internals.

### Phase 3, done: the prefix now says who owns it

`board_*` / `BOARD_*` mean "a seam every board implements"; a board's own names carry its
own prefix. Applied to DM330030 (EV88G73A already worked this way):

| was | now |
|---|---|
| `board_ports_digital_default()` `board_uart1_pins_init()` `board_user_io_pins_init()` | `dm330030_*` (same shape). The first of the three was **deleted on 2026-08-03** — see the ANSEL-sweep section near the end. |
| `board_mikrobus_a_{spi1_tdm_client_pins_init,mclk_init,i2c1_pins_init,i2c1_init,i2c1_audio_init}()` | `dm330030_mikrobus_a_*()` |
| `g_uart1_pins_init_ok` `g_user_io_pins_init_ok` | `g_dm330030_*` — matching EV88G73A's |
| `board_pins.h`, `BOARD_LED1_PIN` … `BOARD_MIKROBUS_A_*` | `dm330030_pins.h`, `DM330030_*` |
| `BOARD_CLOCK_TARGET_FOSC_HZ` (a `-D` knob), `BOARD_CLOCK_FRC_HZ`, `BOARD_CONSOLE_BAUD_*` | `DM330030_*` |

Untouched on purpose: the BOARD SEAM's four functions and `DSPIC33CK_BOARD_*` build
guards (both genuinely per-board contracts), and `chip_drivers/wm8904_port.h`'s
`BOARD_USE_CMSIS_I2C` / `BOARD_CODEC_*` — those are that driver's own board-option seam,
which is the same convention seen from the other side.

The `-D` knob rename is user-visible: `-DDM330030_CLOCK_TARGET_FOSC_HZ=8000000` is now
the way to ask for FRC-direct. (The clock policy itself is in
`ck_hal_ports_from_ak.md`, "Clock".)

Behaviour-neutral by construction, and confirmed as such: DM330030 51,639 and EV88G73A
46,350 bytes, byte-for-byte the same totals as before the rename, in the default and
`ENA_WM8904_AUDIO=1` variants.

Still not done: Phase 4, splitting each board into the three roles above. Kept separate so
that file moves never share a commit with behaviour changes. Also still inconsistent and
deliberately left for it: `LED3_RGB_SetColor()` / `LED3_RGB_TickHandler()` keep their
vendor ALL-CAPS names, and `SYSTEM_Initialize()` / `CLOCK_Initialize()` keep the MCC ones
— those become `dm330030_board_init()` and friends when the files move, not before.

Cost: EV88G73A 46,290 -> 46,350 bytes (69%), DM330030 51,516 -> 51,639.
**Verified on EV88G73A**: boot banner byte-identical (Fosc/Fcy/Baud/`PLL at target`/
`PLLstatus=0`), the wiring line identical now that it comes from the macro, DMA self-test
PASS, `?sr` and `*sr` still work, idle cadence still 39.8 s. **Not observable**: SW0's
pull configuration (only the audio path reads SW0, and it needs a codec), and all of
DM330030 — no board.

### Phase 4, done: the three roles are files now

Both boards, same six-entry shape. No behaviour change in it — file moves and renames
only, which is why it is its own commit.

```
boards/ev88g73a/                      boards/dm330030/
  ev88g73a_pins.h       facts           dm330030_pins.h
  ev88g73a_board.{c,h}  bring-up        dm330030_board.{c,h}
  ev88g73a_io.{c,h}     runtime         dm330030_io.{c,h}
  config_bits.c         fuses           config_bits.c
  ev88g73a_console_out.c console seam   dm330030_console_out.c
  main.c                the profile     main.c
                                        dm330030_led3_rgb.{c,h}  <- has state
                                        demo_rgb_pot_buttons.{c,h}
```

| was | now | why |
|---|---|---|
| `board_ev88g73a.{c,h}` | `ev88g73a_board.{c,h}` + `ev88g73a_io.{c,h}` | one file was all three roles |
| `dm330030/board.c` + `system.c` | `dm330030_board.c` | one role was two files |
| `board.h` + `system.h` | `dm330030_board.h` | ditto |
| `led_sw.{c,h}` + `pot.{c,h}` | `dm330030_io.{c,h}` | both were plain accessors |
| `led3_rgb.{c,h}` | `dm330030_led3_rgb.{c,h}` | prefix only — see below |
| `SYSTEM_Initialize()` / `CLOCK_Initialize()` | `dm330030_board_init()` / static `dm330030_clock_init()` | MCC names for functions with nothing generated left in them |
| `LED3_RGB_SetColor()` … 20 vendor names | `dm330030_led3_rgb_set_color()` … | ditto |
| `led_sw_set(LED_SW_LED1, …)` | `dm330030_led_set(DM330030_LED_1, …)` | the Phase-3 prefix rule, applied to what Phase 3 missed |
| `EV88G73A_ENABLE_WM8904_AUDIO` / `_I2C1_PROBE` / `_SPI_EXERCISER` | same names, in `boards/ev88g73a/main.c` | they select which shared module the PROFILE runs; the board is identical either way |

**DM330030's `board.c`/`system.c` split was MCC's, not a reader's.** `system.c` held the
ORDER and `board.c` held the STEPS, so following the bring-up meant reading two files
alternately — and the one ordering dependency that had actually bitten this board
(`dm330030_ports_digital_default()` cleared ANSEL everywhere, so the pot's analog pin had to
be configured after it) spanned the boundary. Now the order is at the top of one file and
the steps are below it. That dependency no longer exists at all: the sweep was **deleted on
2026-08-03** — see the ANSEL-sweep section near the end — which is the stronger fix, but it
does not undo the reason the split was wrong.

**`dm330030_led3_rgb` did not merge into `io`, and the rule is the point.** It advances
three soft-PWM accumulators from the 1 ms tick, i.e. it owns a state machine, and a device
with state is not an accessor. So role three splits per device *when the device has
state*, and not otherwise — which is the line that keeps this from drifting back to five
files.

**Two switches moved to `main.c`, and DM330030's equivalents deliberately did not.**
`EV88G73A_ENABLE_*` are only ever read by that board's `main.c`, and nothing about the
board changes with them. DM330030's `ENA_WM8904_AUDIO` / `ENA_I2C_WM8904_PROBE` /
`ENA_TDM_MASTER_LOOPBACK` stay in `app/app_config.h` because they gate demos **shared**
between boards, where one header is the place to see them. Same rule, different answer.

**One latent defect fell out of the rename.** `led3_rgb.h` declared
`LED3_GREEND_SetIntensity` (note the D) while the `.c` defined `LED3_GREEN_SetIntensity` —
a declaration with no definition, which survived because nothing called it. Unifying the
names removed it.

Cost: EV88G73A **46,350 bytes, byte-for-byte unchanged**. DM330030 51,639 -> **51,636**,
i.e. 3 bytes *smaller*: `dm330030_clock_init()` became `static` with one caller, so `-Os`
inlined it and the symbol is gone from the map — the whole of the difference, and checked
rather than assumed. Build coverage: both defaults plus DM330030 `ENA_WM8904_AUDIO=1`
(51,882), `ENA_I2C_WM8904_PROBE=1` (51,735), `ENA_TDM_MASTER_LOOPBACK=1` (52,647), and
EV88G73A `_WM8904_AUDIO=0` (46,008) and `_WM8904_AUDIO=0,_I2C1_PROBE=1` (46,125) — the
last two also proving the moved switches still take `-D` overrides. The mutual-exclusion
`#error` still fires when both are 1.

**Verified on EV88G73A hardware**: boot banner byte-identical again (`Fosc 200000000` /
`Fcy 100000000` / `Baud 230400` / `PLL at target` / `PLLstatus=0` / `LOCK@sw=1`), wiring
line identical, DMA self-test PASS, `?sr` → `EXTR(MCLR) RCON=0x0083`, `?dt` → `backward=0`,
`*sr` → ack survived and the next boot reported `SWR(software reset)` (so the reset-cause
latch still latches and clears across the file move), idle reminder still 39.8 s.
**Not observable**: SW0 (needs a codec to be read), and all of DM330030 — no board.

### Review follow-up: two clock-dependent defects, and the rule that had an exception

Code review of the Phase 4 commit found two real defects and three design points. All five
are fixed. Both defects were **DM330030-only** and both came from a value that is only
correct at one operating point — the same shape twice, in code written months apart.

**1. FRC-direct came up at 230400 and said nothing.** The console rate followed
`g_dm330030_clock_on_target`, which is true whenever the *requested* point was reached.
Build `-DDM330030_CLOCK_TARGET_FOSC_HZ=8000000` — the FRC-direct option this tree documents
and offers — and the request succeeds, so the flag is true, so the console was configured
for 230400 on a 4 MHz Fcy: divisor 4, +12% error, every frame corrupt. **The documented
low-risk operating point was the one that produced a dead terminal.** "Reached what we
asked for" and "230400 is representable" are now separate questions, and the second is
asked of the achieved Fcy rather than of a list of known-good points:

```
BRG+1 = round(Fcy / (4 * baud))        the HAL's own formula (uart_calc_brg)
error <= 0.5 / (BRG+1)                 so <=2% needs BRG+1 >= 25
100 MHz -> 108  fine        4 MHz -> 4  refused, falls back to 9600
```

Published as `g_dm330030_console_fast_ok` beside `g_dm330030_clock_on_target`. Checking the
divisor rather than the frequency means a future third entry in the clock menu cannot
silently reintroduce this.

**2. The WM8904 MCLK divider was a literal 16.** `REFOCONH.RODIV = 16` gives 12.5 MHz from
Fosc 200 MHz and 500 kHz from Fosc 8 MHz — and `dm330030_mikrobus_a_mclk_init()` returned
`true` either way. On the FRC (requested, or after a failed PLL lock) the codec would get a
SYSCLK 25× too slow, not answer on I2C, and the board would report *"codec init failed"*: a
clock fault presenting as a wiring fault, which is the misdiagnosis `app/wm8904_audio.c`
orders `mclk_init` **first** specifically to avoid. The divider is now derived from the
achieved Fosc and required to be **exact** — 8 MHz has no integer divisor giving 12.5 MHz,
so the honest answer there is `false`, and the report becomes "MCLK init failed". Exact
rather than nearest on purpose: a codec MCLK that is merely close is a codec running at the
wrong sample rate, which is worse than one that refuses to start, because the audio plays.

**3. The bring-up stages are `static` now.** `dm330030_ports_digital_default()`,
`dm330030_uart1_pins_init()` and `dm330030_user_io_pins_init()` each had exactly one caller
and a load-bearing order between them (the first cleared ANSEL on every port, so the pot's
analog pin was only correct after it). Exporting them let any caller run them in an order
that silently yields a dead pot or a silent console. The order is enforced by linkage now
rather than by a comment; the results are still observable as the `g_dm330030_*_init_ok`
volatiles. The **sweep itself is gone as of 2026-08-03** (see the ANSEL-sweep section near the
end), so what remains ordered is only "clock before the pins that need it" — but keeping the
stages `static` is right for the same reason it was then.

**4. The pot moved back out of `dm330030_io` into `dm330030_pot.{c,h}`.** `io` claims
"runtime accessors, owns no init", and the pot owns an ADC handle, an init, a cached last
value and a fault string — so the header had to carry an "except for the pot" clause, and a
rule with an exception in it is a rule the next reader will not apply. The rule now holds
without one, and it subsumes the `led3_rgb` decision instead of sitting beside it:

| file | owns |
|---|---|
| `dm330030_io.*` | **nothing** — level reads and writes (LEDs, switches) |
| `dm330030_pot.*` | a **peripheral**: an ADC handle and its init |
| `dm330030_led3_rgb.*` | a **state machine**: soft-PWM advanced by the 1 ms tick |

One file per thing with state of its own; one shared file for the things with none.
EV88G73A's `ev88g73a_io` needs no sibling because it has only an LED and a button. The
three *roles* are unchanged — this is a division within role three.

**5. Live comments still naming `system.c` / `board.c` / `pot.c` are corrected** across
`app/app_config.h`, `app/board_seam.h` (merged into `app/app_traps.h` on 2026-08-06),
`app/wm8904_audio.h`, `chip_drivers/wm8904_port.h`,
`uart_platform/uart_platform_stdio.{c,h}`, `boards/dm330030/{config_bits.c, dm330030_pins.h,
dm330030_led3_rgb.c, main.c}`. Sentences that describe *history* ("was `board_ev88g73a.c`",
"split out of `system.c`") keep the old name, because that is what they are about.

Cost: DM330030 51,636 -> **51,744** (+108: the two derived-and-checked computations, and
`dm330030_pot.c` becoming its own translation unit). EV88G73A **46,350, still byte-for-byte
unchanged** — nothing in either defect or its fix is EV88G73A's. Build coverage now also
includes `-DDM330030_CLOCK_TARGET_FOSC_HZ=8000000` alone (51,660) and with
`ENA_WM8904_AUDIO=1` (51,906), i.e. the operating point that exposed both defects.
Re-verified on EV88G73A hardware: banner, DMA self-test and console identical again.

**Still not verifiable here**: both fixes are on DM330030, and there is no DM330030 board.
The evidence for them is the arithmetic above plus the `g_dm330030_console_fast_ok` /
`g_dm330030_console_baud` volatiles, which say on first flash which rate was chosen and
why.

## SPI-TDM across two boards: facts keep the connector, functions take the role

The two board files had drifted on a different axis from the ones Phases 1-4 fixed, and it
was not presence/absence — both boards route SPI1/TDM — but **what the name binds to**.

| | EV88G73A | DM330030 (was) |
|---|---|---|
| function | `ev88g73a_tdm_pins_init(role)` — **signal function** | `dm330030_mikrobus_a_spi1_tdm_client_pins_init()` — **connector + peripheral + role** |
| granularity | one function, a `role` argument | one function per (connector, peripheral, role) |
| who chooses the role | the profile; the board serves both | the board name asserts `client`, and `main.c` wrapped it to reject MASTER |
| `pins.h` | `EV88G73A_TDM_RP_{BCLK,FS,SDO,SDI}` | `DM330030_MIKROBUS_A_{BCLK,FS,SDO,SDI}_RP` |

**Converged on EV88G73A's axis for the functions, DM330030's for the facts.** The rule:

> **Facts keep the connector; functions take the role.**
> `dm330030_board.c` exports `dm330030_tdm_pins_init(role)` — identical in shape to
> EV88G73A's, and to the single `configure_pins(role)` hook the transport HAL offers.

**Amended 2026-08-03 — the macros went too (see Sec. "Pin macros name the role").** The
rule above stopped at the function names and left `DM330030_MIKROBUS_A_BCLK_RP` in place,
reasoning that the connector is a board fact. It is — but a *fact* is not the same as an
*identifier*: the connector belongs in the comment and the wiring string a human reads,
not in the token the code passes around. The macros are now
`DM330030_TDM_RP_{BCLK,FS,SDO,SDI}` / `DM330030_AUDIO_RP_MCLK` / `DM330030_I2C1_RP_{ASCL,ASDA}`.

Three reasons, in the order they actually decided it:

1. **The HAL seam's shape already dictated it.** `dspic33ck_spi_i2s_tdm_port_t` has *one*
   `configure_pins(role)` callback. A board exporting one function per role forces
   profile-side glue to adapt — which is exactly where the capability statement got split.
2. **The role is not a board fact.** Both boards' four pins physically carry either
   direction. DM330030's `_client_` asserted a decision the wiring does not make. Its one
   genuine constraint — REFO1 already supplies the codec's SYSCLK, so dsPIC-as-BCLK-master
   was never the intended arrangement — is a *profile* fact, and now sits beside
   `.dspic_is_master = false` in `main.c` as a stated choice rather than a refusal.
3. **Connector-named functions multiply.** A second codec on mikroBUS-B turns five
   functions into ten; a signal-named one takes an argument.

### What that bought: `app/` no longer contains a board pinout

`app/demo_tdm_master_loopback.c` held `DEMO_RP_BCLK 72` / `FS 66` / `SDO 70` / `SDI 71` —
DM330030's mikroBUS-A — plus its own `configure_pins` duplicating what that board's file
already did in the other direction. It had to: the board's function was client-only, so the
demo could not call it. So a module shared between boards owned one board's pinout and
could run nowhere else, while the board that actually has hardware (EV88G73A) kept a
private copy of the same exerciser, since deleted.

The demo now takes a `demo_tdm_master_loopback_port_t` (`configure_pins` + a `wiring`
string it prints at start), exactly as `app/wm8904_audio.h` takes `wm8904_audio_port_t`,
and `boards/dm330030/main.c` fills it with `dm330030_tdm_pins_init`. The MASTER branch in
the board file **is** that deleted duplicate, moved rather than rewritten.

Mechanical check, stronger than grepping for numbers: **no file in `app/` includes
`nora_gpio.h` or `nora_pps.h` any more**, so nothing there can route a pin even
by accident. The only RP numbers left under `app/` are inside sentences describing this
history and one example string in `i2c_probe.h`.

The role check moved too, and in the opposite direction: "this demo only drives MASTER" is
now enforced *in the demo*, because it is the demo's constraint. Asking a board to refuse a
role on a demo's behalf was the inversion that started all of this.

### Three things here are NOT divergent, and were left alone

- **`clc_passthrough` is NULL on both boards** — for two different reasons, neither of
  which was written down and both of which now are. It is the CLC *bypass* route that fans
  a slave's incoming clock out to a second device; no profile here has one. It is **not**
  where the 50%-duty FS comes from.
- **CLC1 belongs to the HAL, and finds its own pin.**
  `hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm_fs_clc.c` reverse-scans the RPOR table for
  whichever pin the board routed FRMSYNC (SSx) to, routes FRMSYNC to virtual pin RPV0,
  configures CLC1 as a J-K flip-flop and repoints that same pad to CLC1OUT. So a board file
  containing no CLC code is **correct, not incomplete** — which is why EV88G73A's never
  had any, and why DM330030's still does not.
- **No board configures an SPI register.** Format, BRG, slot count, word width and DMA are
  the app's config and the HAL's to apply. Both boards only route pins.

Also deliberately not done: MCLK and I2C1 bring-up stay in the board file (they are genuine
board facts), and `wm8904_audio_port_t` is not merged into
`dspic33ck_spi_i2s_tdm_port_t` — different layers, and the audio module forwards one to the
other on purpose.

Cost: DM330030 51,744 -> **51,861** baseline (+117 — the MASTER branch is linked even where
nothing calls it, since `remove-unused-sections` is off), and 52,755 -> **53,229** with
`ENA_TDM_MASTER_LOOPBACK=1` (the duplicate routing code is gone; the increase is the
board's `wiring` string and the line that prints it). EV88G73A **46,350, byte-for-byte
unchanged**. All five variants build warning-free.

**Not verified on hardware**: DM330030's new MASTER branch, because there is no DM330030
board. Its evidence is that it is the loopback demo's own previously-working routing for
the same four pins, moved. The demo is board-agnostic now, so giving EV88G73A a port struct
would make this hardware-verifiable on the board that has hardware — deliberately left as a
separate step, since EV88G73A is at 69% flash.

Those older per-milestone docs — which still named `board_ev88g73a.c`, `system.c`,
`led_sw.h` and other pre-reorganisation files — were consolidated away on 2026-08-03.
Superseded file and symbol names went with them; what survived is in
`ck_silicon_findings.md` (hardware findings) and `ck_hal_ports_from_ak.md` (AK→CK register
deltas). **This file describes the tree as it is now.** The originals are in `git log` if a
dated report of what was true at the time is ever wanted.

## What `src/bsp/` was, and where it went

`bsp/` held eleven `Copyright 2016 Microchip` vendor files. Its name claimed
nothing, but every one of its APIs had exactly one consumer — DM330030's
`main.c` — and `build.ps1` listed them only in the DM330030 source set. The
ownership lived in a build script and in nobody's head.

| was | now |
|---|---|
| `led1.h` `led2.h` `button_s1..s3.h` | merged into `boards/dm330030/led_sw.{c,h}`, GPIO HAL |
| `led3_rgb.{c,h}` | `boards/dm330030/`, pin writes on the GPIO HAL |
| `adc.{c,h}` | `boards/dm330030/`, **unchanged — see below** |
| `timer_1ms.{c,h}` | `app/` |
| `hardware.txt` | deleted: it documented a PIC24F256GA7 board, neither of ours |

### The defect this removed

`board_pins.h` defined `BOARD_LED1_PIN = port E pin 6` and `board.c` configured
it through the GPIO HAL, while `led1.h` drove `LATEbits.LATE6` / `TRISEbits.TRISE6`
directly. Two owners of one pin. Same for LED2 and SW1/SW2/SW3, and for the RGB
LED's three pins — `board_pins.h` already held every one of them, so the vendor
headers duplicated pin facts that were already correct elsewhere.

Concretely: `LED1_On()` rewrote `TRIS` on **every call**, as did
`BUTTON_S1_IsPressed()` and `led3_rgb.c`'s `PinControl()` — and that last one runs
off the 1 ms tick, so it was re-asserting direction thousands of times a second,
behind the HAL's back.

`led_sw.c` has no `init()` on purpose: `board.c`'s `board_user_io_pins_init()`
configures these pins and stays their only configurer. Nothing in `led_sw.c` or
`led3_rgb.c` touches a direction register any more. (The AK fleet's
`board_components/led_sw.c` does own its init — there is no separate `board.c`
doing it there.)

Cost: DM330030 grew 39,387 → 39,555 bytes, +168, buying one owner per pin. At 14%
of a 256 KB part that is not a trade worth arguing about.

## Where the build is defined

`firmware.X/nbproject/configurations.xml`, not a script. See
[`buildtools/README.md`](../buildtools/README.md). Every board-owned `.c` `#error`s
if compiled into the wrong configuration, so an exclusion mistake in the xml stops
the build instead of quietly compiling one board's register writes into the other
board's image.

## ADC HAL status

CK ADC register access now lives in `hal_adc/` (`nora_adc.h`, `nora_adc_dspic33ck.c`,
`nora_adc_dspic33ck_reg.h`). It owns
the shared-core ADC registers (`ADCON*`, `ADSTAT*`, and `ADCBUF0 + ANx`), while
`boards/dm330030/pot.c` is only a board adapter: it identifies the POT's RE3/AN23
connection and nothing else.

### The pot adapter, tidied (2026-08-02)

Asked whether `boards/dm330030/adc.c` could be put onto this HAL, the answer was that
it already was — 43 lines of adapter, and `grep` finds no `ANSEL`/`TRIS`/`gpio` in the
HAL at all, so the adapter's pin call was load-bearing rather than redundant. What the
file did have was four small versions of problems this tree has been finding all along:

**The pot pin had a different owner from every other pin on the board.** `adc.c`
configured RE3 as analog while `board.c` configured the LEDs and switches — the
two-owners shape that `led_sw.h` exists to document. Worse, the dependency between them
was invisible: `board_ports_digital_default()` cleared `ANSEL` across every port, so
making RE3 analog only worked *after* it, and the two calls sat in different files that
happened to be sequenced correctly. Both are now in `board_user_io_pins_init()`, and
`pot.c` owns no pin. The "in order" half of that sentence expired on 2026-08-03 when the
sweep was deleted (section near the end): RE3 is now the only analog-capable pin this board
configures, it is the one pin that *wants* `ANSEL` set, and nothing else touches it.

**A 16-bit convention that went up and came straight back down.** The adapter returned
`raw << 4` for the RGB LED's PWM, and the display shifted every value back with `>> 4`
— i.e. the board's ADC adapter carried the LED driver's bit depth. `dm330030_pot_read()`
now returns the raw 12 bits, and `demo_rgb_pot_buttons.c` — the one file that knows the
PWM is 16-bit — converts at the single `LED3_RGB_SetColor()` call (`PWM_OF()`). The
channel state and the screen now share one domain, so three `>> 4` disappeared.

**A failure nobody could see.** `main.c` discarded the init result with a bare `(void)`,
and a failed conversion silently repeated the previous value: an ADC that never came up
was indistinguishable from a pot nobody had turned. Same class as
`board_user_io_pins_init()` having no caller. `dm330030_pot_fault()` now returns the
HAL's reason as text and the demo's pot line prints `FAULT (TIMEOUT)` in place of the
number. It is reported *there*, not at bring-up, because `PrintHeader()` clears the
screen immediately after bring-up — a message printed then would be erased.

The reason string comes from a new `nora_adc_result_str()` beside the enum in
`hal_adc/`, for the same reason `nora_i2c_status_str()` was added there: every
caller reporting a result wants the same words, and a `switch` next to the enum stops
compiling silently when a result is added.

**One constant doing two jobs** — the same `1000000` served as `ready_timeout_count` at
init and as the per-conversion timeout. Split into `DM330030_POT_READY_TIMEOUT_COUNT`
and `DM330030_POT_READ_TIMEOUT_COUNT`; values unchanged.

Renamed `adc.{c,h}` -> `pot.{c,h}`, symbols `dm330030_adc_*` -> `dm330030_pot_*`: the
module is one device, like `led_sw.{c,h}` and `led3_rgb.{c,h}` beside it, and the old
name read like a second ADC HAL.

Cost: DM330030 51,282 -> 51,516 bytes (the fault plumbing and the result strings).
EV88G73A unchanged at 46,290 — `hal_adc` is `ex="true"` there, so none of this reaches
the 64 KB part. **Build-verified only**: this board is not available, and the pin-owner
move and the fault reporting are both changes that only show themselves on hardware.

The public `nora_adc_*` API deliberately follows Sonora's dsPIC33AK ADC HAL
interface. The peripherals are still different internally: CK exposes one shared
ADC core and an `ANx` channel, whereas the AK implementation has per-instance
modules. CK therefore exposes only `NORA_ADC_INSTANCE_1` and requires
`positive_input` to match `channel`; this keeps the API compatible in shape without
pretending the hardware models are identical.

This first phase deliberately permits only external shared-core inputs. The
device-specific checks are AN2--AN23 on CK256MP508 and AN0--AN15 on CK64MC105;
dedicated-core and internal temperature/band-gap inputs need an explicit future
extension. `sample_time_tad` remains the common API field, but on CK it means the
actual shared-core sample duration in TADCORE cycles (2--255); the HAL converts it
to the hardware's `SHRSAMC = sample_time_tad - 2` encoding.

The initial scope is polling plus a software-triggered conversion. DMA, ADC
interrupt ownership, PPS configuration, and audio-input policy remain outside this
HAL. CK has no equivalent of the AK implementation's explicit `CALREQ`/`CALRDY`
calibration sequence, so requesting calibration returns
`DSPIC33CK_ADC_RESULT_UNSUPPORTED`; the DM330030 POT adapter does not request it.
`get_result()` accepts only an in-flight, ready conversion. On a conversion or
startup timeout the HAL disables ADC1 and marks its handle uninitialized, so the
caller must initialize it again before reuse. `clear_error()` only clears a
diagnostic result on an initialized handle; it does not recover a timeout fault.

Only the CK256MP508 DM330030 configuration builds the POT adapter; the CK64MC105
EV88G73A configuration has no board ADC consumer. Compile-and-link coverage exists for
the HAL itself, but hardware verification of the DM330030 knob-to-RGB-LED behaviour is
still pending.

**The EV88G73A configuration therefore excludes `hal_adc/nora_adc_dspic33ck.c`**
(`ex="true"`), and that exclusion is measured rather than tidiness. Both
configurations compiled it at first, and because `remove-unused-sections` is off, all
nine `dspic33ck_adc_*` functions landed in the EV88G73A image with nothing calling
them: 45,462 -> 46,314 bytes, +852, 68% -> 69% of a 64 KB part. Verified after
excluding it: zero `dspic33ck_adc` symbols in the map and the size back to 45,462.

Note the asymmetry with the other `hal_*` directories, which both configurations do
compile: those have consumers on both boards. If EV88G73A ever gains an analog input,
flip that one `ex` back — the HAL is device-correct for CK64MC105 already (its channel
check is AN0--AN15 on this part).


One codec, one set of names (2026-08-02)
----------------------------------------

The same WM8904 board attaches to both boards in this repo, and on each it fills the same
three `wm8904_audio_port_t` slots. The names filling those slots did not match, which was
the last visible seam:

| slot | EV88G73A | DM330030, before | DM330030, now |
| --- | --- | --- | --- |
| `configure_pins` | `ev88g73a_tdm_pins_init(role)` | `dm330030_tdm_pins_init(role)` | unchanged |
| `i2c_init` (audio) | `ev88g73a_i2c1_init()` | `dm330030_mikrobus_a_i2c1_audio_init()` | `dm330030_i2c1_init()` |
| `bus_init` (probe) | the same function | `dm330030_mikrobus_a_i2c1_init()`, 100 kHz | the same function |
| `mclk_init` | NULL (codec has X1) | `dm330030_mikrobus_a_mclk_init()` | `dm330030_mclk_init()` **[row deleted 2026-08-04]** |

This is the T4 rule applied to the rest of the audio path: **facts keep the connector,
functions take the role.** `mikroBUS-A` is still in the pin macros' comments (they were
`DM330030_MIKROBUS_A_*` until 2026-08-03 — see the section below), in
`config_bits.c`'s ALTI2C1 note, and in the wiring text the boot report prints
-- because which connector the codec plugs into really is a fact about this board. It is
not a property of the operation, and having it in the function name made a shared
operation look board-specific. That was not cosmetic: it is why "EV88G73A appears to have
no SPI-TDM init" was a reasonable thing to conclude from reading the two files
side by side.

Three other things fell out of the same pass:

- **One I2C rate, 400 kHz.** See the superseded section above. Both callers now reach the
  codec identically; the parameterised `i2c1_init_at()` stays static so the rate remains
  the board's to choose. (**Superseded 2026-08-04**: that layer is deleted — the rate is
  the `DM330030_I2C1_BUS_HZ` define and each board has exactly one I2C1 function. See
  "Three functions one board had and the other did not".)
- **`i2c1_pins_init()` is static.** It was public with one caller, inside this file. Same
  reason the three bring-up stages became static in the review follow-up: an exported
  pins-only stage lets a caller clear ANSEL and never initialise the module -- a bus that
  looks configured and answers nothing. EV88G73A never exported an equivalent.
- **The wiring text moved to `dm330030_pins.h`** as `DM330030_AUDIO_WIRING_STR`,
  `DM330030_TDM_LOOPBACK_WIRING_STR` and `DM330030_I2C1_WHERE_STR`. It was hand-typed in
  `main.c`, twice, spelling out RP numbers that the macros above it already hold --
  exactly the duplication `EV88G73A_AUDIO_WIRING_STR` exists to prevent on the other
  board.

What deliberately did NOT converge: `mclk_init` exists only on DM330030. That is the one
real asymmetry between the two attachments -- EV's codec self-clocks from its own X1
crystal, DM's takes REFO1 from the dsPIC -- and flattening it would mean inventing a
no-op hook to make the two files look alike. **[FALSE, corrected 2026-08-04: same WM8904 board and same XTAL on both -- the stage and the hook are deleted. See the MCLK section near the end.]**

Measured. EV88G73A 46,350 bytes, byte-for-byte unchanged (nothing on that board was
touched). DM330030 baseline 51,861 -> 51,834 (-27, the deleted second I2C wrapper),
`ENA_I2C_WM8904_PROBE=1` 51,933, `ENA_WM8904_AUDIO=1` 52,125, `ENA_TDM_MASTER_LOOPBACK=1`
53,229 -> 53,202. All five warning-free.

Not verifiable here: every line above is DM330030-only and there is no DM330030 board.
The 100 kHz -> 400 kHz change is the only behavioural difference in the set; its evidence
is the fleet measurement cited above plus EV88G73A running this same codec at 400 kHz on
hardware. The rest is renaming and linkage.

## Step 1 of "same calls, different parameter context": TDM pins (2026-08-02)

The two `board.c` files are being converged on a stated principle: **the same
initialisation functions get called on both boards, and what differs is the
parameter context handed to them.** Not as one rewrite -- one stage at a time,
each measured, with "EV88G73A keeps working" as the acceptance condition. The
precedent is sonora's AK512/AK128 pair, where the same discipline meant the second
board came up in about half a day.

Stage 1 is the clearest case in the two files.

### What was measured first

`ev88g73a_tdm_pins_init()` and `dm330030_tdm_pins_init()` were forty lines each and
differed in **four RP numbers**. Same two role branches, same four directions, same
four PPS tokens, same "seed outputs low before PPS drives the pad" rule. One of
them was even a copy of the other, moved from `app/demo_tdm_master_loopback.c`.

### What moved, and where

The forty lines are now `hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm_pins.c`:

    bool dspic33ck_spi_i2s_tdm_pins_configure(const dspic33ck_spi_i2s_tdm_pinmap_t *map,
                                              dspic33ck_spi_i2s_tdm_role_t          role);

They went to the **HAL**, not to `app/`, and the reason is the axis: "SPI1's frame
clock is the SS1 token, and it is an output when we are the master" is a fact about
the device family, while an RP number is a fact about a board. This is the same
placement `dspic33ck_spi_i2s_tdm_fs_clc.c` already had -- it owns CLC1 so that no
board has to know CLC exists.

Each board keeps a four-field `static const` map and a one-line function. The board
still owns the CALL and the numbers; nothing in the shared file reads a board header.

### The order is EV88G73A's, deliberately

The shared file does all four directions first and all four PPS routes second.
EV88G73A did that; DM330030's SLAVE branch interleaved them pin by pin. The
two-phase order is the one a scope has actually seen on the loopback demo, so the
convergence goes toward it -- i.e. the behaviour change lands on the board with no
hardware, and not on the board that is verified.

### Measured

| build | before | after |
| --- | --- | --- |
| CK64MC105_EV88G73A `ENA_WM8904_AUDIO=1` | 46,350 | **46,350 (unchanged)** |
| CK256MP508_DM330030 `ENA_WM8904_AUDIO=1` | 52,125 | 52,101 (-24) |
| CK256MP508_DM330030 `ENA_TDM_MASTER_LOOPBACK=1` | 53,202 | 53,178 (-24) |

No warnings, no errors. EV88G73A byte-for-byte identical is the result that matters:
this stage cost the flash-constrained board nothing and needs no hardware re-check.

### Why this order of stages

The remaining delta between the two files is not all of one kind, so it is not all
worth the same treatment:

- **values** -- RP numbers, FRC/target Fosc, the two baud rates, the I2C rate, the two
  RAM-derived trap addresses. These are what the parameter-context idea is for.
- **capability asymmetries** -- DM330030 generates the codec's MCLK on REFO1 and
  EV88G73A has no such thing (its codec has a crystal — **wrong, see the 2026-08-04 MCLK
  section: both codecs are the same board and both have the crystal**); EV88G73A routes UART RX and
  DM330030 is TX-only; EV88G73A latches and decodes RCON and DM330030 deliberately does
  not. These stay explicit optional hooks. Forcing them into a table would link both
  boards' features into the 64 KB part, and `remove-unused-sections` is **off**.
- **drift** -- the console-rate predicate, the I2C pull-ups and `timeout_ms`, RP-form
  versus port/bit-form `set_analog`. Worth converging on their own merits, but each
  touches EV88G73A's behaviour, so each needs a hardware re-check.
- **structure** -- EV88G73A builds a `dspic33ck_uart_config_t` inline while DM330030
  calls `uart_platform_stdio_init()`. That is a different path, not a different
  parameter, and is **out of scope** for this work.

  > **Reversed 2026-08-04.** The two configs turned out to be identical field for field,
  > so this was one path stated twice, not two paths. Both boards now call
  > `uart_platform_stdio_init()`; what really differs is only how they WRITE (printf vs
  > `console_out.h`). See "Three functions one board had and the other did not".

Next stage: the pin tables (`user_io`, `uart1` pins) as `{pin, config}[]`, whose size
delta decides how far the rest goes.

## Step 2: the pin lists as tables -- and the measurement that argued against it (2026-08-02)

`hal_gpio/dspic33ck_gpio_table.{c,h}`: a board's fixed pins as
`{pin, &config}[]`, applied by one shared walk, plus the four descriptions both
boards were writing out for themselves (`output_low`, `output_high`,
`input_pullup`, `analog_input`).

DM330030's nine `if (!...) return false;` lines and its two local config structs
become one nine-entry table; EV88G73A's two become a two-entry table. `ORDER IS PART
OF THE DATA` was stated in the header and in DM330030's table, because the pot entry had to
stay last: `dm330030_ports_digital_default()` cleared ANSEL across every port, so making RE3
analog again had to come after it.

> **That invariant is gone as of 2026-08-03**, and the header now says the opposite —
> `ENTRIES ARE INDEPENDENT`. The sweep was the *only* reason a table of pin descriptions was
> order-sensitive, so deleting it (section near the end) removed the ordering rather than
> merely documenting it. A future need to order these entries would mean a pin description is
> incomplete.

Equivalence was checked before measuring, not assumed:
`dspic33ck_gpio_config_digital_output(pin, false)` builds exactly
`dspic33ck_gpio_cfg_output_low` and calls the same `dspic33ck_gpio_config()`, so the
register writes and their order are unchanged.

### Measured -- BOTH BOARDS GREW

| build | step 1 | step 2 | delta |
| --- | --- | --- | --- |
| CK64MC105_EV88G73A `ENA_WM8904_AUDIO=1` | 46,350 | 46,446 | **+96** |
| CK256MP508_DM330030 `ENA_WM8904_AUDIO=1` | 52,101 | 52,125 | +24 |
| CK256MP508_DM330030 `ENA_TDM_MASTER_LOOPBACK=1` | 53,178 | 53,202 | +24 |

The prediction in step 1's note -- that the table would SHRINK both boards -- was
wrong, and the reason is worth recording because it bounds where this technique
applies. A table pays for itself only against a long list. EV88G73A's list is two
entries, and a table plus a loop plus four `extern const` descriptions cannot beat
two inlined `config_digital_output()` calls. The four descriptions are a fixed cost
paid by both boards whether or not they use all four, because the link is done with
`--no-gc-sections`. That fixed cost is also why DM330030's nine-entry table, which
should have won on length, only broke even and then some.

### Why it was adopted anyway

The stated stopping rule for this work is "measure EV88G73A each step, stop if it
grows", and this step tripped it. It was adopted by explicit decision after the
number was on the table: 96 bytes leaves EV88G73A at 69%, and the return is that
adding a pin to either board becomes one line in a table rather than one more
branch in a function -- which is the property this whole line of work exists to buy
for the day DM330030 hardware arrives.

**This is the first change in this series that alters EV88G73A's firmware**, so
unlike step 1 it wants a hardware re-check (LED0 lit at boot, SW0 read by `?sw`).
The change is intended to be behaviour-identical; the check is because "intended"
and "verified" are different words.

### What was deliberately NOT tabulated

`uart1_pins_init()`. It is two pins reached through PPS, not GPIO config, and a
descriptor for it needs a per-entry direction flag plus both an output token and an
input token, one of which is dead in every entry. Step 1 got its win because all
four TDM pins shared one shape; these two do not, and the SPI case has already
absorbed the routing repetition that mattered.

## Step 3: the boot clock stage as {source, input, target} (2026-08-02)

`ev88g73a_clock_init()` and `dm330030_clock_init()` are now the same three lines
each: a `static const dspic33ck_clock_bringup_t` naming two frequencies, one call
to `dspic33ck_clock_bringup()`, and republication of the result under the board's
own `g_<board>_clock_init_status` / `osccon_after_switch` / `clock_on_target`.
The stage itself is `src/hal_clock/dspic33ck_clock_bringup.{h,c}`.

What moved is not the two HAL mechanisms -- `dspic33ck_clock.h` already had those
-- but the POLICY both boards had written around them, identically, by hand:

  1. snapshot `OSCCON` before anything else, so the evidence is what the request
     produced and not what the fallback left behind;
  2. judge "on target" from THREE conditions -- HAL status OK **and**
     `OSCCONbits.LOCK` **and** achieved Fosc == requested. A status of OK alone
     only says the sequence ran, which is exactly how a half-configured PLL
     passes for success;
  3. never stay on a PLL that did not lock -- fall back to the input source.

Rule 3 was learned on EV88G73A (a silent half-speed clock presenting as a garbled
console) and then copied into `dm330030_board.c` by hand. Copied policy is policy
that can drift, which is the whole reason this step was worth doing.

DM330030's compile-time `#if (DM330030_CLOCK_TARGET_FOSC_HZ == DM330030_CLOCK_FRC_HZ)`
no-PLL arm is gone, replaced by the engine's run-time `target_fosc_hz == input_hz`
comparison. EV88G73A gains that option for the first time.

### The measurement, and the decision

| build | step 2 (`a8dc945`) | step 3 | delta |
| --- | --- | --- | --- |
| EV88G73A `ENA_WM8904_AUDIO=1` | 46,428 (69%) | 46,551 (70%) | **+123** |
| DM330030 `ENA_WM8904_AUDIO=1` | 52,125 | 52,245 | +120 |
| DM330030 `ENA_TDM_MASTER_LOOPBACK=1` | 53,202 | 53,322 | +120 |
| DM330030 `-DDM330030_CLOCK_TARGET_FOSC_HZ=8000000` | -- | 52,245 | same as default |

No warnings, no errors, on any of the four. That last row is itself a result: the
FRC-direct variant is now the SAME SIZE as the default, because the `#if` is gone
and the operating point is purely data.

Every build grew, and unlike step 2 **no board got smaller** -- so this step was
adopted on its de-duplication value alone, by explicit decision with the number in
hand, not because the size argued for it. The reason it grows is
`--no-gc-sections`: the shared engine always links both the PLL arm and the no-PLL
arm, where each board previously compiled only its own, plus the cost of passing
the request and result structs. Step 2's `+96` on EV was at least paid back as
`-24` on DM; this one is not paid back anywhere.

Correction to the step-2 section above: EV88G73A's step-2 size was re-measured at
`a8dc945` as **46,428**, not the 46,446 recorded there. The `+96` conclusion of
that step is unaffected; the baseline for step 3 is the re-measured number.

Running total for the series on EV88G73A: 46,350 (step 1, unchanged) -> 46,428
(step 2) -> 46,551 (step 3), i.e. **+201 bytes for three stages of board.c reduced
to parameter context**, with about 18.9 KB of the 64 KB part still free.

### Still divergent after step 3

Unchanged and deliberately so: the console STRUCTURE
(`uart_platform_stdio_init()` on DM vs an inline `dspic33ck_uart_config_t` on EV)
is out of scope for this series. (**Converged 2026-08-04** — see the section of that date.) Next up is the (b) flag group -- the RCON latch
policy, the ANSEL sweep port mask, and the I2C internal pull-up. (**The ANSEL entry was
settled by deletion, not by a mask**, on 2026-08-03 -- section near the end.)

## Pin stages get concrete names on both boards (2026-08-03)

Not a convergence step -- a naming/shape alignment, asked for because progress on these
two files is judged by eye.

Two boards, one shape now: every pin stage is its own function named
`<board>_<peripheral>_pins_init()`, static, called by the stage that owns the peripheral.

  - EV88G73A: `ev88g73a_uart1_pins_init()` split out of `ev88g73a_uart1_init()` (the
    4 GPIO/PPS calls; the peripheral half stays where it was — and that split is what made
    2026-08-04's step possible, the peripheral half then being deletable outright), and
    `ev88g73a_i2c1_pins_init()` split out of `ev88g73a_i2c1_init()` (ANSEL + pulls).
  - DM330030: `i2c1_pins_init` -> `dm330030_i2c1_pins_init`, `i2c1_init_at` ->
    `dm330030_i2c1_init_at` (deleted 2026-08-04). They were the only two functions in
    either board file without the board prefix.

What was NOT done: `ev88g73a_user_io_pins_init()` was NOT split into led0/sw0 stages.
DM330030 has the identically-named nine-entry `dm330030_user_io_pins_init()`, so
splitting EV would have moved the two files apart, not together.

Section ORDER needed no change: both files already run
tdm_pins_init -> (DM only: mclk_init, deleted 2026-08-04) -> i2c1 -> BOARD SEAM.

Measured, same `-BuildId measure` on both sides of the change (this matters -- the build
ID string is linked into flash, so the absolute numbers recorded in the steps above are
only comparable against a build with an identically-long ID):

  | build              | before | after  | delta |
  |--------------------|--------|--------|-------|
  | CK64MC105_EV88G73A | 46,419 | 46,422 | +3    |
  | CK256MP508_DM330030| 51,954 | 51,954 | 0     |

+3 bytes on EV, from the UART split (the I2C split cost nothing). Reported rather than
absorbed, per the standing rule that any EV growth is the user's call.

## The clock verdict has one shape on both boards (2026-08-03)

`clock_init()` itself was already identical on the two boards after the shared
`dspic33ck_clock_bringup()` stage. What still differed was everything AROUND it -- the
state it wrote and how anyone read it back:

| | EV88G73A (before) | DM330030 |
|---|---|---|
| HAL status | `volatile g_ev88g73a_clock_init_status` | same shape |
| OSCCON snapshot | `static uint16_t osccon_after_switch` | `volatile g_dm330030_osccon_after_switch` |
| on-target verdict | `static bool clock_on_target` | `volatile g_dm330030_clock_on_target` |
| accessors | **five exported functions** | **none** |

Three changes brought them together:

- The two bare file statics became `g_ev88g73a_osccon_after_switch` /
  `g_ev88g73a_clock_on_target`, `volatile`, matching DM330030's names bit for bit apart
  from the board prefix. Being volatile is the point on a board whose console may never
  come up: these are debugger evidence, and a board-prefixed name is what you can find
  in a symbol table.
- `ev88g73a_fosc_hz()` / `ev88g73a_fcy_hz()` are GONE. They were one-line pass-throughs
  to `dspic33ck_clock_get_fosc_hz()` / `_get_fcy_hz()`, which every other consumer in the
  tree already called directly (`uart_platform_stdio.c`, `app/wm8904_audio.c`,
  `app/demo_tdm_master_loopback.c`, and both of DM330030's own rate-derived stages). A
  board cannot make the HAL's record of Fosc more authoritative by forwarding it.
- `ev88g73a_clock_at_target()` / `_clock_status_code()` / `_osccon_after_switch()` are
  gone too; `main.c`'s banner reads the volatiles directly.

One honest cost: DM330030's clock volatiles have NO reader in code at all -- they are
pure debugger evidence, because that board has no boot banner. EV88G73A's are read by
`main.c`, so the three had to be declared `extern` in `ev88g73a_board.h`. That is an
export DM330030 does not have. It is the smaller asymmetry of the two available (the
alternative was keeping five wrapper functions), but it is not zero.

`uart_baud_applied` and `reset_cause_raw` deliberately KEEP their bare static names.
DM330030 has no counterpart to either -- its console rate lives in
`g_dm330030_console_baud` and is decided by a different rule
(`DM330030_CONSOLE_FAST_MIN_DIV`), and it reads RCON live rather than latching it.
Renaming them to `g_ev88g73a_*` would invent an agreement that does not exist.
(**2026-08-04**: `uart_baud_applied` is not a file static any more -- it is a local in
`ev88g73a_board_init()`, since the stage that used to read it is now a call. Only
`reset_cause_raw` remains.)

### mclk_init: documented, not converged

> **THE PREMISE OF THIS SUBSECTION IS FALSE (established 2026-08-04).** Both boards use the
> SAME WM8904 board and it carries its own XTAL, so "that board's codec self-clocks" is not
> an EV88G73A property — it is true of both. The conclusion below ("the one genuine
> asymmetry") does not hold, and the resolution is to delete the DM330030 stage rather than
> to converge or extract it. See "`dm330030_mclk_init()` rests on a false premise" near the
> end of this document. Left here unedited because the reasoning it records — a hook that is
> NULL costs nothing, an empty stage returning true would claim a capability — is sound in
> itself and was applied elsewhere.

`dm330030_mclk_init()` (REFO1 at 12.5 MHz to the codec's MCLK pin) has no EV88G73A
counterpart and must not get one -- that board's WM8904 self-clocks from its own X1.
This is the one genuine asymmetry in the audio path, and it already costs nothing:
`wm8904_audio_port_t.mclk_init` is a function pointer, EV88G73A leaves it `NULL`, and
`wm8904_audio.c` NULL-checks before calling. An empty `ev88g73a_mclk_init()` returning
true would make the two files look more alike and would claim a capability the board
does not have. `ev88g73a_board.h` now carries a note saying exactly that, mirroring the
`(none -- codec has X1)` cell in dm330030_board.h's seam table, so a reader comparing
the files finds the reason on the side where the function is missing.

### Measured

Same `-BuildId measure` on both sides, so the Build-ID string length is constant:

| Configuration | before | after | delta |
|---|---|---|---|
| CK64MC105_EV88G73A | 46,422 | 46,383 | **-39** |
| CK256MP508_DM330030 | 51,954 | 51,954 | 0 |

EV88G73A shrank: five functions removed, and under `--no-gc-sections` every one of them
cost flash whether called or not. Both configurations build with no diagnostics.

## RCON decoded once, latch policy left to the board (2026-08-03)

Step 4's (b) flag group had three items. Only the RCON one was worth doing; see the
bottom of this section for why the other two were left.

`RCON`'s bits and their priority are a FAMILY fact -- a POR also sets BOR on this family,
a trap conflict is more specific than the watchdog that may also be set -- so deciding
which one to name was never board knowledge. EV88G73A had a seven-way ladder;
DM330030 had no copy and its `board_reset_cause_str()` returned a flat refusal:

    "(DM330030 does not latch RCON; read the raw word)"

That mattered because `app/app_traps.c:247` and `uart_app/system_console.c:38,65` are
SHARED code calling the same BOARD SEAM entry point. One board got a real answer;
the other got an apology.

New `src/hal_reset/dspic33ck_reset.{c,h}`:

    const char *dspic33ck_reset_cause_str(uint16_t latched, bool is_latched);
    void        dspic33ck_reset_cause_clear(void);

### The latch stays with the board, and that is the whole design

The two boards genuinely differ and both reasons are good:

- **EV88G73A latches** at the very top of `board_init()` and clears the bits after. It
  has to: with no reset button on a Curiosity Nano, telling a POR ("you power cycled it")
  from an SWR ("the `*sr` command worked") is the only evidence a software reset happened.
- **DM330030 does not latch**, deliberately. Nothing there captures or clears RCON, so the
  register still holds what the last reset set, and adding a latch-and-clear would change
  what a debugger sees on a board whose hardware verification is still deferred. That
  decision was recorded in `dm330030_board.c` and is unchanged.

So the decoder takes `is_latched` rather than reading RCON itself. Without a latch the
bits ACCUMULATE across resets, and naming "the" cause from an accumulation is a
plausible-looking wrong answer -- the failure mode this repo keeps finding in its own
history. The refusal DM330030 used to return was protecting against exactly that, and the
reasoning was sound; it was just implemented as "decline always", which also declines the
common case where one bit is set and the answer is not in doubt. Now:

- `is_latched == true` (EV88G73A): name the most specific bit.
- `is_latched == false` (DM330030): name the bit if exactly ONE candidate is present,
  otherwise return "(multiple bits set, not latched -- read the raw word)".

Same honesty, useful in the unambiguous case.

### Measured, and why EV88G73A grew

| Configuration | before | after | delta |
|---|---|---|---|
| CK64MC105_EV88G73A | 46,383 | 46,527 | **+144 (69% -> 70%)** |
| CK256MP508_DM330030 | 51,954 | 52,338 | +384 (19%) |

From the map: `.text` 0x56 = 86 bytes, `.const` 0xda = 218 bytes. **The `.const` dominates**
-- seven string literals plus the `reset_causes[]` table of `{mask, name}` pairs. EV88G73A
already had those same seven strings inline, so in principle it should have paid almost
nothing; the +144 is mostly the table's pointer array, which the inline ladder did not need.

Adopted with the growth reported and accepted (69% -> 70%, ~20 KB still free on
CK64MC105). The alternative considered was keeping the shared function but reverting its
body to the seven-way ladder, which would drop most of the 218 bytes; the table was chosen
because it keeps the ladder and the "how many candidates are set" test from disagreeing
about what the candidates are, which is the bug a second hand-written list invites.

Registering a new folder costs six edits in `firmware.X/nbproject/configurations.xml`:
two `<itemPath>` entries, two per-configuration `<item>` blocks, and both
`includeDirectories` attributes. Miss the include path and the build fails on the header;
miss the `<item>` block and the object is silently never linked.

### The other two items were NOT done

- **ANSEL sweep** (`dm330030_ports_digital_default()`, ports A-E x 16 bits) as a shared
  function taking a port mask: EV88G73A does not need it at all -- and under
  `--no-gc-sections` it would link the loop body even when called with mask 0. Cost with no
  benefit on the board that can actually be tested. **Superseded on 2026-08-03: the sweep was
  deleted outright** (section near the end), so there is nothing left to share. Note also that
  the reason given here for EV88G73A not needing it -- "none of its pins is analog-capable on
  MC105" -- turned out to be **false**: RD10 (LED0) and RD13 (SW0) both have `ANSELD` bits.
  The conclusion held anyway, because those two pins are configured through a `gpio_table`
  entry that states `.analog = false` itself.
- **I2C1 pin-stage differences** (RP-form vs port/bit-form `set_analog`, internal pull-ups
  on/off, `timeout_ms` 10 vs 0): both boards already drive the same bus (RC8/RC9 = RP56
  ASDA1 / RP57 ASCL1 at 400 kHz). Whether internal pull-ups are wanted may be a fact about
  each board's external resistors, and with no DM330030 hardware that cannot be verified.
  Converging the call FORM alone would be safe; the pull and timeout should not move on
  a guess.

## Hardware verification of the convergence steps (2026-08-03)

Everything from Step 2 onward had accumulated as unverified: `a8dc945`, `4224abc`,
`b783bc8`, `bee5630` and `c4329ba` all say "Not yet re-checked on hardware" in their
commit messages. They have now been checked, on the one board that physically exists.

Setup: EV88G73A Curiosity Nano, dsPIC33CK64MC105, kit MC020023603RYN000772, console on
COM5 @ 230400 through the serial_monitor HTTP bridge (127.0.0.5:8080 -- the lab's own
endpoint, deliberately not the sonora bridge's 127.0.0.1:8080). Clean rebuild of
`CK64MC105_EV88G73A` immediately before flashing, 46,524 bytes / 70%, no compiler
diagnostics. Programming classified Success on UART evidence (`Build ID = hwtest` seen on
the console); STATUS.TXT was the documented OS-cached read and gave no post-copy evidence,
which is why the UART marker is the authority and not the drive.

### What `bee5630` was verified by

The boot banner, which is the only code that reads the clock verdict. All of it correct:

```
Fosc Hz  = 200000000
Fcy Hz   = 100000000
Clock    = PLL at target
PLLstatus= 0
COSC@sw  = 1
LOCK@sw  = 1
```

The first two now come from `dspic33ck_clock_get_fosc_hz()` / `_get_fcy_hz()` where they
used to come from EV-only wrappers, and the rest from the `g_ev88g73a_*` volatiles. The
wrappers being gone is exactly what this proves: the banner prints the same numbers it did
before, through the shape DM330030 already used.

### What `c4329ba` was verified by, including the interesting case

Three observations, in order:

1. After programming: `Reset = EXTR(MCLR)`. Correct -- drag-and-drop programming ends in
   an nEDBG-driven MCLR, not a power-on.
2. `?sr` -> `reset cause this boot: EXTR(MCLR)  RCON=0x0083`. **This is the case the flag
   parameter exists for.** 0x0083 has three cause bits set (POR, BOR, EXTR), and the
   decoder still names one -- legitimately, because EV88G73A latched the word before
   anything could add to it and passes `is_latched = true`. The same word from a
   non-latching board returns the "multiple bits set, not latched" refusal instead. So
   both branches of `dspic33ck_reset_cause_str()` are justified by one real reading, rather
   than the latching branch being the only one anyone ever sees.
3. `*sr` -> `"*sr" software reset now (was: EXTR(MCLR))`, then after the reboot
   `Reset = SWR(software reset)`. This exercises the parts a static read cannot: the latch
   captures the new boot's cause, `dspic33ck_reset_cause_clear()` really did clear the old
   POR/BOR/EXTR so SWR is not buried under them, and the table's priority order puts SWR
   ahead of the power-on causes -- the specific requirement written into
   `nora_reset_dspic33ck.c`'s comment.

### The rest of the console, and two things NOT verified

Passing: DMA selftest (ch3, RAM->RAM) PASS, `?gv` = hwtest, `?dt` SCCP1 read test 256
samples with backward=0 and no steps >= 1024, `?xl` last trap none / traps since power-on
0. Console liveness at 230400 is implied by every exchange above.

NOT verified, and recorded as such rather than assumed:

- **LED0 blinking and SW0 press.** SW0 has no console query in this build -- it is polled
  in the LED loop as the profile's mute button (`main.c:179`) -- and LED0 is physical.
  What was confirmed is only that the loop is running: its periodic status line kept
  appearing well after the last command was answered.
- **The audio path.** The WM8904 is not wired to this Nano, so the codec cannot come up.
  That is the EXPECTED NEGATIVE and it failed in the right shape: `wm8904_read_reg()` I2C
  read failed -> `wm8904_confirm_device_id()` refused -> `apply=FAILED` -> "staying
  stopped". It declines on evidence instead of continuing into silent no-audio, and it did
  not disturb any other bring-up stage -- reset, clock, DMA, timer and trap paths all
  passed with the codec absent.

## Turning on --gc-sections (2026-08-03)

`remove-unused-sections` had been **off** in both configurations, i.e. `--no-gc-sections`:
every linked function and every `extern const` cost flash whether anything called it. That
was not an accident -- most of the size arithmetic recorded above in this document treats it
as a premise, which is why e.g. the Step 4 ANSEL sweep was declined ("EV does not need it
and --no-gc-sections would charge EV for it anyway"). It is now **on** in both.

Measured, both configurations rebuilt clean with the same `-BuildId` string (`gcbase`, so the
Build ID linked into flash is the same length in both halves of the comparison):

| configuration        | program before | program after   | delta      | data before | data after |
|----------------------|----------------|-----------------|------------|-------------|------------|
| CK64MC105_EV88G73A   | 46,524 (70%)   | **44,889 (67%)**| **-1,635** | 4,698 (57%) | 4,694      |
| CK256MP508_DM330030  | 52,338 (19%)   | **45,630 (16%)**| **-6,708** | 6,308 (25%) | 4,922      |

No compiler or linker diagnostics in either build. DM330030 gains far more because it links
more that nothing reaches; EV88G73A is the number that matters, since MC105 is the 64 KB part
and 70% was the figure worth reducing.

Note for anyone re-measuring: EV88G73A also builds at 44,883 with `-BuildId gcon`. That is not
a second improvement, it is the six characters the shorter Build ID string does not occupy.

### Why this needed hardware re-verification, and what was actually at risk

A garbage-collecting link changes which sections survive, so the things at risk are the ones
reached by **hardware** rather than by a call: the trap vectors. Nothing in C calls
`__AddressError`; if the linker judged it unreferenced and dropped it, the build would still
succeed, the banner would still print, and the failure would only appear the first time
something trapped -- which is the worst possible time to discover it.

So the forced-trap commands, not the banner, are the real test here, and all three were run on
the EV88G73A Curiosity Nano (kit MC020023603RYN000772, console COM5 @ 230400 via the
serial_monitor bridge on 127.0.0.5:8080). Clean rebuild immediately before flashing;
programming classified Success on the UART marker (`Build ID = gcon`), STATUS.TXT being the
documented OS-cached read that yielded `Evidence the read is post-copy: NO` as usual.

| command | result | what it proves survived |
|---------|--------|--------------------------|
| `*xa` | `Reset = SWR(software reset)` after reboot | `__AddressError` handler present and it reached the software-reset path |
| `*xm` | `Reset = SWR(software reset)` after reboot | `__MathError`, a separate vector, also present |
| `*xs` | `Reset = TRAPR(trap conflict)` after reboot | the documented hardware path -- see below |
| `?xl` | `traps since power-on: 2` | the persistent-RAM trap record survived the resets and was reported at boot |

The `*xs` line is the interesting one and it is **correct, not a regression**: the BOARD SEAM
comment already records that a stack overflow cannot be reported in software, because the handler's own
context push overflows too, the core sees a trap within a trap, and it resets immediately with
`RCON.TRAPR` set. So `*xs` legitimately produces a different reset cause from the other two --
and that is also why `?xl` reports **2** and not 3. Two traps were software-reportable; the
stack one reset the part before any handler could record it. A count of 3 there would have been
the bug.

Everything previously verified still reads identically: `Fosc Hz = 200000000`,
`Fcy Hz = 100000000`, `Clock = PLL at target`, `PLLstatus= 0`, `COSC@sw = 1`, `LOCK@sw = 1`,
DMA selftest (ch3, RAM->RAM) PASS, `?dt` SCCP1 256 samples `step min=43 max=72 backward=0
steps>=1024=0`, `*sr` -> `SWR(software reset)`, and `?sr` -> `RCON=0x0083` decoded as
`EXTR(MCLR)` by the latching board. The WM8904 still fails to initialise; the codec is not
wired to this Nano, so that is the expected negative and it failed in the right shape.

### One thing this does NOT do, and a consequence to remember

It does not remove dead code from the source tree. `src/app/demo_tdm_master_loopback.c` is
still compiled into the EV88G73A configuration (`ex="false"`) even though only DM330030's
`main.c` calls it -- DM has it at `ex="true"`, which looks backwards. `--gc-sections` now
discards the result, so the flash cost is gone and fixing the `ex=` flag would save nothing
further; it is a tidiness question, not a size one, and is left alone.

The consequence: the size model that justified several earlier decisions no longer holds. In
particular the reason given for declining the shared ANSEL sweep -- that EV would pay for code
it does not use -- **is no longer true**. That does not by itself make the sweep worth doing,
but it does mean the objection has to be re-argued on other grounds rather than inherited from
this document.

## Raw register access removed from the SPI/I2S/TDM HAL: PPS through hal_gpio (2026-08-03)

The instruction was to eliminate **raw register access** by **growing hal_gpio**, with sonora
(100% hal_gpio, its PPS held up as the good model) as the target shape. EV88G73A primary,
DM330030 best-effort.

### What was actually raw

Inventoried rather than assumed. Board and app code already route every pin through hal_gpio;
the one grep hit in `ev88g73a_board.c` is comment prose, not code. The raw SFR writes that
remain are **not GPIO** and are out of scope for this step:

| Site | Registers | Why it stays |
| --- | --- | --- |
| `src/app/app_traps.c` | `INTCON1`, `INTCON3` trap flags | trap-status, no HAL |
| `dm330030_board.c:447-450` | `REFOCON` | clock HAL's territory, not GPIO |
| `ev88g73a/main.c:272-293` | Timer1 | timer HAL's territory |

The single genuine GPIO/PPS offender was `src/hal_spi_i2s_tdm/dspic33ck_spi_i2s_tdm_fs_clc.c`,
which carried its **own** IOLOCK sequence (`__builtin_write_RPCON`), its **own** per-device
RPORx-slot -> RPn tables, and its **own** bank arithmetic to reach `_RPnnR`.

### Why it had gone its own way, and what the HAL was missing

Its header claimed self-containment (xc.h only) as a virtue: "the transport HAL stays
vendoring-portable". It went private because the PPS HAL could not answer three questions:

1. route an output to a **virtual** pin (RPV0/RP176 -- padless PPS endpoint);
2. route an **input** from a virtual pin (`route_input` rejected anything above RP79);
3. **reverse lookup** -- which physical pad currently carries this peripheral output.

All three were added to `hal_gpio/dspic33ck_pps.{c,h}` instead of patched in place.

### The bug that private copy was hiding

RPn is **NOT affine** in the RPORx slot. The two 6-bit fields per 16-bit register make the
*slot* affine, but the *pin numbering* is not: CK64MC105's remappable pins are non-contiguous
above RP61 (RPOR15 = {RP65, RP72}, RPOR16 = {RP74, RP77}), while CK256MP508 is contiguous
RP32..RP79. fs_clc's old `(&RPOR0) + (rp - 32) / 2` formula therefore addressed **unrelated SFR
space** for RP176 -- 48 words past the bank on MP508, 55 on MC105. A silent write into whatever
lives there.

The fix that matters is structural, not arithmetic: the new read table is generated from the
**same per-RP `#ifdef` list** as the existing write switch, so the two cannot disagree about
which pins the device has. One copy of a device fact, one chance to get it wrong.

```c
typedef struct { uint8_t rp; uint8_t slot; } dspic33ck_pps_rpor_entry_t;
static const dspic33ck_pps_rpor_entry_t s_pps_rpor[] = { ... };   /* per-RP #ifdef'd */

#if defined(_RP65R) && defined(_RP64R)
    DSPIC33CK_PPS_RPOR_ENTRY(65u, 33u)   /* MP508: contiguous after RP64 */
#elif defined(_RP65R)
    DSPIC33CK_PPS_RPOR_ENTRY(65u, 30u)   /* MC105: RPOR15 low field      */
#endif
```

### A table, not a mirror switch -- measured

`_RPnnR` is a readable bit-field alias, so a 48-case read switch is the obvious mirror of the
write switch. Measured on EV88G73A **with `--gc-sections` on**, it cost **326 bytes** of flash
(`find_output_rp` alone spanned 0x4a4e..0x4b94 in the map). The table sweep is a few
instructions plus two bytes of const per pin: `find_output_rp` came down to **80 bytes**.

Read-only, so it never touches IOLOCK. Called only at bring-up -- no hot path -- so the linear
sweep costs nothing that matters.

### The other duplicate deleted

`hw_get_ss_pps_code()` returned a raw `uint8_t` from a per-device `_RPOUT_SS1OUT` / `_RPOUT_SS1`
`#ifdef` ladder -- a ladder `dspic33ck_pps.c` already carried for `DSPIC33CK_PPS_OUTPUT_SSx`. It
now returns the **enum** instead:

```c
bool dspic33ck_spi_i2s_tdm_hw_get_ss_pps_output( tdm_spi_inst_t inst,
                                                 dspic33ck_pps_output_t* output );
```

leaving it doing the one thing only the transport HAL knows -- which SPI instance owns which
frame-sync signal. Device availability is now answered inside the PPS HAL (`get_output_code`
returns false for an absent SS3), so behaviour is preserved.

### The self-containment claim was retired, not quietly dropped

The header's rationale was replaced rather than deleted, because it was a real argument that
lost on evidence: the private copy of the register map is exactly where the non-contiguous-RP
bug lived, and a second copy of a device fact is a second chance to get it wrong. The PPS HAL is
a **sibling within the same hal_gpio family** and travels with it, so the dependency is inside
the family rather than across it.

`fs_clc.c` now contains zero matches for the raw-PPS pattern
`RPOR|_RP[0-9]+R|_CLCINAR|__builtin_write_RPCON|_RPOUT_` (verified by grep). What is left is the
CLC1 J-K flip-flop configuration, which is genuinely this module's own.

### Size: EV went UP, which trips the stop rule

Both measurements used `-BuildId ppsref` (6 chars) so the linked build-ID string cannot skew the
comparison; the baseline was re-verified at exactly 44,889 via `git stash push` / `pop`.

| Config | Before | After | Delta |
| --- | --- | --- | --- |
| CK64MC105_EV88G73A | 44,889 | **45,024** | **+135** |
| CK256MP508_DM330030 | 45,630 | **45,894** | **+264** |

Netted per object rather than reported as a total only:

| Object | text | const |
| --- | --- | --- |
| `dspic33ck_pps.o` | +278 | +68 |
| `dspic33ck_spi_i2s_tdm_fs_clc.o` | -224 | -40 |

fs_clc shrank **264 bytes (60%)**. The HAL grew more than fs_clc shrank because
`find_output_rp` is **general** where fs_clc's private version was hardcoded to its one caller.

The standing rule is "measure EV's bytes each step and stop if they increase." EV increased, so
this is **reported for a decision, not pushed past**. What the +135 buys: three new PPS HAL
capabilities available to every future caller, one deleted duplicate of the device register map,
and the removal of the bug class that map produced. What reverting buys: 135 bytes.

### EV88G73A hardware verification (all PASS)

Clean-rebuilt and flashed; `flash-curiositynano.ps1` reported `UART verification: marker seen
(matched 'ppsref')`, `Classification: Success`.

| Check | Result |
| --- | --- |
| Banner | `Reset = EXTR(MCLR)`, `Fosc Hz = 200000000`, `Fcy Hz = 100000000`, `Clock = PLL at target`, `PLLstatus= 0`, `PLLPRE=1`, `PLLFBDIV=200`, `POST1DIV=2`, `POST2DIV=2`, `COSC@sw = 1`, `LOCK@sw = 1` |
| DMA selftest | ch3 RAM->RAM software CHREQ: **PASS** |
| WM8904 init | FAILED -- expected negative, codec not wired to this Nano |
| `?gv` | `build ID = ppsref` |
| `?sr` | `EXTR(MCLR)  RCON=0x0083` |
| `?dt` | SCCP1 256 samples: `step min=43 max=72 backward=0 (worst=0) steps>=1024=0` |
| `*xa` | `Reset = SWR(software reset)` -- address-error handler survives |
| `*xm` | `Reset = SWR(software reset)` |
| `*xs` | `Reset = TRAPR(trap conflict)` -- **correct, not a regression** |
| `?xl` | `traps since power-on: 2` -- **2, not 3** |
| `*sr` | `(was: TRAPR(trap conflict))` then clean `SWR(software reset)` reboot |

The last two are the subtle pair. Per the BOARD SEAM section of `src/app/app_traps.h`, a stack overflow **cannot** be
reported in software: W15 is past SPLIM, so the handler's own context push also overflows, the
hardware sees a trap within a trap and resets immediately with `RCON.TRAPR`. So `*xs` yielding
`TRAPR` is the documented behaviour, and the trap counter must read **2** -- a count of 3 there
would have been the bug.

Note on reading the log: filtering for `*xa` also matches the help line
`?xl last trap  *xa/*xm/*xs force address/math/stack trap`. Anchor on the TX marker (`>> *xa`)
instead.

### Scope note

DM330030 builds clean at 45,894 and usefully exercises the **other** branch of the new
divergent RPOR table (MP508 contiguous vs MC105 non-contiguous), but was not flashed -- no DM
hardware this session, and "best-effort" was the instruction.

## The WM8904 driver is vendored, and the rules for re-syncing it (2026-08-03)

Absorbed from `ck_wm8904_audio_passthrough.md`, now deleted — the file names and the
board-layer story in it were already superseded by "One WM8904 audio path for both boards"
above, but these facts were not recorded anywhere else.

`board_components/wm8904.{c,h}` + `wm8904_def.h` are a **near-verbatim copy** of the
upstream audio project's driver. `wm8904_def.h` (register facts) is copied untouched. The
adaptation is mechanical and deliberately stays that way, so the next re-sync is a re-apply
rather than a merge:

- `dspic33ak_i2c_*` → `dspic33ck_i2c_*`, `DSPIC33AK_I2C_*` → `DSPIC33CK_I2C_*`;
  `wm8904_i2c_hal_inst()` maps legacy instance 1 → `DSPIC33CK_I2C_INST_1`.
- Every app-layer include from upstream (`resolved_board_config.h`,
  `resolved_transport_config.h`, `app_runtime_overrides.h`, `app_specific_config_defs.h`,
  `timer_app.h`) is replaced by the single shim **`board_components/wm8904_port.h`**, which
  supplies `delay_ms`/`delay_us` (`libpic30 __delay_*`), `GetTicks`
  (`dspic33ck_tick_timer_get_ms`), `TRACE` → `printf`, `COMPILEASSERT` → `_Static_assert`,
  and the audio framing macros (`APP_USE_I2S_FORMAT=0`, `APP_USE_1_BIT_DELAY=0`,
  `APP_SLOTS_PER_FS=8`).
- `#if defined(ENA_CMSIS_I2C)` branches are inert here (never defined) and left in place.

**Two upstream config macros are defined explicitly rather than left undefined-as-0** —
`RESOLVED_BOARD_CODEC_INPUT_IS_RED_JACK` and `RESOLVED_BOARD_CODEC_MIC_BIAS_ENABLED`, both
0 (BLUE input jack, mic bias off: this is a line-in→line-out path, not a mic path).
Undefined-as-0 would have been the same behaviour and no record of the choice.

Kept-but-unused, on purpose, because stripping them makes the next re-sync a diff to
resolve instead of a copy: `wm8904_set_rate_hz()`/`_get_rate_hz()` (the full
8/11.025/12/16/22.05/24/32/44.1/48 kHz table — FLL-less 48 k family, FLL-based 44.1 k
family; nothing here calls it, so it stays at the 48 kHz default),
`wm8904_reg_write()`/`_reg_read()` (upstream's hook for its interactive console),
`wm8904_is_distinct_slave()` (tells a genuine second codec from a bridged A/B I2C bus
aliasing the master codec back onto the query — for a future dual-codec board), and the
declick-research restart-strategy bitmask (`wm8904_declick_mask_t` and friends: nothing
arms a mask, so the shipping WSEQ-shutdown/manual-startup sequence is always what runs).

**`wm8904_init()` returns `bool`, and that return is load-bearing.** True only once the
device ID readback *and* every configuration write/readback verified. The audio module
checks it and stays stopped — no SPI arm, no unmute — on failure. Continuing past an
unconfirmed codec is exactly the unverified-assumption shape `ck_silicon_findings.md` is
about.

### Clocking numbers for the codec-master topology

The default topology is the upstream non-USB one: **the WM8904 is the audio-clock master**
(it drives BCLK/FS/LRCLK) and the dsPIC SPI1 is a TDM8/32-bit **slave**. The codec still
needs a system clock, and where it comes from is the one thing that differs per board — on
DM330030 the dsPIC divides it out of REFO1, on EV88G73A the codec self-clocks from its own
X1 crystal (which is why `port->mclk_init` is NULL there — a fact, not an omission). **[FALSE, corrected 2026-08-04: same WM8904 board and same XTAL on both -- the stage and the hook are deleted. See the MCLK section near the end.]**

- DM330030: Fosc 200 MHz, REFO1 `ROSEL=FOSC`, `RODIV=16` → **MCLK 12.5 MHz**. The literal
  `16` was a defect; see "Review follow-up" above — the divider is derived from the
  *achieved* Fosc now, because on the FRC it emitted 500 kHz and reported success.
- WM8904 48 kHz TDM8/32-bit: SYSCLK = MCLK, **SYSCLK/fs = 256** → fs ≈ **48.83 kHz**;
  BCLK = SYSCLK = 12.5 MHz (= 256·fs); LRCLK = fs; `BCLK_DIV = 1`, `LRCLK_RATE = 256`. The
  same operating point the upstream project verified in dsPIC-master mode.
- The transport is rate-agnostic: as slave the dsPIC simply locks to the incoming BCLK/FS.

Bring-up **order**, which is what the deleted file was really for — MCLK must exist before
the codec will accept configuration over I2C:

1. high-res timer (load monitor)
2. **MCLK** (or nothing, on a self-clocked codec)
3. I2C1 pins + `dspic33ck_i2c_init` @ 400 kHz
4. `wm8904_init()` — ID 0x8904, configure, start BCLK/FS/LRCLK
5. configure + `open(SLAVE)` + `inst_start()` the dsPIC SPI1 TDM8/32-bit slave
6. `is_running()` gate → un-mute the analog output

### The `ALTI2C1` fuse polarity is the opposite of the datasheet's bit table

Both boards use the **alternate** I2C1 pair, for the same reason: the default SDA1/SCL1
(RP40/41 = RB8/RB9) double as PGC1/PGD1 and are wired to the on-board debugger. EV88G73A:
ASDA1 = RP56 (RC8), ASCL1 = RP57 (RC9). DM330030: the same alternate pair is what
mikroBUS-A carries (datasheet TABLE A-1, "Parallel MikroA, MikroB") — which is why the
older `ENA_I2C_WM8904_PROBE`, written against the default pins, could never have worked.

**`#pragma config ALTI2C1 = ON` selects ASDA1/ASCL1 (field = 0); `OFF` selects the default
pins (field = 1).** The pragma's ON/OFF sense is the *inverse* of what the datasheet's raw
bit table reads at face value (`1 = default pins`), so this was confirmed against the
part's own DFP (`DSPIC33CK64MC105.PIC`) semantic names rather than trusted from the prose.

Also confirmed, and it makes this pin pair unlike every other pin in the profile: **I2C1
does not appear in this device's PPS input or output tables at all.** The fuse is a fixed
two-way pin-pair select, not a remap — so there is no `nora_pps_route_*` call for I2C.

### The I2C1 probe, measured with nothing attached

The mechanism was verified on EV88G73A hardware before any codec existed, by a standalone
probe that classifies the result into **OK / clean NACK / inconclusive** (timeout, bus
error, collision) rather than pass/fail — the same transfer-vs-data distinction the SPI/DMA
audit was built around. Weak internal pull-ups turn a floating idle bus into a clean high
first (still not a real bus: no codec, no real capacitance).

```
I2C1 probe: ASDA1=RP56(RC8) ASCL1=RP57(RC9), 400 kHz, no codec attached
  target addr7=0x1A reg=0x00 -> status=ERR_NACK rx=0x0000
  I2C1 probe: PASS (mechanism) -- clean NACK, exactly what "module works, nothing attached" looks like
```

A clean `ERR_NACK`, **not** a timeout and not a bus error: the module drove a real START,
clocked the address out, and correctly detected no ACK. That is as far as "I2C confirmed
standalone" can go without a codec present — it establishes the pins, the fuse and the I2C1
HAL, and defers device-ID readback and full config to a session with a codec. The TDM8
stream was running throughout and stayed unaffected (`blocks` climbing, `miss=0`).

### Still open on the DM330030 side

`DM330030_AUDIO_RP_MCLK = RP69/RD5` is a **placeholder** — confirm which mikroBUS-A
pin actually feeds the WM8904 board's MCLK input, and that the jumper carries it. Nothing
on DM330030's audio path has been on hardware; there is no board.

> **CLOSED 2026-08-04, by deletion rather than by confirming the pin.** There is no MCLK pin
> on this board: same WM8904 board, same XTAL, and 12.5 MHz was the wrong figure regardless
> (the driver's rate tables are solved for 12.288 MHz). See the MCLK section near the end.

---

## Pin macros name the role, not the connector (2026-08-03)

The T4 rule ("facts keep the connector, functions take the role") stopped at the function
names. `dm330030_pins.h` still spelled the same four signals
`DM330030_MIKROBUS_A_{BCLK,FS,SDO,SDI}_RP` while EV88G73A spelled them
`EV88G73A_TDM_RP_{BCLK,FS,SDO,SDI}` — so "mikroBUS" had been driven out of the function
names and left in the identifiers those functions pass around. The macros now match:

| was | now |
|---|---|
| `DM330030_MIKROBUS_A_{BCLK,FS,SDO,SDI}_RP` | `DM330030_TDM_RP_{BCLK,FS,SDO,SDI}` |
| `DM330030_MIKROBUS_A_MCLK_RP` | `DM330030_AUDIO_RP_MCLK` |
| `DM330030_MIKROBUS_A_{ASCL1,ASDA1}_PIN` | `DM330030_I2C1_RP_{ASCL,ASDA}` |

**A fact is not an identifier.** Which connector carries a signal is a real board fact, and
it is now written out where a reader with the board in front of them looks: a table in the
macro comment giving role → RP → port → mikroBUS-A pin → the WM8904 board's own name for it,
plus the `_WIRING_STR` strings the boot report prints. What the code passes around is the
ROLE, because that is what the peripheral, the HAL and the bring-up each care about — none
of them cares which connector it arrived on.

**The I2C1 pair moved to RP form in the same step**, which closes the "call form" half of the
I2C1 pin-stage divergence below: `dm330030_i2c1_pins_init()` now calls
`nora_gpio_rp_set_analog()` like every other pin stage on both boards, instead of being
the one place that spoke port/bit. Both configurations build byte-identical in size to
before, as a pure renaming should.

---

## SW0 → LED0 blink pause (2026-08-03)

`LED0 blink` and `SW0 press` were the only things in this profile with no hardware evidence,
for a structural reason: LED0 was only ever *watched* (a running loop and a working pin look
the same), and SW0 was read in exactly one build — the audio one, where it is the mute
button. So neither had a test; both now share one.

**Press SW0 → the blink stops. Press again → it resumes.** A press that does nothing, or a
blink that never pauses, isolates one of the two pins. Implemented in
`boards/ev88g73a/main.c`:

- **Gated on `!EV88G73A_ENABLE_WM8904_AUDIO`.** In the audio build SW0 is the mute button;
  one press meaning both "mute" and "stop blinking" makes neither observation clean. Gating
  on the audio switch itself (rather than a new define) means the two uses cannot both be
  compiled in.
- **The LED pauses, the cadence does not.** The 1.000 s blink is also the live clock
  measurement, so the Timer1 wait, the console, the report gate and every exerciser keep
  running while the LED sits still — "I paused it" and "it hung" must stay distinguishable.
  What distinguishes them is **the console still answering commands** (`console_task_poll()`
  runs inside the Timer1 wait, so it is alive in every build) — *not* the periodic status
  line, which at the time this test was written was the one throttled to ~9 hours on this
  board and so proved nothing either way. (That throttle is now
  `DEMO_TDM_STATUS_PERIOD_MS = 5000` ms of running time — see the cadence entry above — so
  the status line has become a second, slower liveness signal. The console remains the
  primary one.) The LED
  parks OFF, and each press prints `SW0: LED0 blink paused (loop still running)` / `resumed`.
- **Sampled at 100 ms and latched**, inside the Timer1 wait rather than once per 500 ms
  iteration: a human press is 100–300 ms, so a per-iteration sample would miss taps and look
  like a broken button, which is the ambiguity the test exists to remove. No debounce —
  bounce is an order of magnitude below the sampling interval.

Cost: +198 B on the loopback build (47187 vs 46989). The audio build is byte-identical to
before, which is the gate working.

**PASS on hardware, 2026-08-03** (EV88G73A / MC020023603RYN000842, COM28, TDM loopback build,
build ID `3681e92-dirty-…`). Three press/release cycles, each producing exactly one
`paused` / `resumed` line — so the 100 ms latch caught every tap and no press double-fired.
The user confirmed the physical LED: **"LED OK"**, blink stopping and restarting with the
presses. That closes both pins: LED0 drives the physical LED, SW0 reads.

**And the pause is only the LED.** While paused (18:56:49 → 18:58:03), `?dt` was sent and
answered normally — `SCCP1 read test, 256 samples: step min=43 max=73 backward=0` — so the
console, the Timer1 wait and the loop were all still running with the LED parked. That is
the paused-vs-hung distinction actually exercised, not merely asserted. `?gv` returned the
matching build ID on the same connection.

---

## Three functions one board had and the other did not (2026-08-04)

Not a new technique — the same convergence work, applied to the leftovers that are
`<board>_x()` **existing on one side only** while both boards do the same thing. That shape
is what makes a side-by-side read of `dm330030_board.c` and `ev88g73a_board.c` expensive:
a reader cannot tell "this board does something extra" from "this board says the same thing
in more functions".

### 1. `ev88g73a_uart1_init()` → the shared `uart_platform_stdio_init()`

**This was recorded as out of scope** in step 1 above ("a different path, not a different
parameter") because DM330030's console *is* `printf` and EV88G73A's is not. That reason
holds for the **write** side and was over-applied to the **bring-up** side. Checked field
by field before touching anything: EV88G73A's local `dspic33ck_uart_config_t` set all
eleven fields to exactly what `uart_platform_stdio.c` sets — Fcy from the Clock HAL, the
caller's baud, `timeout_ms = 0` with `get_ms = NULL`, 8N1, `high_speed`, `BCLKSEL =
FOSC_DIV2`, TX and RX both enabled. Not similar; identical.

So `ev88g73a_board_init()` now makes the same two calls DM330030's does: its own pin stage,
then `uart_platform_stdio_init(baud)`. `g_ev88g73a_uart1_init_status` is gone with the
function — the status is `g_console_init_status`, one symbol for both boards, and it is the
name the bring-up docs already tell a debugger to look for. Nothing in the tree ever read
the board-prefixed one.

One thing the shared file gained, because EV88G73A needs it and DM330030 does not: the
console instance is now `UART_PLATFORM_CONSOLE_UART_INST` in the header rather than a
private `#define` in the `.c`. EV88G73A must name the instance itself (its `console_out`
transport addresses the UART directly), so `ev88g73a_board.c` holds a `_Static_assert`
tying `EV88G73A_CONSOLE_UART_INST` to it. Two statements of one fact are fine when the
compiler refuses to let them drift. DM330030 names no instance at all — everything it
prints goes through `printf`, hence through the `write()` hook.

### 2. `ev88g73a_reset_cause_raw()` / `_str()` → the BOARD SEAM names directly

`board_reset_cause_raw()`/`_str()` were one-line forwards to a board-prefixed pair: two
public names per question, where DM330030's seam functions answer directly. The seam
functions now read the latch and call the shared decoder themselves, and the pair is gone
from `ev88g73a_board.{c,h}`. `main.c`'s banner — the only caller outside the seam — now
asks `board_reset_cause_str()`, the same name the shared `*rc` console command uses; it was
already getting the identical string through the longer path.

**The latch is untouched and stays visible**, because it is the one genuine difference:
`ev88g73a_board_init()` captures and clears RCON before anything else runs (no reset button
on a Nano, so POR-vs-SWR is the only evidence `*sr` worked), DM330030 reads RCON live. That
difference is one line in `board_init()` plus the `true`/`false` argument to
`nora_reset_cause_str()`.

### 3. `dm330030_i2c1_init_at(bus_hz)` → deleted, one function per board

The mirror-image case, on the other board. The static parameterised layer survived the
collapse to one I2C rate on the grounds that "the rate is still the board's business";
that is true and does not need a function — it is the `DM330030_I2C1_BUS_HZ` define, which
is where EV88G73A has always kept it. This supersedes the note in the mikroBUS-naming
section that says the parameterised form "stays static".

### Measured

Both configurations rebuilt clean with `-BuildId measure` on both sides (same string
length, so the absolute numbers are comparable):

| configuration | program before | program after | delta | data before | data after |
|---|---|---|---|---|---|
| CK64MC105_EV88G73A | 45,738 (68%) | **45,756 (68%)** | **+18** | 4,712 (57%) | **4,708** |
| CK256MP508_DM330030 | 46,485 (17%) | 46,485 (17%) | 0 | 4,932 (20%) | 4,932 |

No compiler or linker diagnostics in either build. DM330030's zero is the `_init_at`
removal being a pure inline collapse.

**EV88G73A's +18 is the interesting number, and it is a `--gc-sections` effect** — worth
recording because it inverts the intuition that de-duplication is free. The before-map's
*Discarded input sections* list shows `uart_platform_stdio.o`'s entire **54-byte `.text`
dropped** from the EV88G73A image: the file was compiled in (`ex="false"` in both configs)
and its 4-byte `.libc.write` section was kept because libc references it, but nothing
called `uart_platform_stdio_init()`, so the init was collected. Calling it links those 54
bytes; deleting the local config, the two reset-cause forwarders and the
`uart_baud_applied` static gives back about 36. Net +18, i.e. EV88G73A now pays for a
function it previously did not link.

The −4 of RAM is `uart_baud_applied` becoming a local in `ev88g73a_board_init()`. It was a
file static only because `ev88g73a_uart1_init()` was a second function that had to read it;
with the peripheral stage shared, the rate is decided and passed within three lines. It is
deliberately not kept as debugger evidence the way DM330030 keeps
`g_dm330030_console_baud`, because on this board the better answer already exists —
`nora_uart_get_baudrate()` reports what the HAL *applied*, not what the board asked
for, and `ev88g73a_board.h` has a standing note refusing to blur those two.

Reported rather than absorbed, per the standing rule that any EV88G73A growth is the user's
call, and adopted with the number in hand — 45,756 is 68% of the 64 KB part.

### Hardware

**Not re-verified on hardware yet.** The change is intended to be behaviour-identical —
same eleven register-level UART settings, same call order, same strings — but "intended"
and "verified" are different words, and the console is the thing at risk: the banner at
230400, its `Reset =` line, and `*sr` reporting `SWR` afterwards are what would show a
regression. DM330030 stays compile-only as always.

### Still divergent after this

The remaining differences between the two board files, all of them deliberate:

- **capability** — EV88G73A's RCON latch; EV88G73A routing UART RX where DM330030 is
  TX-only. **`dm330030_mclk_init()` was NOT one of these** and is deleted — see the next
  section: same WM8904 board and same crystal on both.
- **electrical** — EV88G73A's I2C internal pull-ups; the inert `timeout_ms` 10 vs 0. Both
  blocked on hardware, see the section below.
- **drift, and the next candidate** — the **console-rate predicate**. DM330030 asks whether
  the *achieved* Fcy can represent 230400 (`DM330030_CONSOLE_FAST_MIN_DIV`, a divisor
  check); EV88G73A asks whether the clock request *succeeded*
  (`g_ev88g73a_clock_on_target`). At both boards' actual operating points the two rules
  pick the same rate — 100 MHz Fcy gives divisor 108 (fast), the 4 MHz FRC fallback gives 4
  (safe) — but EV88G73A still carries the bug DM330030's predicate was written to fix:
  build it with `EV88G73A_TARGET_FOSC_HZ` set to the FRC and the request "succeeds" at
  4 MHz, so it would choose 230400 and go silent. Converging it is a behaviour change on
  the console, so it wants its own hardware pass.

---

## `dm330030_mclk_init()` rested on a false premise — deleted (2026-08-04)

**The last remaining difference between the two board files was not a capability asymmetry.
It was a leftover from a hardware claim that is wrong**, and it is now gone, together with
its pin, its wiring-string clause, and the `wm8904_audio_port_t.mclk_init` hook it filled.
The two board files have no section either one lacks.

The investigation ran first and the deletion followed a turn later, once the sibling tree's
evidence was on the table (third subsection below) — recorded in that order because the
first pass deliberately stopped at "no code change".

### The hardware, corrected

**Both boards hang the SAME WM8904 board off mikroBUS/jumper, and that board carries its own
XTAL. Both processors are the same dsPIC33CK family.** So neither codec needs the MCU to
supply MCLK, and there is nothing about MCLK that differs between the two setups.

What the tree says instead, in four places, is that this is EV88G73A-only:

| location | the claim |
|---|---|
| `dm330030_board.h` seam table | `(none -- codec has X1)` in the EV88G73A column |
| `dm330030_board.h` | `dm330030_mclk_init()` … "**REQUIRED on this board** and absent on EV88G73A, which is the one real asymmetry here: that codec self-clocks from its own X1" |
| `dm330030_pins.h` | "the opposite of EV88G73A, whose WM8904 board self-clocks from its own X1 crystal" |
| `ev88g73a_board.h` | "THERE IS NO `ev88g73a_mclk_init()`, AND THERE MUST NOT BE … on that board the dsPIC supplies the codec's master clock" |
| `wm8904_audio.h` / `.c` | `mclk_init` is "NULL when the codec self-clocks from its own crystal — which is EV88G73A's case"; "the codec's SYSCLK comes either from its own crystal or from REFO1" |

`dm330030_board.h` **contradicts itself two paragraphs apart**: the same comment block opens
with "the same WM8904 board hangs off both boards in this repo, and it fills the same three
`wm8904_audio_port_t` slots on each" and then prints a seam table whose EV88G73A column is
empty because "codec has X1". Both cannot be true.

### The frequency is wrong too, independently of the premise

This is the part that turns "unnecessary" into "would be a defect if it ever ran". The
shared codec driver is written around **SYSCLK = MCLK = 12.288 MHz**, the WM8904 board's
crystal:

- `chip_drivers/wm8904.c` rate table: every row's `CLK_SYS_RATE` (SYSCLK/fs) and `BCLK_DIV`
  code is solved from 12.288 MHz for the 48 k family (`use_fll = false`, "SYSCLK = MCLK
  12.288M").
- The 44.1 k family's single shared FLL setting derives from the same input: `FVCO =
  FREF(12.288M) × N.K(7.35) × FRATIO(1) = 90.3168 MHz`, chosen to land in the required
  90–100 MHz window, giving SYSCLK 11.2896 MHz.

`dm330030_mclk_init()` emits **12.5 MHz** — from its own unrelated reasoning ("256 × fs at
fs ≈ 48.8 kHz"), not from anything the driver believes. That is **+1.7 %**, which mis-scales
every `CLK_SYS_RATE`/`BCLK_DIV` row and moves the FLL's `FREF` out from under the constants
above. And it would arrive on the net the codec board's own crystal already drives, so the
electrical outcome depends on that board's oscillator wiring — which is not in hand here.
Either way it is a second clock source at the wrong frequency for a codec that already has
the right one.

### The sibling tree solves the same problem and never GENERATES MCLK

`dspic33ak-audio-dsp-sonora` runs the same WM8904 on dsPIC33AK, and it is the
reference for what the board layer's MCLK job actually is. `src/board/audio/audio.c`:

```c
// MCLK is the ONE clock whose SOURCE varies (axis B), and it varies by a BOARD/compile
// fact, NOT by any leg's master/slave role (axis A):
//   - USB-audio bridge      : dedicated MCLK net (CLCINC RP26).
//   - controller-clocked    : WM8904-B reuses the owning leg's BCLK as its MCLK.
//   - codec-master (default): the board's MCLK net (CLCINC RP16).
static bool board_route_mclk( void )
```

Three things follow, and all three are exactly what the CK stage got wrong:

1. **It ROUTES, it does not generate.** Every arm is a `CLC_PASSTHROUGH(clc, in_rp, out_rp)`
   of a clock that already exists — a BCLK, or the board's MCLK net. No REFO, no divider, no
   frequency arithmetic in the board layer at all.
2. **In the codec-master case it routes NOTHING, and says why**: "The dsPIC must NOT drive
   B's MCLK net -- route NOTHING to CLC3OUT/RP97, leaving RP97/RG0 a non-driving input …
   so it cannot contend with the jumper-supplied XTAL on B's MCLK." That is a warning
   against precisely what `dm330030_mclk_init()` did.
3. **MCLK's source is orthogonal to master/slave.** The header says so outright, and
   `audio_transport_board_clc_passthrough(role)` takes the role and then `(void)role`s it,
   with the note that the role-conditional version was "the old role-conditional mess".

That third point answers the objection that would otherwise keep the stage alive: WM8904 and
the MCU can trade TDM master/slave freely — on AK, and on CK, where the master direction is
already hardware-verified (`docs/ck_silicon_findings.md`, Stage D). The codec needs its
SYSCLK in every one of those combinations — but an MCU-generated MCLK is not the answer in any
of them, for reasons that hold whatever the board supplies it from. So there is no role, and no
board, for which this stage belongs here.

> **Narrowed 2026-08-09.** This paragraph ended "and in every one of them the XTAL on the codec
> board supplies it". How the codec's SYSCLK physically arrives in each jumper position is a
> board fact this repo has **not measured** (`nora_sonora_api_alignment_plan.md` §20.4), and the
> argument against the stage never needed it. **Everything above this note in this document is a
> historical record and quotes the old comments verbatim on purpose** — those quotations are
> left as they were; the live claims are the ones in `src/` and in the parity contract.

### And it has never run

`DM330030_AUDIO_RP_MCLK` = RP69/RD5 is a **placeholder**, already flagged as such in
`dm330030_pins.h`: chosen because it is a free mikroBUS-A pin, not because anything says the
WM8904 board's MCLK input lands there. No DM330030 board exists here. So the path is
"never executed, not known to be connected, and wrong if it were".

### Why extraction into `hal_clock` was considered and dropped

The obvious move by analogy with steps 1–3 was to leave the stage one-sided but push the
REFO1 register writes and the exact-divisor check into `hal_clock` (REFO1 is a family
peripheral, and this board file is the only raw `REFOCONL/H` access in the tree — against
the grain of the "raw register access removed from the SPI/I2S/TDM HAL" pass). That was
started and abandoned once the premise above collapsed: **extracting the mechanism would
dignify and preserve a stage the hardware does not want.** The `hal_clock` REFO work is only
worth doing if some board genuinely needs the MCU to source a reference clock. None here
does.

### The resolution: deletion, not convergence

MCLK handling is now identical on both boards because neither has any. What went:

- `dm330030_board.c` — `dm330030_mclk_init()`, `DM330030_WM8904_MCLK_HZ`,
  `DM330030_REFO_RODIV_MAX`; the `<xc.h>` include comment reduces to RCON; the MCC-block
  note's `REFOCONL/REFOCONH` paragraph (which says this function owns REFO1) and the ANSEL
  audit's "REFO1 MCLK pin" line both lose their subject.
- `dm330030_board.h` — the declaration, the seam-table row, and the `REQUIRED on this board`
  bullet.
- `dm330030_pins.h` — `DM330030_AUDIO_RP_MCLK` and its comment block; `MCLK on REFO1 (RP69)`
  in `DM330030_AUDIO_WIRING_STR`.
- `boards/dm330030/main.c` — `.mclk_init = dm330030_mclk_init` in the audio port.
- `app/app_config.h`, `app/wm8904_audio.{h,c}` — the "supply MCLK on REFO1" and "either from
  its own crystal or from REFO1" wording, and the `ev88g73a_board.h` note that states the
  premise most forcefully.
- `app/wm8904_audio.h` — **the `mclk_init` hook itself**, which had been left open as a
  sub-decision and went too: with both boards passing `NULL` it was dead code that also
  stated the false premise in its own doc comment. `wm8904_audio.c` loses the NULL-check,
  the call, and the `"WM8904 audio: MCLK init failed"` string, and its numbered steps
  renumber from 1. The header now records what to build INSTEAD if a board ever needs the
  MCU to source a reference clock: sonora's routing hook, keyed on board/compile facts —
  not a generator hook, which is what invited this defect.
- Comments that asserted the premise, in `boards/*/main.c` (both audio ports and DM330030's
  `dspic_is_master` justification), `ev88g73a_board.h`, `app/app_config.h`,
  `app/wm8904_audio.c`, and the DM330030 board file's MCC-block and ANSEL-audit notes. These
  are corrected in place with the old wording quoted, because "the tree used to claim X" is
  the part a future reader needs in order to trust the rest of the comment.

### Measured

Clean rebuilds, `-BuildId measure` on both sides:

| build | before | after | delta |
|---|---|---|---|
| CK256MP508_DM330030 (default) | 46,485 | **46,272** | **−213** |
| CK256MP508_DM330030 `ENA_WM8904_AUDIO=1` | 46,782 | **46,572** | **−210** |
| CK64MC105_EV88G73A (audio path is its default) | 45,756 | **45,678** | **−78** |

Data memory unchanged in all three. No compiler or linker diagnostics. The two variants that
do not use the audio port also build clean, since `app/` files changed:
`EV88G73A_ENABLE_WM8904_AUDIO=0` (44,709) and `ENA_TDM_MASTER_LOOPBACK=1` (48,069).

**EV88G73A shrinks even though it never had the stage**, and that is the hook's cost showing
up: the NULL-check, the call site, and above all the 31-byte failure string were linked into
a build that could never print them. It also returns EV88G73A to below its pre-convergence
size for the day: 45,738 at the start, 45,756 after the three-function step, 45,678 now.

### Hardware

**Not verified on hardware.** EV88G73A can be re-run (its audio path is the default build)
and DM330030 remains compile-only. What the change cannot break on EV88G73A is the codec
clock itself, since that board never supplied one; what it does change there is the shared
module's step sequence, so the audio smoke test is the check.

---

## Open items (2026-08-03)

Carried over from the FS-alignment handoff note when it was deleted, so they are not
rediscovered from scratch. These are **open questions, not a plan**.

### DONE 2026-08-03: cadences are milliseconds of running time, not loop counts

**`app/timer_app.{c,h}` is the running time**, and it is the CK counterpart of sonora's
`src/timer_app/` under the same file name and the same idiom, so the two trees read alike:

```c
static uint32_t last;
if ((uint32_t)(GetTicks() - last) >= INTERVAL_MS) { last = GetTicks(); /* due */ }
```

The cast-and-subtract is what makes the test correct across the 32-bit millisecond rollover
(~49.7 days); the header says so at the use site, because it is the part an edit is likely to
"simplify" into `GetTicks() >= last + INTERVAL_MS`, which stops firing for one interval.

**What was wrong, and it was three separate things:**

| | |
|---|---|
| the tick had no owner | The only `GetTicks()` in the tree was a `static inline` inside `chip_drivers/wm8904_port.h`. "What time is it" was reachable only by including a **codec driver's porting shim**, so it compiled out of every profile without a codec. |
| the vector belonged to nobody on the board that exists | `_T1Interrupt` was in `app/timer_1ms.c`, which is `ex="true"` in the EV88G73A configuration. That board therefore **could not run a 1 ms tick at all**, which is why its blink polled `IFS0bits.T1IF` by hand. |
| so the timestamps lied | `wm8904.c`'s TRACE lines print `GetTicks()`. On EV88G73A they printed `0`. After the change the same lines read `@40` / `@49` — measured on hardware, and the clearest single piece of evidence that the seam was missing rather than merely untidy. |

**What each cadence became:**

| where | was | is |
|---|---|---|
| `demo_tdm_master_loopback_poll()` | `(throttle++ & 0xFFFFu)` — one line per 65,536 calls, i.e. a few seconds on DM330030 and **~9 hours** on EV88G73A | `DEMO_TDM_STATUS_PERIOD_MS = 5000`. The first call still prints immediately: that line is the one that says whether the transport came up. |
| EV88G73A LED0 | a count of 100 ms Timer1 periods, polled with interrupts disabled | `EV88G73A_LED_STATE_PERIOD_MS = 500`, deadline **advanced by the period** rather than resampled — the blink is an instrument, and resampling would read up to 0.2 % slow |
| EV88G73A SW0 sampling | inside the same period count | `EV88G73A_SW0_SAMPLE_PERIOD_MS = 100`; deliberately not 1 ms, or release bounce would toggle the pause twice |
| EV88G73A status line | `EV88G73A_STATUS_PRINT_EVERY_N_STATES` (with an `#error` guarding its arithmetic) | `EV88G73A_STATUS_PRINT_PERIOD_MS = 2000`, resampled — a print may skip, it must not drift |
| `wm8904_audio` | `status_every_n_reports` / `idle_report_every_n_reports` | `status_period_ms` / `idle_report_period_ms`. The counts needed a paragraph explaining that the same number meant ~2 s on one board and a few screen repaints on the other; the milliseconds do not. |

The `#error` and that paragraph are worth naming as the tell: **a cadence that needs
arithmetic to be understood is measuring the wrong thing.** The counted version also carried
a real 4x error in its own comment ("~10 s" for what was 40 s), because it assumed a countdown
ticked per poll when it ticked per report.

**Two deliberate asymmetries, both recorded at their definitions:**

- **Which clock feeds Timer1 is a board decision.** EV88G73A asks from **Fcy**
  (`timer_app_start_from_fcy`): the tick is 1 ms only if the system clock is at its stated
  operating point, and that coupling is the *feature* — the LED cycle is a live check on the
  clock, and it is how the original half-speed-clock fault was caught. Reading Fcy back from
  the Clock HAL would self-correct and hide exactly that. DM330030 asks from the **FRC**
  (`timer_app_start_from_frc`), as `timer_1ms.c` did for itself, so its tick survives a wrong
  or fallen-back system clock.
- **IRQ priority 2, changed from `timer_1ms.c`'s 7, and it must stay below 4.** The TDM RX
  block ISR runs at `PRIO_TDM_DMA = 4` and is the one whose execution time is measured against
  the block period. A tick above it would preempt it, so the load and margin figures the audio
  work is steered by would include a millisecond interrupt at a rate that does not divide the
  block rate. Nothing was observed going wrong at 7 and nothing would have been: the cost is a
  measurement quietly losing meaning.

**`timer_1ms.c` keeps its registry and gives up the vector.** `TIMER_SetConfiguration()` no
longer starts anything — it checks that the tick *is* running and registers the per-millisecond
client walk through `timer_app_set_tick_hook()`. So the profile must start the tick **before**
calling it, which is stated at DM330030's call site. `TIMER_CONFIGURATION_OFF` detaches the
hook and deliberately **leaves the timer running**: it is the whole application's running time
now, and every `GetTicks()`-paced cadence in the tree would stop with it.

**Review follow-up: the exact-period contract was a claim, not a behaviour.** `timer_app.h`
says both entry points refuse a clock that cannot give 1.000 ms, and the tick HAL underneath
was not doing that — `calc_period_reg()` rounded, `(clk + denominator/2) / denominator`, never
checked the remainder, and returned OK. Both current boards divide exactly (100 MHz / 8 / 1000
= 12500, 8 MHz / 1 / 1000 = 8000), so **there was no runtime defect**; what was wrong is that
the new time base's stated contract was unenforced, and it is now the contract everything else
leans on — one approximate tick makes every millisecond in the tree wrong by the same unstated
factor, with a drift nobody can attribute. Fixed: exact divisors only, with a new
`DSPIC33CK_TICK_TIMER_ERR_INEXACT_PERIOD` kept distinct from `ERR_OUT_OF_RANGE` (inexact = fix
the clock; out-of-range = the period is exact but no prescaler brings the count inside 16 bits).
**Which of the two names to return took a second pass.** The first version flagged the *inexact*
divisors and returned `ERR_INEXACT_PERIOD` if it had seen any, which misreads the asymmetry
between the two walls. A remainder at one divisor implies a remainder at every later one
(1|8|64|256), so seeing one says nothing about the divisors already tried; an overflow says
nothing about later ones, which is the whole reason the loop continues. Concretely, at
65,537,000 Hz prescaler 1 divides exactly and gives 65,537 counts — one past PR1's 16 bits —
while 8, 64 and 256 all leave a remainder, so the first version answered `INEXACT_PERIOD` and
sent the caller looking for a clock that divides by 1000 when theirs already did. The flag now
records the *exact* case: some divisor divided exactly but none fitted → `OUT_OF_RANGE`, no
divisor divided exactly → `INEXACT_PERIOD`, which is what the header says each name means.
Neither board reaches either branch, so nothing was re-measured for this.

**Review follow-up: `GetTicks()` was not an atomic read, and the listing is what settled it.**
The review declined to call this a defect from the C alone — correctly, because XC-DSC *can*
move 32 bits in one `MOV.D`, and Microchip's own guidance is that whether it does depends on
the code generated, so check the listing. Checked, on the EV88G73A production build:
`mov.w 0x1052,w0` then `mov.w 0x1054,w1`. **Two instructions**, so the read tears. The window
is narrow but the consequence is not: if the Timer1 ISR lands between those two `mov.w`s on the
one millisecond in 65,536 where the low half carries, the caller pairs a new low half with an
old high half and reads a value ~65,536 ms away from the truth. Every cadence in the tree is
`(uint32_t)(GetTicks() - last) >= period`, so one such reading either fires immediately or holds
that cadence off for 65 s. Fixed by taking the snapshot with `_T1IE` masked — no tick is lost,
because masking IE leaves IF to latch and the pending interrupt runs on restore, and the
previous IE state is *saved* rather than forced on so the call stays safe from a context that
already masked it. Same technique the TDM transport already uses for its own 32-bit diag
counters. Cost: **+27 bytes** (45,711 → 45,738), and the re-disassembly confirms the shape —
`bfext` to save IE, `bclr.b 0x820,#1` to mask, the two `mov.w`s, then the restore. That restore
is itself a non-atomic read-modify-write of `IEC0`, which is the fleet's already-deferred
"IFS/IEC non-atomic RMW" item; nothing in this tree writes T1's IEC bit from an ISR, so it is
noted and not fixed here.

**Hardware evidence (EV88G73A, 2026-08-03).** The "codec not started" line, whose period is
40 s reached through the 2 s report gate, arrived at 39.757 s and 39.768 s — against 39.780 s
and 39.775 s from the **pre-change firmware measured minutes earlier on the same board**. The
cadence is unchanged, which is the regression check that matters: the LED period is derived
from the same tick, so the blink is unchanged too. (The ~0.6 % short reading is the FRC
tolerance the PLL multiplies up, i.e. the tick faithfully reports the real clock — which is
the whole reason EV88G73A counts Fcy.) A `*sr` warm reset restarts the tick cleanly: no
"tick FAILED" line, and the first report 2.497 s later.

**Post-fix hardware run, EV88G73A, two builds (2026-08-03).** The review asked for ~70 s with
the 5 s TDM line, the 40 s idle line, the LED and the console watched together. Those two lines
cannot appear in one build — `EV88G73A_ENABLE_TDM_LOOPBACK` and `EV88G73A_ENABLE_WM8904_AUDIO`
`#error` against each other, they contend for SPI1/RP48-51 — so it was run as two flashes, each
verified by build ID over the monitor rather than by `STATUS.TXT` (which the kit documents as
OS-cached).

*Build `revfix`, WM8904 path, 122 s.* Boot clean: `PLLstatus=0`, `LOCK@sw=1`, Fcy 100 MHz, DMA
selftest PASS. The idle line printed **immediately** at boot and then at 39.757 s and 79.502 s
(= 2 × 39.751) — the first-print-immediate behaviour intact, cadence intact. `?dt` mid-run
answered in 4 ms with `step min=43 max=72 backward=0 steps>=1024=0`.

*Build `revtdm`, TDM8/32-bit master loopback, 75 s.* **16 consecutive status lines** at
4.966-4.977 s, `run=1 miss=0 load(us10) last=454 max=454` on every one — no missed block in
~115,000, and the block-ISR load unchanged at 45.4 µs, which is the number the tick was placed
at priority 2 to protect. `?dt` answered in 10 ms *while that ISR was running*, which is the
priority split doing its job.

**What the −0.64 % does and does not establish.** 39.751 s against a nominal 40,000 ms and
4.968 s against 5,000 ms are the *same* scale factor through two different code paths and two
different period constants. That is **consistent with the real Fcy running about 0.64 % fast
rather than with a regression in either cadence path**, and the exact divider arithmetic
(100 MHz / 8 / 1000 = 12500, no remainder) and the matching pre-change measurement of 39.780 s
support that reading.

It is not a *proof* that the clock is the culprit, and an earlier draft of this paragraph
overclaimed by arguing "a tick bug would not produce one consistent ratio". It would: both
cadences read the same `GetTicks()`, so a tick that is uniformly 0.64 % fast makes both lines
0.64 % early by exactly this ratio. The two paths agreeing rules out a bug in *one* cadence,
not a bug in the shared time base. Settling it would take an independent time reference against
something else Fcy derives — BCLK, or the LED period on a scope. Nothing here depends on that,
so it was not done.

**Not verified on hardware:** DM330030's half (no board), the `wm8904_audio` status line (no
codec attached here), and SW0's pause, which needs a finger on the button. The LED was not
visually confirmed in this pass either — it is driven from the same tick as the lines above,
so those stand in for it.

**Defect 7's microsecond cost — deferred until the hot path is actually tight.** The
`≈7.7 µs / ≈1.2 %` figure is arithmetic from measured instruction counts, not a hardware
timing; `docs/ck_silicon_findings.md` says so and gives the recipe. Measuring it for real
means reading `inst_get_status()`'s `load` with and without the conversion in a build that
exercises the gain path. Revive it if the block-ISR margin ever becomes a close call; there
is no decision riding on it today.

### DONE 2026-08-03: the ANSEL sweep is deleted, `ANSEL` is owned per pin

**Shared ANSEL sweep — history of the question.** The rationale that previously rejected a
shared bulk `ANSELx` clear died with `be6d5fe` (`--gc-sections`), which reopened it. Material
gathered for that re-judgement, kept because it is what the decision below rests on:

| | |
|---|---|
| what exists | `dm330030_ports_digital_default()` — 5 ports × 16 bits = **80** `set_analog(pin,false)` calls at bring-up. EV88G73A has no equivalent. |
| the old argument | "EV88G73A would pay flash for something it does not need." MC105 has 64 KB and the WM8904 build did overflow it once, so this was reasonable at the time. |
| why it died | `be6d5fe` enabled `--gc-sections` in both configurations: an unreferenced function is dropped at link time, so a shared helper EV88G73A never calls costs EV88G73A nothing. The premise is simply no longer true. |
| the other direction | **26 of those 80 writes land on bits that exist; 54 do not.** Counted 2026-08-03 from the DFP headers (`ANSELx<n> : 1;` bit-fields), so this needs no board. |

**Which `ANSEL` bits are real (DFP headers, 2026-08-03).** The first step of the
re-judgement, done:

| port | MP508 / DM330030 | MC105 / EV88G73A |
|---|---|---|
| `ANSELA` | 5 — bits 0–4 | 5 — bits 0–4 |
| `ANSELB` | 8 — bits 0–4, 7–9 | 8 — bits 0–4, 7–9 |
| `ANSELC` | 6 — bits 0–3, 6, 7 | 6 — bits 0–3, 6, 7 |
| `ANSELD` | 3 — bits 10, 11, 13 | 2 — bits 10, 13 |
| `ANSELE` | 4 — bits 0–3 | **register absent** |
| total | **26** (of 80 attempted) | 21 |

Three things fall out of the table, and one of them **corrected comments in four places**.

**"PortC/D carry no `ANSEL` bits" does not hold on CK** — that was the fleet's
`ansel-bulk-clear-removal-study` finding on the part *it* examined; here C has six and D has
two or three.

**RC8/RC9 (the I2C1 alternate pair) have no `ANSEL` bit on EITHER part.** `dm330030_pins.h`,
`dm330030_board.c` (twice) and `ev88g73a_board.c` all said the opposite — that the pair *is*
analog-capable on MP508 "unlike MC105", making the forced-digital call load-bearing there. It
is not: MP508 implements `ANSELC` 0–3, 6 and 7, exactly like MC105. All four sites are
corrected. Both boards keep the call, for the reason now stated at each: `ANSEL` is that
stage's to own, and the retarget pair RB8/RB9 does have real bits.

**RD10 and RD13 — EV88G73A's LED0 and SW0 — ARE analog-capable on MC105.** `ANSELD` there
implements exactly bits 10 and 13, so both pins come out of reset **analog** and the shared
`gpio_table` configs' `.analog = false` is load-bearing on the board that has no sweep.
`ev88g73a_board.c` claimed "none of these pins is analog-capable on MC105"; corrected. This is
the pairing worth noticing: the part with the sweep needed it least, and the part without one
had the two pins that actually depend on per-pin ownership.

Note the HAL already gates on register presence, not bit presence:
`nora_gpio_dspic33ck.c`'s port table is built under `#if defined(ANSELx) && …`, which is why
MC105's missing `ANSELE` costs nothing. The 54 wasted writes are all *inside* registers that
do exist, and writing an unimplemented bit position there is a harmless no-op.

**DONE 2026-08-03 — aligned with sonora: the boot-time bulk clear is DELETED and `ANSEL` is
owned per pin.**

The three options that had been drawn up here (share / shrink / leave) were all answering the
wrong question, because they took the sweep's existence for granted. **The fleet already
removed it**, and that decides it — sharing a stage sonora has deleted would mean building
the thing a later alignment pass has to tear out. In `dspic33ak-audio-dsp-sonora` `main`:

| commit | |
|---|---|
| `34f6080` | **Remove boot-time `ANSELx=0` bulk clear; own `ANSEL` per-pin** |
| `75d860a` | review follow-up (corrected the ANSEL output audit) |
| `1f72f44` | **AK128 hardware smoke PASS** |
| `fe46e0e` | cleaned up comments that still referenced the removed clear |

`src/main.c` there now says each pin sets its own analog/digital mode, and
`src/board/clock/sonora_clock.c` explicitly notes that the REFI1 pin "no longer leans on the
boot-time blind `ANSELx=0` clear" — the phrase *blind* being the point: a bulk clear
configures pins nobody has thought about, and hides the fact that a pin's owner never stated
its mode.

**CK was already shaped for it**, which is why this was a mechanical change rather than a
redesign:

- `nora_gpio_table_dspic33ck.c`'s shared configs each state `.analog` explicitly — `false` for
  `cfg_output_low/high` and `cfg_input_pullup`, `true` for `cfg_analog_input`. Every pin
  configured through a board's `gpio_table` therefore already owns its mode.
- Both boards' I2C1 pin stages already call `nora_gpio_rp_set_analog(..., false)`
  explicitly rather than relying on the sweep.
- **The ordering dependency is gone.** "The pot entry must stay last in
  `dm330030_user_io_pins[]`" existed *only* because the sweep ran first and would undo it.
  With the sweep deleted the table's entries are independent — one less load-bearing
  invariant a future edit could quietly break. The warnings in `dm330030_pot.h`,
  `dm330030_board.h` and on `dm330030_user_io_pins[]` went with it, and
  `dspic33ck_gpio_table_apply()`'s doc-comment was **inverted**: it used to open with "ORDER
  IS PART OF THE DATA" and now says "ENTRIES ARE INDEPENDENT", with the note that a future
  need for ordering means the pin descriptions are incomplete.
- 80 writes at bring-up became 0, of which 54 were no-ops anyway (see the table above). The
  saving is not the point — the point is that no pin's mode is set by something that did not
  know the pin existed.

**The work was the audit, not the deletion**, exactly as expected — and it came out clean.
Every pin configured outside a `gpio_table` already owned its `ANSEL`, because
`nora_gpio_rp_config_digital_output()`/`_input()` set `.analog = false` themselves: UART1
TX (and EV88G73A's RX), all four TDM pins (`dspic33ck_spi_i2s_tdm_pins.c`), DM330030's REFO1
MCLK pin, and both boards' I2C1 pair (which calls `rp_set_analog(false)` directly). Every
`gpio_table` config states `.analog` explicitly. Cross-checked against the MP508 census above,
the only analog-capable pin on DM330030's list is RE3, the pot — which *wants* to be analog.
`hal_gpio/nora_gpio_dspic33ck.c`'s one raw `ANSEL` access is its port table. So no pin was left
relying on the sweep, and nothing had to be added to replace it.

**What actually changes at runtime:** unused analog-capable pins now stay at their POR analog
state instead of being forced digital by something that had never heard of them. That is the
intended difference, and it is recorded in the ~35-line note left in `dm330030_board.c` where
the sweep used to be — the deleted code's shape, the audit, the bit census and the runtime
consequence, so the deletion is readable as a decision rather than as an absence.

**Verification:** EV88G73A is hardware-PASS (boot banner, DMA selftest, console, `*sr` warm
reset, and the timed cadences above, all on the real board with no sweep-related change in
behaviour). **DM330030 is compile-only — no board exists here — so the half that deleted the
sweep is unverified on hardware.** Both configurations build clean, and so do the
`ENA_TDM_MASTER_LOOPBACK=1 ENA_WM8904_AUDIO=1` and `EV88G73A_ENABLE_WM8904_AUDIO=0` variants.

### Blocked on hardware that does not exist here

**I2C1 pin-stage differences between the two boards.** The `set_analog` **call form** is now
converged (both boards use the RP form — see the renaming section above). What remains is
**pull-ups**: EV88G73A enables the internal ones because nothing else holds the bus high with
no codec attached; DM330030 does not. That is an electrical choice, not code tidiness — do not
converge it without a board.

**`timeout_ms` is not one of those differences, and the reason changed.** It reads 10 on
EV88G73A and 0 on DM330030, but **neither board wires `get_ms`**, and the HAL disables timeout
handling entirely without a time source — so EV88G73A's 10 is inert and the two boards behave
identically. What bounds a stuck bus on both is the HAL's pending/bus-idle guards. Until
2026-08-03 the honest reason was "there is no running time to hand it"; there is one now
(`app/timer_app.h`'s `GetTicks()`, started in both profiles' bring-up), so this became a
*choice* rather than a limitation. Left inert deliberately: turning a dead timeout live changes
I2C failure behaviour on a path whose only hardware evidence is EV88G73A's expected-negative
probe, so it wants its own hardware pass. Noted at both call sites.

**`DM330030_AUDIO_RP_MCLK` (RP69/RD5) is a placeholder.** Also noted at the end of the
I2C section above. No DM330030 board exists, so nothing on its audio path has run.
**Superseded 2026-08-04:** this is not a pin waiting to be confirmed — the stage behind it
should not exist at all (same WM8904 board, same crystal, and 12.5 MHz is the wrong figure
anyway). See the MCLK section near the end.

### Closed 2026-08-03

**LED0 blink and SW0 press on EV88G73A — CLOSED, hardware PASS.** Three press/release cycles
on the real board, LED confirmed by eye, and `?dt` answering while paused proved the loop was
still running. Details in the section above. Nothing in this profile is now without hardware
evidence.

### Carried over 2026-08-04 (from the ROM/TDM-sine handover note, deleted with this edit)

Same rule as the 2026-08-03 batch above: these outlive the session that found them, so they
live here instead of in a note that describes a moment.

- **`board.c` differences vs. role orthogonality — still unresolved.** The standing objection
  is "`board.c` に差があるのはやはりリファクタリングが足りない". Three of those differences
  were closed on 2026-08-03/04 (UART1 init, the ANSEL sweep, the MCLK stage). What remains
  wants a decision per difference: **genuine hardware fact, or leftover duplication?** The
  I2C pull-ups above are the clearest "hardware fact" case. Do this against the master/slave
  role axis explicitly — role decides who drives BCLK/FS, and it is orthogonal to every
  clock-source question, which is the confusion that produced the deleted MCLK stage.
- **Optional, cosmetic:** the measurement tables in this file label their rows by build
  variant; "configuration × `-Define` variant" would say what they actually are.
- **ROM reserve, unspent and not needed at 46 %:** a project-local integer formatter behind
  `console_out.h` (≈ 3-4 KB), then banner/help trimming (≈ up to 2 KB). Both are real code
  changes with real test cost.
- **Identified, not waste:** `dspic33ck_clock.c` pulls `divmoddi3`/`muldi3`/`umuldi3`
  (≈ 765 B) for 64-bit frequency arithmetic. That is overflow protection in the PLL maths;
  recorded so it is not rediscovered as a find.

## Phase 1 of the parity port: one configuration vocabulary (2026-08-05)

Plan, classification and the specification being ported to:
[`dm330030_parity_and_design.md`](dm330030_parity_and_design.md) (parts 1 and 2).
This section is what was **built**, and what it was measured to cost.

### The defect this closes

The same three SHARED `app/` modules were gated by two different names each:

| shared module | the profile's switch | the module's own switch |
|---|---|---|
| `app/wm8904_audio.c` | `EV88G73A_ENABLE_WM8904_AUDIO` (EV `main.c`) | `ENA_WM8904_AUDIO` (`app_config.h`) |
| `app/demo_tdm_master_loopback.c` | `EV88G73A_ENABLE_TDM_LOOPBACK` | `ENA_TDM_MASTER_LOOPBACK` |
| `app/i2c_probe.c` | `EV88G73A_ENABLE_I2C1_PROBE` | `ENA_I2C_WM8904_PROBE` |

A shared module is its own translation unit and reads `app_config.h` directly, so a
`#define` in a board's `main.c` is invisible to it: a TDM build had to pass **both**
spellings, and EV's `main.c` carried an `#error` asserting the pair because half-right
produced an empty object and a link error. **The two-name shape was the defect**; the
`#error` was a symptom being managed.

### The shape now

- **`src/boards/<board>/board_profile.h`** — one per board, each answering the same
  questions under one set of names. The filename carries no board prefix on purpose: it is
  a seam name like `uart_platform/console_out.h`, and each configuration puts only its own board
  directory on the include path (`extra-include-directories`), so `#include
  "board_profile.h"` resolves to the selected board's copy. Shared code reads one name and
  never learns a board's. Each copy `#error`s if the matching `DSPIC33CK_BOARD_*` is absent,
  so an include-path/macro mismatch cannot hand one board's profile to another's code.
- **`app/app_config.h`** stopped defining switches and became the **checker**: it pulls in
  the profile, refuses anything unresolved (undefined would otherwise evaluate to 0 in
  `#if` — the silent failure being designed out), derives `DEMO_ENABLE_DMA_SELFTEST` and
  refuses a hand-written one, enforces the pin-exclusivity pairs, and refuses each
  **retired name** by name so a stale `-DENA_TDM_MASTER_LOOPBACK=1` in a script fails
  loudly instead of doing nothing.
- **The names**: `DEMO_ENABLE_WM8904_AUDIO`, `DEMO_ENABLE_TDM_MASTER_LOOPBACK`,
  `DEMO_ENABLE_I2C_PROBE`, `DEMO_ENABLE_CONSOLE_COMMANDS`, `BOARD_CONSOLE_HAS_RX`,
  `BOARD_CONSOLE_USE_RX_ISR`, plus derived `DEMO_ENABLE_DMA_SELFTEST`.
- Both `main.c` files now only **read** switches. The prose that explained each choice
  stayed next to the code it selects; the defaults moved to the profile.

### Three more things in the same pass, each an item from the accident list

1. **DM330030's console RX pin exists and is now routed.** `RP67/RD3`, from the MCP2221A
   USB-UART — a **separate device from the PKOB4** programmer, which the console does not
   pass through (DS50002859A p.14/p.19). The inherited demo only ever printed, so only TX
   was recorded and the absence read as a hardware unknown. `uart_platform_stdio_init()`
   already enabled the receiver on both boards, so the peripheral was listening to an
   unrouted pad. Routing is unconditional, like TX: the pin is a board fact, and
   `BOARD_CONSOLE_HAS_RX` is what the app layer consults.
2. **`console_out_idle()` on DM330030 no longer lies.** It returned `true` unconditionally
   (argued as safe: nothing called it, and reporting idle loses at most a line where
   returning false forever hangs). It now delegates to
   `nora_uart_tx_done(UART_PLATFORM_CONSOLE_UART_INST)`, the same question EV asks.
   `printf` stays as the formatter — `uart_platform_stdio.c`'s `write()` hook is already on
   `hal_uart`, so there is no second transport — while the **wait** goes to the HAL, which
   is the one thing `stdio` cannot answer. The callers are the reset acknowledgement and
   the trap report, both about to destroy the machine that would finish the transmission.
3. **RX vectors follow capability, not board name.** `hal_uart/nora_uart_dspic33ck_isr.c` was
   excluded in the EV configuration (which *has* console input, polled) and linked in the
   DM one (which had no RX pin routed) — exactly backwards. Both exclude it now, both poll,
   and `app_config.h` refuses `BOARD_CONSOLE_USE_RX_ISR=1` until a configuration provides
   the vectors, so the capability cannot be claimed without the source list.

`DEMO_ENABLE_CONSOLE_COMMANDS` is 1 on EV and **0 on DM330030 — a porting gap, stated as a
0 rather than left as an absence of code.** That is the first Phase 2 item, and with the RX
pin routed nothing hardware-shaped blocks it.

### Gate A — measured, no hardware

| build | before | after | `-BuildId` of the "after" |
|---|---|---|---|
| EV88G73A default (codec live) | 40,254 B / 60 % | **40,242 B / 60 %** | `ph1` |
| EV88G73A TDM exerciser (sine) | 30,585 B / 46 % | **30,579 B / 46 %** | `t1tdm` |
| DM330030 (compile-only) | 19,872 B / 7 % | **19,569 B / 7 %** | `ph1` |

The build-ID column is not decoration: the string lands in `.const`, so two characters of
it move the image by about 6 B (`ev88g73a_rom_budget.md`). Both EV rows are within that, in
the direction the shorter ID predicts, i.e. **the rename is byte-neutral** — which is the
claim worth having, since the whole point was to change no behaviour. RAM: EV 4,708 B / 57 %,
DM 4,710 B / 19 %.

DM330030 drops 303 B, and that is the one behavioural change on that side: the RX vector
file left the link. It removes a vector nothing called.

**The TDM variant now takes one flag where it took two:**

    -Define DEMO_ENABLE_WM8904_AUDIO=0 DEMO_ENABLE_TDM_MASTER_LOOPBACK=1 DEMO_TDM_TX_MODE=2

Also audited: no old macro name survives anywhere in `src/` except in prose that explains
the history, and both configurations build warning-free.

**Two negative tests, because a guard nobody has seen fire is a guard nobody has tested:**

| test | result |
|---|---|
| `-Define DEMO_ENABLE_WM8904_AUDIO=1 DEMO_ENABLE_TDM_MASTER_LOOPBACK=1` (two owners of SPI1) | build stops at `app_config.h`: *"both drive SPI1 and its four TDM pins; enable only one"* |
| `-Define ENA_TDM_MASTER_LOOPBACK=1` (retired name, the stale-script case) | build stops at `app_config.h`: *"ENA_TDM_MASTER_LOOPBACK is retired -- use DEMO_ENABLE_TDM_MASTER_LOOPBACK"* |

The second is the one worth keeping: before this pass that flag was *meaningful*, so a
copied command line would have half-configured a build. Now it cannot be ignored.

### Gate B — NOT DONE, and this is a hold

Phase 1 edits shared code, so the EV board has to re-prove its own contract (banner, `?dt`,
`*sr`, the three forced traps, TDM `miss=0`) before any of this is trusted. **The board is
not attached** — no COM28, no Curiosity Nano mass-storage volume, and `serial_monitor`
reports `connected:false`. So the whole of Phase 1 is **build-verified only**, exactly the
state that let a never-run MCLK stage survive for months. Do not treat it as done, and do
not start Phase 2 on top of it, until the EV regression has run.

### Branch state

`feature/ck64mc105-ev88g73a-profile` was fast-forwarded into `main` at `f085575`
(2026-08-03, 25 commits) and again at `3a8243e` (2026-08-04, 9 commits: the TDM
sine/ramp modes, `isolate-each-function` in both configurations — 72 % → 46 % ROM —
the MCLK-stage deletion, and this review's documentation fixes). Both on explicit user
instruction. Development continues on the branch;
`main` is only advanced at a publish point, and **merging still requires explicit user
confirmation** — a pasted review saying "you may merge" is not that confirmation.

## `board_seam.h` merged into `app_traps.h` (2026-08-06)

`app/board_seam.h` was a header holding **four declarations and no code**, created in the
2026-08-02/03 passes that moved the console's `s` and `x` modules out of
`boards/ev88g73a/`. It is now the **BOARD SEAM** section at the bottom of
`app/app_traps.h`; the file is deleted and its `<itemPath>` removed from
`configurations.xml`.

### Why it was worth merging

The complaint was that the file read as bolted on, and the measurement agrees: **all four
functions serve one subject**, which is the subject of `app_traps.h` — reporting and
provoking traps and resets.

| function | who calls it |
|---|---|
| `board_reset_cause_raw()` / `_str()` | `app/app_traps.c` (the boot trap report), `uart_app/system_console.c` (`*sr`/`?sr`), EV's banner |
| `board_trap_bad_addr()` / `board_trap_stack_beyond_limit()` | `uart_app/traps_console.c` only (`*xa`, `*xs`) |

**Three of the six includers already included `app_traps.h`** (`app_traps.c`,
`traps_console.c`, EV `main.c`), so for them the separate header was a second `#include`
saying the same thing. The board files now include `app_traps.h` to implement their half,
and `system_console.c` gains it.

### What did NOT change

The seam itself. Still **four functions, not macros** — the reason is unchanged and is
restated where they are declared: macros would put `#if defined(DSPIC33CK_BOARD_...)` into
shared files, so every new board would edit `app/`, whereas a new board adds one file of
its own and nothing above the seam changes. The two implementations, the latch-vs-live
RCON difference, and the `dspic33ck_reset_cause_str(latched, is_latched)` split are all
untouched.

`board_*` remains **reserved for the seam**; only the header that declares it moved.

### Options rejected, and why

| option | why not |
|---|---|
| declare the four in each `boards/<b>/board_profile.h` | zero new `#include`s (it already arrives through `app_config.h`), but the contract would be written once per board — a seam whose whole value is being stated in one place |
| reset pair → `hal_reset/dspic33ck_reset.h`, trap pair → `app_traps.h` | puts `board_*` names a **board** implements into a device-only HAL, and splits one seam across two homes |
| keep the file, shorten the comments | the file count was the complaint; the comments are the part worth keeping |

### Verification

Both configurations were regenerated and built (`-Full` is required: `configurations.xml`
changed, so the generated makefiles had to be rebuilt). **Both compile and LINK:**
CK64MC105_EV88G73A at 77 % program / 57 % data, CK256MP508_DM330030 at 10 % / 19 %.

**All four seam functions were then exercised on the EV88G73A board**, since between them
they are the whole change: three of the four are only reachable from the console, and the
fourth prints in the banner. Audio was stopped with `*ts` first (see
`buildtools/README.md` -- the programmer's console traffic is otherwise received as TDM
data), and every reset below is one the commands provoke on purpose.

| function | how it was reached | what the board said |
|---|---|---|
| `board_reset_cause_str()` | boot banner | `Reset    = EXTR(MCLR)` |
| `board_reset_cause_raw()` | `?sr` | `reset cause this boot: EXTR(MCLR)  RCON=0x0083` |
| both, after a software reset | `*sr` | `Reset    = SWR(software reset)` |
| `board_trap_bad_addr()` | `*xa` | `*** TRAP on the previous run: ADDRESS ERROR (unimplemented address)`, `traps since power-on: 1` |
| `board_trap_stack_beyond_limit()` | `*xs` | `Reset    = TRAPR(trap conflict)`, and `(No handler ran, so the trap counter did not advance for this one.)` |

That last row is the measured fact this file has claimed all along, now reproduced: a stack
overflow cannot be reported in software, because the handler's own push overflows too. The
hardware reset via `RCON.TRAPR` is the only evidence, and the counter -- which a handler
increments -- correctly does not move.

Two limits, stated rather than implied. The flashed image (`seam1`) was these two commits on
top of `2dc06c2`; the branch was afterwards rebased onto `52a9c06` and both configurations
rebuilt from there, but that rebased image was not re-flashed -- the intervening commits
(the 8x-unrolled hot loops, the gain ramp) do not touch anything here. And DM330030 remains
compile-only, as everywhere else in this file.

No hardware behaviour is touched by the change itself: it moves declarations, and no code.
