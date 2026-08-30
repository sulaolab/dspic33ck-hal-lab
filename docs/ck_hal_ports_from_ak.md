# AK → CK HAL ports: the silicon deltas

Consolidated 2026-08-03 from `ck_clock_hal_port.md`, `ck_dma_hal_port.md`,
`ck_i2c_master_port.md`, `ck_gpio_pps_ak_parity.md`, `ck_gpio_event_ak_parity.md`,
`ck_spi_i2s_tdm_stage1.md`, `ck_spi_i2s_tdm_stage2.md`,
`ck_spi_i2s_tdm_stage3_prep.md`, `ck_maxspeed_clock_and_app_layer.md` and
`ck64mc105_ev88g73a_profile.md`. Those ten files are gone.

Each of them was a per-milestone record: what landed, which files, "builds clean under
`-Wall`, links to hex", what was deferred to the next stage. **All those stages are done,
their file lists are the tree, and their build claims are `git log`.** What could not be
recovered from the code is the AK→CK **silicon mapping** — the table you need to not
re-derive a register split from two data sheets — and the **caveats that outlived their
stage**. That is what is below.

Design intent for these HALs is deliberately *not* here; it lives beside the code in
`ck_src_layout.md`. Findings from running them on hardware are in `ck_silicon_findings.md`
— including **five defects in the DMA/SPI port whose wrong values are contradicted by the
tables below**, so read that document before trusting any value here that its §"Part 1"
touches.

The governing rule for every port: **same public API and cfg-struct field names as AK, so
consumers move across unchanged; register layer rewritten.** Where the silicon collapses
two AK concepts into one, the collapse is documented in the struct rather than the API
being narrowed.

---

## Clock — a different clock tree, not a different register set

AK targets the dsPIC33A tree: per-PLL `PLL1CON`/`PLL2CON` plus `CLKGEN1..12`
(`CLKxCON`/`CLKxDIV`) and a VCO fracdiv. **CK is the classic single-clock tree**: one
system PLL through `OSCCON` + `CLKDIV` + `PLLFBD` + `PLLDIV`, switched by the global
`OSCCON.OSWEN` and confirmed by `OSCCON.LOCK`. No CLKGEN, no PLL1/PLL2 split. What was
reused from AK is the `solve_pll()` integer-divider search and the timeout-guarded
`WAIT_CLEAR`/`WAIT_SET` switch macros.

Register facts (DFP 1.15.423, verified against the DFP structs):

- `OSCCON`: OSWEN(0), CF(3), LOCK(5), CLKLOCK(7), NOSC(8:10), COSC(12:14)
- `CLKDIV`: PLLPRE(0:5) · `PLLDIV`: POST2DIV(0:2), POST1DIV(4:6), VCODIV(8:9) ·
  `PLLFBD`: PLLFBDIV(0:11)
- NOSC encoding: FRC=0, FRCPLL=1, PRI=2, PRIPLL=3, LPRC=5, BFRC=6, FRCDIVN=7
- `Fvco = (Fin/PLLPRE) · PLLFBDIV`, `Fosc = Fvco/(POST1DIV · POST2DIV)`, **`Fcy = Fosc/2`**

> The last relation was the bug. The port had `Fosc = FPLLO`, missing a fixed divide-by-2,
> and ran the part at half speed while `LOCK` read 1. See `ck_silicon_findings.md` Part 2 —
> that is also where `PLLKEN = OFF`, the highest-Fvco `solve_pll()` choice, and DM330030's
> `FCKSM = CSDCMD` silent no-op are recorded.

**One authoritative Fcy.** The operating point used to be assumed independently in three
places — the timer (`TIMER_1MS_CLOCK_HZ 8000000`), the UART (`UART1_DEMO_CLOCK_HZ 4000000`)
and the I2C probe (`I2C_PROBE_FCY_HZ`, **wrongly 100 MHz**) — and `CLOCK_Initialize()` was
defined but never called, so the clock ran on the config-bit default. Now everything reads
`dspic33ck_clock_get_fcy_hz()`, which is why raising Fosc to 200 MHz later needed no baud
arithmetic anywhere.

**Two numerically-equal sources of truth are intentional.** `DSPIC33CK_CLOCK_FRC_HZ` (a
device fact: FRC is a fixed 8 MHz) versus the board's primary-oscillator Fosc fed into the
HAL. **Timer1 is clocked from the FRC, not from Fcy**, so it must not follow
`get_fcy_hz()` — they diverge the moment the PLL raises Fosc while the FRC stays 8 MHz.
Ordering matters and holds: the clock stage runs before the UART, so the recorded Fcy is
valid when the UART reads it.

**100 MIPS and a 230400 console.** Default target is Fosc 200 MHz / Fcy 100 MHz, the
CK256MP508 maximum, from the **internal FRC** (crystal-independent — deliberate, since the
DM330030 crystal frequency is unconfirmed). `pll_configure(FRC, 8 MHz, 200 MHz)` solves
PLLPRE 1 / PLLFBDIV 100 / POST 4×1. Overridable to `8000000` for FRC-direct.

**The console-rate predicate, and why the obvious one was wrong.** The rate first followed
*"did we reach the operating point we requested"* — so asking for FRC-direct **succeeded**,
set that flag true, and brought the console up at 230400 on a 4 MHz Fcy. **The documented
low-risk option was the one that produced a dead terminal.** The predicate is now *"can
the achieved Fcy represent 230400"*, derived from the HAL's own BRG rounding
(`BRG+1 = round(Fcy/(4·baud))`, error ≤ `0.5/(BRG+1)`, so ≤2% needs `BRG+1 ≥ 25`): 100 MHz
gives 108 and is used, 4 MHz gives 4 and falls back to 9600 (within 0.2% at both rates).

**The same cause, one layer over:** `dm330030_mclk_init()` used a literal `RODIV = 16`,
correct only at Fosc 200 MHz, and **returned true on the FRC while emitting 500 kHz instead
of 12.5 MHz**. The divider is now derived from the achieved Fosc and must be exact, so
FRC-direct plus WM8904 audio fails with "MCLK init failed" rather than the misleading
"codec init failed". Both of these are the same shape: *a status that reports the request
rather than the achievement.*

---

## DMA — 4 channels, 16-bit, and everything in `DMAINTn`

CK has **4 channels** (`DMACH`/`SRC`/`DST`/`CNT`/`INT` 0-3), all 16-bit, IRQs `_DMA0..3`.

| Concern | AK | CK |
|---|---|---|
| Channels / width | 0-7, 32-bit | **0-3, 16-bit** |
| Per-channel config | `DMAxCH` | `DMACHn` — CHEN0 / SIZE1 / TRMODE2-3 / DAMODE4-5 / SAMODE6-7 / RELOAD9 |
| Trigger + status + HALFEN | split across `DMAxSEL` / `DMAxCH` / `DMAxSTAT` | **all in `DMAINTn`** — CHSEL[14:8], HALFIF4, DONEIF5, HALFEN0 |
| Count | `DMAxCNT` = count | `DMACNTn` = **count** (see below) |
| Reload | RELOADS / RELOADD / RELOADC | **single `RELOAD`** |
| Done-int enable | `DONEEN` | **none** — DONE is gated by `_DMAnIE` and cannot be masked separately from HALF |
| Element size | byte / half / word32 | **1 bit**: 16-bit word or byte. **No 32-bit element exists.** |
| Global enable | `DMACON.ON` + a DMALOW/HIGH window | `DMACON.DMAEN` + `DMAL`/`DMAH` (see below) |

HALFIF/DONEIF sit at bits 4/5 — the same positions as AK's `DMAxSTAT` — so
`half_from_status()` and the `STAT_HALF`/`STAT_DONE` masks port unchanged.

**Two entries in that table were wrong in the original port and are corrected here**
(details, evidence and datasheet citations in `ck_silicon_findings.md` Part 1):

- `DMACNTn` **is the count**, not `count − 1`. The port wrote `count − 1` and every
  transfer was silently one element short.
- **`DMAL`/`DMAH` must be programmed.** The port asserted CK "has no DMALOW/DMAHIGH window
  to program (the reset address window already covers RAM)" — both halves false. `DMAH`
  resets to `0x0000`, which places *every* RAM address out of bounds.
- Also: `DMAINTn.CHSEL` is **not** the peripheral IRQ number. It is a dedicated trigger
  encoding (SPI1 RX/TX = **0x02/0x03**, not 9/10). The field that carried it used to be
  `cfg.trigger_sel` (a raw `uint8_t`); since 2026-08-10 the contract takes a logical
  `nora_dma_trigger_t` and the whole transcription — including the refusal for a trigger
  this silicon does not have — lives in `dma_trigger_to_chsel()` in the DMA backend.

**The one CK detail with no AK counterpart, and it is a trap.** Because trigger, HALFEN and
status all share `DMAINTn`, `clear_status()` and `isr_snapshot()` must clear **only** the
flag bits (`…_INT_STATUS_MASK`). Zeroing the whole register loses CHSEL and HALFEN
mid-stream. AK could zero its separate `DMAxSTAT` safely; here the same idiom kills the
channel.

API collapses, documented in `dspic33ck_dma_channel_cfg_t`: `reload_count`/`reload_src`/
`reload_dst` OR-collapse into the single CK `RELOAD`; `done_int_en` is accepted but writes
no bit; `size` maps HALFWORD→16-bit word, BYTE→byte, and **rejects WORD (32-bit)**.

Scope is low-level DMA only, matching AK: no ping-pong or streaming policy, no
scatter-gather, no callback framework — the consumer owns the `_DMAnInterrupt` vectors.

---

## I2C master — the classic 16-bit module, and the `STAT2` that does not exist

AK targets the dsPIC33A "new" 32-bit I2C; CK is the classic 16-bit module. Verified against
`dsPIC33CK-MP_DFP 1.15.423`.

| Concern | AK | CK |
|---|---|---|
| Instances | I2C1-4 | **I2C1-3** |
| Control | `I2CxCON1`/`CON2` (32-bit) | **`I2CxCONL`/`CONH`** (16-bit) |
| Status | `I2CxSTAT1`/`STAT2` | **`I2CxSTAT`** (single) |
| Baud | `I2CxLBRG`/`HBRG` | **`I2CxBRG`** (single) |
| Bus-idle timeout | `CON2.BITE` + `BITO` | **none** |
| Interrupt routing | `INTC` + event/RX/TX | `MI2Cx` / `SI2Cx` / `I2CxBC` |

The bits the master engine actually uses line up bit-for-bit between AK `CON1`/`STAT1` and
CK `CONL`/`STAT`, so the blocking API and byte state machine ported almost verbatim. **The
substantive work was reformulating everything that depended on AK-only `STAT2`** — and
that is the part worth keeping, because each line is a decision about how to detect the
same event from different bits:

| Event | AK (`STAT2`) | CK |
|---|---|---|
| START done | `STARTE \|\| S` | `!CONL.SEN` (hardware auto-clears) |
| RESTART done | `STARTE` | `!CONL.RSEN` |
| STOP done | `STOPE && !PEN` | `!CONL.PEN` — keeps AK's intent of waiting for the request bit to retire, which is what fixed a 100 kHz overlap there |
| NACK | `NACKE` | `STAT.ACKSTAT` (1 = NACK) |
| error | `ERR` | `STAT.BCL \| IWCOL \| I2COV` |
| host active | `HSTACT` | **no equivalent** |

`HSTACT` having no CK counterpart removed two waits from `write_byte_blocking` (the
`wait_until(host_active)` step and the `D_A` "data accepted" wait). **CK master transmit is
fully described by TBF (pre-write) → TRSTAT (complete) → ACKSTAT (ACK)**, so nothing was
lost; `deinit`/`set_bus_speed` idle checks use `STAT.TRSTAT` plus the `CONL` request bits.

Baud, CK single-register form: `I2CxBRG = (Fcy/Fscl − Fcy·TDELAY) − 1` with
`TDELAY = 130 ns`, clamped to ≥ 1, in 64-bit integer math. **`TDELAY = 130 ns` is
unvalidated** — and AK measured 100 kHz failing while 400 kHz worked on its own hardware
(memory `i2c-runtime-bus-speed-status`), which flags low-speed BRG accuracy as a real
sensitivity. Verify on a scope before trusting 100 kHz.

Bit positions, verified against the DFP bitfield structs:

- `CONL`: SEN 0, RSEN 1, PEN 2, RCEN 3, ACKEN 4, ACKDT 5, STREN 6, GCEN 7, A10M 10,
  STRICT 11, SCLREL 12, **I2CEN 15** (set last in `init`)
- `CONH`: DHEN 0, AHEN 1, SBCDE 2, SDAHT 3, BOEN 4, SCIE 5, PCIE 6
- `STAT`: TBF 0, RBF 1, R_W 2, S 3, P 4, D_A 5, I2COV 6, IWCOL 7, ADD10 8, GCSTAT 9,
  BCL 10, ACKTIM 13, TRSTAT 14, ACKSTAT 15

Master role only; slave and the interrupt-helper layer are deferred, matching AK's own
master-first history. See `ck_src_layout.md` for the pin/fuse decision (**EV88G73A uses the
ALTERNATE pair via the `ALTI2C1` fuse, because the default RP40/41 double as PGC1/PGD1**);
the older note here that `ALTI2C1 = OFF` and mikroBUS-A pins needed confirming applies to
DM330030 and is still open there.

---

## PPS — one deliberate difference from AK, and it was a silent failure

The prior CK `pps.c` had scaffolding that diverged from AK and carried a real defect.

**Wrong unlock.** It wrote `RPCONbits.IOLOCK = 0/1` directly, copied from AK — but **AK
does that precisely because its PAC leaves RPCON writable and `__builtin_write_RPCON()` is
unsupported on the dsPIC33A target.** The classic CK block is the opposite: IOLOCK only
changes through the `__builtin_write_RPCON()` unlock sequence, so direct writes are ignored
and **PPS routing would silently fail on hardware.** Now `__builtin_write_RPCON(0x0000)`
to unlock, `(0x0800)` to lock (IOLOCK = bit 11). The builtin was verified to compile on the
CK XC-DSC target. This is the one intentional AK↔CK divergence in the file, documented at
both the header and the implementation.

**Computed pointers replaced by `#ifdef` switches.** Output routing used `&RPOR0 + index`
with a 6-bit shift, which does not look like AK and is fragile in the virtual-RP region.
Now `get_output_code()` (`#ifdef _RPOUT_*`) plus `write_output_rp()` (`#ifdef _RP<nn>R`,
RP32..RP79) — the same idiom as AK's RP1..RP128 switch. On CK only ports B/C/D define
`_RPnnR`, so exactly the 48 physical remappable pins compile in and A/E fall out
automatically, with no part-number conditionals.

> **The same computed-pointer mistake recurred independently** in `fs_clc.c`
> (`(&RPOR0) + (rp-32)/2`, a wild SFR write on both parts). See `ck_silicon_findings.md`
> Part 1. That is why the RPORx slot→RPn map is now an explicit table in
> `dspic33ck_pps.c`: this register bank is **not** contiguous on CK64MC105
> (`RPOR15/16` = `{RP65,RP72},{RP74,RP77}`), so any formula over it is wrong on that part.

CK RP facts: flat map `RPn = 16·(port+1) + bit`, ports B=1/C=2/D=3 only → **RP32..RP79**,
48 physical pins, RB0 = RP32, RD15 = RP79. All 48 `_RP<nn>R` registers exist.

**Virtual RPV0-5 (RP176-181) are not routable through the GPIO-typed API** — matching AK's
stance on its own RPV range. They *are* used internally by `fs_clc` (RPV0 carries the
FRMSYNC marker to the CLC input with no pad), which is why the exclusion is at the
GPIO-typed API rather than in the PPS write itself.

Coverage, all `#ifdef`-guarded so an unexposed signal is simply unroutable — outputs:
U1/U2/U3 TX, SPI1/2/3 SS/SCK/SDO, CLC1-4, PWM4H/L + PWMEA-D, CMP1-3, REFO1, CAN1TX.
Inputs: U1/U2/U3 RX, SPI1/2/3 SS/SCK/SDI, CLCINA-D, INT1-3, CAN1RX.

---

## GPIO change-notification — and a latent RP↔Port E collision

`dspic33ck_gpio_event.{c,h}` is a near-verbatim mirror of AK's CN event dispatcher: same
API, slot table, either-edge attach, `process_isr` logic, RP-first wrappers, per-port IRQ
helpers. Silicon deltas only:

| | AK | CK |
|---|---|---|
| CN registers | 32-bit | **16-bit** |
| Ports with CN | A-H | **A-E** (F/G/H rows = NONE) |
| CN flag word | IFS3 (A-D), IFS9 (E-H) | **IFS0** (A,B), **IFS1** (C), **IFS4** (D,E) |
| CNCON CNSTYLE/ON, CNEN0/CNEN1, `_CNxIP/IF/IE` | — | same |

Port E is included because on DM330030 it is a **real digital GPIO port** (LEDs, RGB,
switches, pot on RE3/5/6/7/8/9/13/14/15); it simply has no remappable RPn.

**The core fix that came with it:** `pin_from_rp()`/`rp_from_pin()` mapped virtual
**RP176-181 (RPV0-5) ↔ Port E bits 0-5**. RPV are internal PPS-only pins with no pad, and
the mapping **collided with real Port E pins** — RP181 → RE5 = LED2. No caller used
RP176-181, so nothing had ever misbehaved; it was a loaded gun. Both branches removed,
`RP_MAX` 181 → **79** (RD15), `RP_MIN` = **32** (RB0). Port E stays fully usable by pin
name, which is how the board uses it; only the bogus RP handle is gone.

The application still owns the `_CNxInterrupt` vectors by design — this layer only arms
priority, flag and enable.

---

## SPI/I2S/TDM transport — the register split, and the CLC frame-sync remap

Ports AK's framed-SPI I2S/TDM transport (RX/TX ping-pong DMA, per-instance block callback).
The core, diag and headers are an `ak→ck` identifier rename — they are device-independent,
touching silicon only through the hw/dma/fs_clc/diag layers — so they track AK 1:1. **The
port work is `_hw.c` and `_fs_clc.c`.**

**CK register split** (`s_spi_dev[SPI1..SPI3]` device-facts table):

| Concern | CK register |
|---|---|
| ENHBUF, SPIFE, MCLKEN, MSTEN, CKP, CKE, MODE32, SPIEN | `SPIxCON1L` |
| FRMEN, FRMSYPW, FRMPOL, FRMSYNC, IGNTUR, IGNROV, AUDEN, FRMCNT | `SPIxCON1H` |
| WLENGTH | `SPIxCON2L` |
| data port / baud | `SPIxBUFL` (+`BUFH`) / `SPIxBRGL` |
| event→DMA enables (SPIRBFEN, SPITBEN) | `SPIxIMSKL` |

CPU RX/TX interrupt flags: SPI1→IFS0, SPI2→IFS1, SPI3→IFS3.

> **The Stage-1 configuration recorded here was wrong in three fields** — `ENHBUF = 1`
> (forbidden with DMA), `MODE32 = 1` with `WLENGTH = 31` (a 32-bit slot cannot be DMA-fed,
> because the FIFO push is triggered by the `SPIxBUFH` access), and `FRMCNT = log2(slots)`
> (it counts **wire words**, so `log2(slots × 2)`). Corrected to `ENHBUF = 0`,
> `MODE16 = 1`, `WLENGTH = 0`, two wire words per 32-bit slot. Full evidence in
> `ck_silicon_findings.md` Part 1, defects 4 and 5 — including the two capabilities that
> correction cost (TDM32 and a true 50%-duty I2S FS).

**The CLC frame-sync remap** — CK's route differs from AK's single "CLC reads Virtual
Pin 8" data source, and this chain is the thing worth not re-deriving:

1. `hw_get_ss_pps_code(owner)` → `_RPOUT_SSxOUT`.
2. Reverse-scan the RPORx bank for the physical pin currently routed to FRMSYNC — that pin
   is the external FS. (CK RPORx = two 6-bit `RPnR` fields per 16-bit register.)
3. Route FRMSYNC → virtual pin **RPV0 (RP176, `_RP176R`)** — internal, no pad.
4. Route **CLC input A ← RP176** (`_CLCINAR = 176`) and select `CLC1SELL.DS1 = CLCINA`.
5. Configure **CLC1** as a J-K flip-flop clocked by CLCINA (Gate1 = CLK via `G1D1T`,
   J = K = 1 via `G2POL`/`G4POL` on empty gates, R pulsed via `G3POL` for a known initial
   Q, `LCOE = 1`, `LCEN = 1`), then repoint the external FS pin to `_RPOUT_CLC1OUT`.

`release()` clears `LCEN` and restores the FS pin to its FRMSYNC code. Self-contained
(`xc.h` only); PPS unlock uses the CK `__builtin_write_RPCON` sequence.

| Concern | AK (CLC10, 32-bit) | CK (CLC1, 16-bit) |
|---|---|---|
| Enable | `CLC10CONbits.ON` | `CLC1CONLbits.LCEN` |
| MODE / LCPOL / LCOE | `CLC10CON` | `CLC1CONL` |
| G1-4 POL | `CLC10CON` | **`CLC1CONH`** |
| Gate select (G1D1T) | `CLC10GLS` | `CLC1GLSL` / `CLC1GLSH` |
| Data select | `CLC10SEL.DS1 = Virtual Pin 8` | `CLC1SELL.DS1 = CLCINA`, fed from RPV0 |
| Marker virtual pin | RPV8 = `_RP137R` | **RPV0 = `_RP176R`** |
| FS output code | `_RPOUT_CLC10OUT` | `_RPOUT_CLC1OUT` |
| RPOR field | 4 × 7-bit / 32-bit | **2 × 6-bit / 16-bit** |

The three datasheet assumptions this route rested on — `MODE = 0b110` is "J-K flip-flop
with R", `DS1 = 0` selects CLCINA, and `RPINR45.CLCINAR` accepts virtual RP176 — were all
**confirmed on hardware**; see `ck_silicon_findings.md`.

**Two adaptations worth keeping.** The RX DMA vector uses
`__attribute__((__interrupt__, no_auto_psv))` rather than AK's `((interrupt, context))`:
the CK demo does not set up alternate CPU contexts, the repo's other ISRs use the
`__interrupt__` + PSV-model form, and this silences CK's "PSV model not specified" warning.
And `high_res_timer` started as a shim — see `ck_silicon_findings.md` Part 5 for why the
real one, when it arrived, turned out to have **never worked on any CK part** (it was
Timer2/3-paired; CK has neither).

Still deferred: sharing one CLC FS across several co-clocked instances (single clock domain
only today), and the CMSIS-SAI wrapper (a layer above — never in the core).

---

## Build profiles

| Configuration | MCU | Board | DFP | Linker script |
| --- | --- | --- | --- | --- |
| `CK256MP508_DM330030` | dsPIC33CK256MP508 | DM330030 Curiosity | `dsPIC33CK-MP_DFP 1.15.423` | `p33CK256MP508.gld` |
| `CK64MC105_EV88G73A` | dsPIC33CK64MC105 | EV88G73A Curiosity Nano | `dsPIC33CK-MC_DFP 1.10.386` | `p33CK64MC105.gld` |

```powershell
pwsh buildtools/build.ps1 -Configuration CK256MP508_DM330030
pwsh buildtools/build.ps1 -Configuration CK64MC105_EV88G73A
```

Outputs are isolated under `firmware.X/build/<configuration>/` and
`firmware.X/dist/<configuration>/` — objects from one MCU cannot be reused for the other.

**Why two profiles at all**, following the same principle as the Sonora AK512/AK128
configurations: shared HAL and application code must not imply that the MCU package, the
board routing, the configuration bits or the debugger are the same.
`DSPIC33CK_BOARD_DM330030` and `DSPIC33CK_BOARD_EV88G73A` identify the **board profile**,
not a generic MCU feature test — device-register differences belong in device/HAL adapters,
board wiring in board-profile code.

EV88G73A configuration bits: `FNOSC = FRCDIVN`, `POSCMD = NONE`, `FCKSM = CSECMD`,
`PLLKEN = OFF`, `FWDTEN = ON_SW`, `ICS = PGD3`, `JTAGEN = OFF`. (`FCKSM = CSECMD` matters —
DM330030 has `CSDCMD`, which disables clock switching and makes its FRC→PLL switch a silent
no-op. `ck_silicon_findings.md` Part 2.)

The EV88G73A board is programmed over the **Curiosity Nano on-board debugger**, not the
PKOB4 path — pin-level board facts are in `src/boards/ev88g73a/ev88g73a_pins.h`, which is
the single source for them (the boot-report wiring string lives there too, so text and
numbers change together). `src/profiles/` no longer exists; see `ck_src_layout.md` for the
board-layer shape that replaced it.

---

# Absorbed: the AK→CK API portability notes, the reset-snapshot API, the smoke test

Four separate documents used to carry the durable lessons of porting HAL APIs from
the dsPIC33AK tree to this one, and the evidence that the ports ran. They are folded
in here, verbatim, on 2026-08-11 because they said the same thing from four angles
and none of them was a progress memo worth its own file — the pre-fold filenames,
for a `git log --follow`, were `hal_api_portability.md`,
`hal_api_portability_review.md`, `reset_snapshot_api.md` and
`ck_hal_portability_smoke_test.md`. Their headings are demoted one level; nothing
else was edited, so anything they claim still carries its original evidence and its
original date.

The last of the four is the one to reach for as **evidence** rather than as a lesson:
it is the hardware smoke test (DMA self-test PASS, PPS routes, I²C1) that other
documents cite when they say the ported HAL was observed working.

The four rules worth reading even if you skip the rest:

- **A widened parameter must not be narrowed again by its caller.** Widening a HAL
  parameter to 32 bits buys nothing if the call site truncates it back.
- **Widen ONCE, after the switch.** Per-case 32-bit returns cost +16 B in the
  inlined TDM RX ISR; one widening after the dispatch costs nothing.
- **Two decoders are allowed to disagree, and did**: the two reset decoders read a
  Nano cold boot at `RCON=0x0083` differently. That disagreement is the finding, not
  a bug to paper over.
- **`.const` growth between two builds is not evidence until the tree is clean** —
  the git-describe build ID inflates it. The `pps_route_input()` ladder was the real
  ROM target; the `.const` explanation that was offered for it was withdrawn.

## CK / AK HAL API portability plan

### Purpose and contract boundary

This document records the API-level portability work between the existing
dsPIC33CK and dsPIC33AK HAL directories. It deliberately compares what an
application normally includes and calls, not register-table implementation
headers. The public boundary is:

~~~text
hal_adc:              *_adc.h
hal_clock:            *_clock.h
hal_dma:              *_dma.h
hal_gpio:             *_gpio.h, *_gpio_event.h, *_pps.h
hal_i2c:              *_i2c.h, *_i2c_master.h, *_i2c_slave.h
hal_reset:            *_reset.h
hal_spi_i2s_tdm:      *_spi_i2s_tdm.h
hal_timer:            *_high_res_timer.h, *_tick_timer.h
hal_uart:             *_uart.h
~~~

*_reg.h, *_device.h, *_common.h, *_hw.h, and board pin-map headers are
implementation or integration details. Applications must not use them as a
cross-target contract.

Source portability means that, after the target prefix is selected, the same
call has the same function name, return type, parameter types, public config
shape, and documented result. It does not mean that a CK binary can run on an
AK device, nor that every peripheral instance exists on both targets.

### Implemented in this branch

#### PPS pinmux helpers

CK now provides pinmux_route_input() and pinmux_route_output() with the same
public shape as the AK HAL. They configure the physical RP pin as digital GPIO
before routing PPS, preserving the glitch-aware output ordering. They
intentionally reject virtual RPV endpoints: those have no GPIO electrical
configuration and continue to use pps_route_*() directly.

The GPIO conversion pair gpio_pin_from_rp()/gpio_rp_from_pin() also uses raw
uint8_t RP parameters and results, matching AK exactly after the prefix. The
RP adapter operations retain gpio_rp_t for readability; it is a uint8_t alias
on both targets. This keeps virtual RPV endpoints out of the GPIO conversion
API while making the two public conversion signatures portable.

The UART1 board routes use these wrappers, so both CK board configurations
exercise the new public path at build time.

#### PPS signal-name union

The CK PPS input/output enums now include the logical AK-only signal names.
The additions are appended so existing CK enum numeric values do not change.
When a name has no matching CK device SFR, pps_route_*() returns false without
writing hardware. This is compile portability with an explicit capability
result, not an assertion that CK implements SPI4, ICM, or an AK PWM output.

The enum numbers are not a cross-target wire/storage ABI. A future AK update
may assign stable explicit values to the union, but applications should pass
the named enum directly to pps_route_*() and never serialize it.

#### DMA public-width alignment

The whole public DMA read/config surface is uint32_t now, matching AK:
dma_channel_cfg_t.count, dma_read_status(), dma_half_from_status(),
dma_isr_snapshot(), and dma_read_src(). CK retains its hardware limit:
dma_channel_config() rejects zero and values above UINT16_MAX; it never
truncates the requested count. The 16-bit DMAINTn and DMASRCn values are
zero-extended when returned.

This moves the CK-specific register width behind the HAL boundary while
keeping its real range limit explicit.

Two rules were learned making it free, and both are worth keeping:

- A widened PARAMETER must not be narrowed again by its caller. Both call
  sites that build a count were casting it back to uint16_t, which put the
  truncation in front of the rejection the HAL had just been given. Pass the
  computed width and let the HAL refuse.
- A widened RETURN value from an inlined multi-case switch should widen ONCE
  after the switch, not per case. dma_read_src() is inlined into the TDM RX
  DMA ISR with a runtime channel index, so returning 32 bits from each case
  cost a separate high-word clear per case: measured +16 B in that ISR.
  Assigning a 16-bit local in each case and widening at the single return
  makes the ISR byte-identical (0x340) to the 16-bit version.

#### Reset snapshot

CK provides the seven-function snapshot family fixed by
[ck_hal_ports_from_ak.md](ck_hal_ports_from_ak.md), as a NEW family rather than a
rename -- see the Decisions section below for why the legacy decoder had to
stay. EV88G73A's board_init() now captures with
DSPIC33CK_RESET_LATCH_AND_CLEAR_RCON instead of open-coding the RCON read plus
reset_cause_clear(), and its board seam reads the snapshot. DM330030 is
unchanged and still reads RCON live.

The console's ?sr prints both decodes side by side, because they answer
different questions: the legacy one is the only one that can name TRAPR (the
stack-overflow evidence in app_traps.c) and the portable one is what portable
application logic will branch on.

#### Measured cost on the tightest configuration

CK64MC105_EV88G73A, against the 66,432-byte program denominator that
[ev88g73a_rom_budget.md](ev88g73a_rom_budget.md) establishes:

| | program | data | free program |
|---|---|---|---|
| base (main `9f14380`) | 51,762 B | 4,740 B | 14,670 B |
| + PPS union, pinmux wrappers, DMA widths | 51,909 B | 4,740 B | 14,523 B |
| + reset snapshot live, EV88G73A on the wrappers | 52,443 B | 4,746 B | 13,989 B |
| total | **+681 B** | **+6 B** | |

All three rows are clean-worktree builds, which matters more than it sounds: the
git describe string is compiled into `.const`, so a dirty tree inflates the
program figure by the length of the extra `-dirty-...` text. The two feature
commits' messages quote 52,461 B / 13,971 B for the last row -- that was a
dirty-tree image whose build ID was 6 characters longer. The clean number above
is the one to reproduce, and the way to reproduce it is a `-Full` build of a
clean checkout.

Row 2 is +147 B and ALL of it is one function: `dspic33ck_pps_route_input` grew
630 -> 777 B, `dspic33ck_pps_route_output` did not change size at all, and
`.const` is byte-identical between the two images (17,224 B). So widening the
enums costs dispatch code in the input ladder only. An earlier reading of this
change on a different base attributed +240 B to `.const` jump-table entries for
values whose cases compile out (`p33CK64MC105` has only _RPOUT_PWM4H/PWM4L and
only _ICM1R.._ICM4R, so almost all of them do compile out); that did NOT
reproduce here and is withdrawn. If this configuration ever runs tight, the
target is `route_input`'s ladder -- a sparse table instead of a dense switch --
not the enum itself.

Row 3 is +534 B, and part of it belongs to row 2's feature: the pinmux wrappers
(45 B + 42 B) only enter the image once a board calls them, and exactly one board
file is compiled per configuration, so EV88G73A pays for them here rather than
above. The reset family contributes the four functions the board and console
actually reach -- capture 162 B, cause_str 30, is_captured 12, raw 9 -- plus its
call sites and the portable label strings, and +6 B of data for the three
statics. `snapshot_cause()`, `is_power_on_class()` and `is_warm()` still have no
caller and are garbage-collected.

### Decisions

#### Reset: the legacy raw decoder stays alongside the snapshot

The snapshot family is additive. Do not replace the legacy decoder with it.

The existing CK boards intentionally have different RCON latch policy:

- EV88G73A captures RCON early and clears it.
- DM330030 deliberately does not latch or clear RCON because that changes what
  its debugger investigation sees.

The current CK API expresses this explicitly through
reset_cause_str(raw, is_latched). An unconditional AK-style
reset_latch_cause() would silently select the EV88G73A policy for DM330030,
which is a behaviour change rather than an API cleanup.

There is also a C-language naming collision: CK's reset_cause_str takes two
arguments while AK's takes none. C cannot retain both under one name.

A third reason emerged during implementation: the two decoders have DIFFERENT
precedence, both are right, and on this board they visibly disagree. The legacy
one is most-specific-first (a console reset command reads as SWR, and a trap
conflict as TRAPR); the portable one is power-event-first, which is what AK
already ships. An EV88G73A cold start reads RCON=0x0083 -- POR, BOR and EXTR all
set, because the Nano holds MCLR while the supply comes up -- which the legacy
ladder calls EXTR(MCLR) and the portable decode calls POR (power-on, cold).
Diagnostics want the most specific bit; cold/warm logic wants the power event.
Merging them would have to break one of the two, so ?sr prints both.

So this change introduced a new, unambiguous snapshot family instead. Its exact
prefix-normalized declaration, semantics, legacy migration rules, and
acceptance criteria are in [ck_hal_ports_from_ak.md](ck_hal_ports_from_ak.md):

~~~c
typedef enum {
    RESET_LATCH_PRESERVE_RCON,
    RESET_LATCH_AND_CLEAR_RCON,
} reset_latch_policy_t;

bool reset_snapshot_capture(reset_latch_policy_t policy);
bool reset_snapshot_is_captured(void);
reset_cause_t reset_snapshot_cause(void);
uint32_t reset_snapshot_raw(void);
const char *reset_snapshot_cause_str(void);
bool reset_snapshot_is_power_on_class(void);
bool reset_snapshot_is_warm(void);
~~~

The old CK raw decoder remains available, and is now permanent rather than a
migration aid: it is the only decode that can name TRAPR or IOPUWR, and the
trap-conflict report in app_traps.c depends on that. The rename to
reset_decode_cause_str(raw, is_latched) was NOT done -- with both decoders
staying, renaming the surviving one would churn every existing caller to no
end. Board startup states its policy explicitly, which was the actual goal.

#### Clock: define a system-clock facade, do not copy CLKGEN names to CK

CK has a system oscillator / single-PLL model; AK exposes multiple PLLs and
CLKGEN blocks. Adding CK stubs for clkgen_configure() would compile but would
not provide portable behaviour.

The common API should express the application-level intent instead:

~~~c
clock_status_t clock_system_configure(
    const clock_system_config_t *config,
    uint32_t *actual_hz);
uint32_t clock_get_fosc_hz(void);
uint32_t clock_get_fcy_hz(void);
~~~

Target-specific PLL selection, NOSC switching, and peripheral-clock-generator
selection remain extension APIs. Unsupported requested capability must return
an explicit status, never silently choose a different clock.

#### UART: separate synchronous transport from extensions

The synchronous byte/buffer API is already close between targets. The current
uart_config_t is not: CK carries clock-mode fields while AK carries RX-ring and
asynchronous TX/RX fields.

The common design should use a minimal synchronous config and init operation,
with separate extension configuration for CK clock choices and AK asynchronous
engines. A target must reject a non-default extension it cannot implement; it
must not silently ignore it.

#### SPI/I2S/TDM: add a separate portable stream facade

> **SUPERSEDED (2026-08-08) — do not act on this section.** Sonora built this facade
> (`nora_tdm_stream.h` + adapter), then **withdrew and deleted it**; the native
> `nora_spi_i2s_tdm.h` is now the canonical transport contract, and a second reduced
> portability facade is explicitly not wanted on either side. The replacement plan —
> including why the int32_t block callback proposed below is rejected on measured CK RAM
> and ISR cost — was recorded separately. The original text is kept below as a record
> of what was considered.

Do not force the existing transport APIs into identical signatures. CK DMA
buffers are tdm_wire_slot_t pairs in wire order, whereas AK exposes int32_t
samples; their open/close/role/domain models also differ.

A later portable facade should use int32_t sample blocks, one stream config,
start/stop, status, and a block callback. CK performs wire-slot
encode/decode inside its adapter; the existing target transports remain
available for high-performance or multi-domain use.

### Near-term follow-up

1. Add an AK adc_result_str() and i2c_status_str() if diagnostic API-set
   equality is required.
2. Give the tick timer a common config shape; AK can expose only its supported
   clock source, while CK retains its exact-period validation.
3. Establish a header-contract check that normalizes prefixes and compares the
   public boundary listed above. It should fail on accidental signature or
   public-struct drift, while allowing documented target extensions.

## Review — `feature/ck-hal-api-portability` (2026-08-06)

Reviewed `abc7f96` + `38ae824` against `dspic33ak-audio-dsp-sonora` `main`
(`src/hal_gpio/dspic33ak_pps.{c,h}`, `src/hal_dma/dspic33ak_dma.h`,
`src/hal_reset/dspic33ak_reset.{c,h}`), plus `docs/ck_hal_ports_from_ak.md` and
`docs/ck_hal_ports_from_ak.md`.

Everything below is measured, not read off the diff. The two builds used
`buildtools/build.ps1 -Full` for each configuration; the baseline figures come
from a throwaway worktree at the branch base `3800929`.

**Read the sizes in this review as AVAS-base sizes.** Every absolute figure below
was measured with the AVAS feature underneath (item 1), which is what made the
configuration look 97.8 % full. After the rebase onto `9f14380` the same work
costs the same *delta* against a much emptier part. The rebased numbers, and the
disposition of every item, are in [Resolution](#resolution-2026-08-06) at the
end — that section is the current state; this one is the review as written.

### What holds up

**`pinmux_route_*` is exact parity.** AK's implementation is
`dspic33ak_pps.c:1078-1095`; CK's new wrappers have the same names, the same
parameter order, the same `initial_high` for the output form, the same
GPIO-configure-first ordering, and the same early-`false` on GPIO failure. The
RPV rejection the header claims is real and is inherited rather than reimplemented:
`dspic33ck_gpio_rp_config_digital_*()` goes through
`dspic33ck_gpio_pin_from_rp()`, which returns false for RP176..181
(`dspic33ck_gpio.c:260-280`). Both boards now call the wrappers, so the path is
build-exercised on both configurations, and `ev88g73a_board_init` got 24 bytes
smaller for it.

**The PPS enum union is complete in the direction that matters.** Set-comparing
the two headers: zero AK output names and zero AK input names are missing from
CK. The additions are appended, so existing CK numeric values are untouched, and
the header says explicitly that the numbers are not a wire ABI.

**The DMA width change does not hide a semantic difference.** This was the part
most likely to go wrong, because identical types can still mean different things.
It checks out: AK documents `DMAxCNT` as a verbatim element count and *not* an
"elements - 1" register (`dspic33ak_dma.h:76-79`), and CK writes the same value
verbatim with a hardware-measured comment saying why (`dspic33ck_dma.c:245-255`).
Same field, same meaning, both sides.

**The reset snapshot matches AK's observable behaviour where it overlaps.** The
seven `cause_str()` labels are character-identical to
`dspic33ak_reset_cause_str()` (`dspic33ak_reset.c:82-95`), and
`dspic33ck_reset_cause_t`'s members and order match `dspic33ak_reset_cause_t`.
`dspic33ck_reset_cause_clear()` touches only the seven cause bits, so acceptance
check 3 in `ck_hal_ports_from_ak.md` holds. Capture-once, preserve-writes-nothing,
and unknown-is-warm are all implemented as documented.

**Both configurations build clean.** No errors, no warnings from either.

    CK64MC105_EV88G73A    64,983 B program (97.8 % of 66,432)   5,714 B data (69 %)
    CK256MP508_DM330030   links, artifact produced

### 1. The branch is stacked on the AVAS branch, not on main

`origin/main..HEAD` is eight commits, and only two of them are this work:

    38ae824 feat(hal): add CK reset snapshot API
    abc7f96 feat(hal): align CK API portability with AK
    3800929 docs(avas): the AVAS synth runs on CK at 80.3 %, with the envelope frozen
    9f8af1d Merge origin/main into feat/avas-type_ty-fixedpoint
    d418120 feat(avas): per-part timing and a part mask, ...
    ac661dc feat(avas): wire the AVAS path in, and find that it overruns the block ISR
    93c6f20 feat(dsp): fixed-point Type_TY AVAS engine for CK, verified bit-exact offline
    117743d docs(avas): Type_TY AVAS L1 line model feasibility study for CK

So a PR from this branch merges the whole AVAS feature — including an engine
whose own commit message records that it overruns the block ISR and only runs
with the envelope frozen. Rebase the two HAL commits onto `9f14380` (the
merge-base) so they can be reviewed and merged on their own.

This also explains item 2 below: the flash headroom this branch spends is
headroom the AVAS work is competing for.

### 2. +498 B of flash on the tightest configuration, mostly for signals the board does not have

Measured, same `-Full` recipe on both:

| | program | data |
|---|---|---|
| base `3800929` | 64,485 B | 5,714 B |
| HEAD `38ae824` | 64,983 B | 5,714 B |
| delta | **+498 B** | 0 |

Free space goes 1,947 B -> 1,449 B against the 66,432 B denominator the ROM budget
establishes. That is about a quarter of the remaining headroom on the configuration
that budget exists for.

By section: `.text` +258 B, `.const` **+240 B**. Per function, from the two maps
(bytes):

    dspic33ck_pps_route_input              630 -> 777   +147
    dspic33ck_pinmux_route_output          new  ->  45    +45
    dspic33ck_pinmux_route_input           new  ->  42    +42
    dspic33ck_dma_channel_config           633 -> 645    +12
    dma_selftest_run                       546 -> 558    +12
    ..._tdm_diag_check_deadline             42 ->  51     +9
    dspic33ck_dma_read_status               36 ->  39     +3
    ev88g73a_board_init                    195 -> 171    -24

`dspic33ck_pps_route_output` did not change size at all, and the reason is the
interesting part. On `p33CK64MC105`, checked in the DFP header
(`.mchp_packs/.../dsPIC33C/h/p33CK64MC105.h`):

* the only remappable PWM outputs are `_RPOUT_PWM4H` and `_RPOUT_PWM4L` — every
  one of the new `PWM1H/2H/3H/5H/5L/6H/6L/7H/7L/8H/8L` cases compiles out;
* `_RPOUT_SS4/SCK4/SDO4` do not exist either;
* on the input side only `_ICM1R.._ICM4R` exist — `_REFI1R`, `_SS4R`, `_SCK4R`,
  `_SDI4R`, `_ICM5R.._ICM9R` do not.

So almost all of the new cases vanish at compile time, and the flash is spent
anyway: widening the enum widens the `switch`, and the dense jump table gains an
entry per new value even when that entry just points at `default: return false`.
That is what the +240 B of `.const` is. Nothing in either board routes ICM, and
nothing can route SPI4, REFI1 or PWM5-8 on this part.

The cost may well be worth paying — but it should be a recorded decision rather
than a side effect, because 1.4 KB is the same order as the envelope-rebuild
budget the AVAS work is trying to find. Three options, cheapest first: record it
and move on; put the union behind a `#define` so a flash-tight configuration can
opt out; or replace the two `switch` ladders with a sparse table, which removes
the per-value table cost and would pay for itself.

### 3. The DMA width alignment stops two functions short, and the doc does not say so

`ck_hal_ports_from_ak.md` presents "DMA public-width alignment" as done for
`count`, `read_status()` and `half_from_status()`. Two public reads in the same
header still differ from AK:

    dspic33ck_dma_isr_snapshot()   uint16_t     AK: uint32_t
    dspic33ck_dma_read_src()       uint16_t     AK: uint32_t

`isr_snapshot()` is the one that matters, because it is the *source* of the
value: `dspic33ck_spi_i2s_tdm.c:1465` holds `uint16_t dma_stat` and then passes
it to helpers that now take `uint32_t`.

The cycle cost of that promotion turned out to be nil — the TDM ISR did not grow
by a single byte, and only the out-of-line `diag_check_deadline` grew (+9 B). So
this is not a performance objection. It is that a reader of the document will
believe the DMA public surface matches AK when two of its reads do not, which is
exactly the drift the document's own follow-up item 3 (a header-contract check)
is meant to catch. Either widen both and re-measure the ISR, or list them in the
document as deliberate CK extensions.

### 4. The `count` widening is cancelled at its only real caller

`dspic33ck_spi_i2s_tdm_hw.c:505`:

    .count = (uint16_t)(count * 2u),   // 32-bit slots as 16-bit DMA half-words

The point of the 32-bit field is "reject an unrepresentable request instead of
truncating it". This cast truncates before the HAL can reject. It cannot
overflow today (the count derives from a RAM-resident buffer, so `count * 2`
cannot approach 65535), so this is not a live defect — but it silently removes
the new guarantee at the one call site that has a computed count. Drop the cast
and let `dma_channel_config()` do the check it was just given.
`dma_selftest.c:52` has the same now-pointless cast on a constant.

### 5. Nothing has ever executed the reset snapshot API

There is no caller on either board, and the linker agrees: all seven functions
appear under **Discarded input sections** in the EV88G73A map, and the three new
statics were dropped too (data size unchanged to the byte). So the API costs
nothing today — and acceptance checks 2, 3 and 4 of `ck_hal_ports_from_ak.md`
(second capture is a no-op, clear-policy saves raw before clearing, preserve
policy writes nothing) are unverified and cannot be verified while the code is
garbage-collected.

The cheapest way to close that is the migration the document already sanctions:
point EV88G73A's seam at the snapshot with `AND_CLEAR`. Two things to carry into
that change:

* `app_traps.c:245` takes `uint16_t rcon = board_reset_cause_raw()` and needs
  `TRAPR` and `POR` out of the **latched** word, so the seam must return
  `(uint16_t)dspic33ck_reset_snapshot_raw()`, not a live `RCON` read. `TRAPR`
  and `IOPUWR` are deliberately not in the portable cause enum, so the raw word
  stays load-bearing — the trap-conflict report at `app_traps.c:278` is the only
  evidence a stack overflow ever produces.
* DM330030 must stay on the legacy live-`RCON` path or `PRESERVE_RCON`, which is
  what the document already says.

### 6. The AK half does not exist, so the CK implementation is the de facto spec

`dspic33ak_reset.h` has none of the seven snapshot functions — only
`reset_latch_cause/cause/raw/is_power_on_class/is_warm/cause_str`. That is fine
as a staged plan, but two corrections for the document while it is still the
contract:

* **`ck_hal_ports_from_ak.md` says "The power-event-first rule is new shared-API
  behaviour." It is not.** AK already decodes POR -> BOR -> EXTR -> SWR -> WDTO
  in that order, with a comment explaining the MCLR-held-through-power-on case
  (`dspic33ak_reset.c:16-40`). CK adopted AK's precedence; the document should
  say so, because "new behaviour" invites someone to re-litigate a rule that is
  already shipping on AK.
* One genuine behaviour change to flag in the AK migration section: AK's
  `reset_latch_cause()` is documented as re-reading an already-cleared RCON on a
  second call and latching UNKNOWN. Implemented over the snapshot, the second
  call becomes a no-op that preserves the good snapshot. That is an improvement,
  but it is a change, not a wrapper. Also, AK currently snapshots `s_raw` and
  then decodes from live `RCONbits` — two reads of RCON. CK decodes from the
  saved word. The AK reimplementation should adopt CK's single read.

### 7. Smaller things

* **Both commits have empty bodies and no `Co-Authored-By`.** Every neighbouring
  commit in this repo carries measurements, the alternatives that lost, and the
  build sizes. Items 1 and 2 above are exactly what those bodies would have
  recorded; as it stands the branch's flash cost and its base are undocumented.
* `reset_snapshot_captured`, `reset_snapshot_raw` and `reset_snapshot_cause` skip
  the `s_` prefix that 13 of the module statics in CK `hal_*` use — and that
  AK's own `s_cause`/`s_raw` use, in the file being aligned with.
* The `reset_cause_t` -> `reset_cause_desc_t` rename in `dspic33ck_reset.c` was
  not forced by a collision (the public type is `dspic33ck_reset_cause_t`), but
  it is the right rename: the local type describes a table row, not a cause.
* The working tree shows `src/boards/ev88g73a/ev88g73a_board.c` modified with an
  empty `git diff` — stat-only, nothing lost.

### Suggested order

1. Rebase onto `9f14380` so the two HAL commits stand alone (item 1).
2. Drop the two `(uint16_t)` casts on `.count` (item 4).
3. Decide and record the flash trade-off for the enum union (item 2), and write
   the sizes into the commit bodies (item 7).
4. Either widen `isr_snapshot()`/`read_src()` or document them as CK extensions
   (item 3).
5. Migrate EV88G73A's seam onto the snapshot so it is executed at least once
   (item 5), and fix the two document claims (item 6).

### Resolution (2026-08-06)

All five steps done, in that order. Sizes below are the rebased branch, so they
supersede every absolute figure earlier in this document.

| | program | data | free program |
|---|---|---|---|
| base `9f14380` (main) | 51,762 B | 4,740 B | 14,670 B |
| + PPS union, pinmux wrappers, DMA widths (commit 1) | 51,909 B | 4,740 B | 14,523 B |
| + reset snapshot live, EV88G73A on the wrappers (commit 2) | 52,461 B | 4,746 B | 13,971 B |
| total | **+699 B** | **+6 B** | |

`CK256MP508_DM330030` is 28,275 B program / 4,756 B data. Both configurations
build with zero warnings, and the middle commit was built on its own to confirm
it is not a broken intermediate.

The total is larger than the +498 B measured on the AVAS base, which is not a
measurement discrepancy: the reset snapshot was entirely garbage-collected when
this review was written and is now reached, which is +249 B of it.

**Item 2's `.const` finding does not reproduce on this base, and is withdrawn.**
Measured between the base and first-commit images: `.const` is byte-identical
(17,224 B), and the whole +147 B is `dspic33ck_pps_route_input` growing
630 -> 777 B, with `pps_route_output` unchanged to the byte. The device-header
facts behind the original explanation are still true -- almost every new case
does compile out on `p33CK64MC105` -- but the cost is dispatch code in the input
ladder, not jump-table entries, so the remedy to reach for if this configuration
runs tight is a sparse table in `route_input` specifically.

The split between the two rows is not the split between the two features: the
pinmux wrappers (45 B + 42 B) enter the image only when a board calls them, and
one board file is compiled per configuration, so EV88G73A pays for them in the
second row even though they are implemented in the first.

That correction came from re-measuring rather than from re-reading, and it nearly
went the other way: one `objdump` in this session ran with a stale Bash working
directory and reported the *base* worktree's image, which briefly looked like the
enum union having no cost at all. An explicit `cd` and a second look settled it.

**Item 3 — widened, and free.** `isr_snapshot()` and `read_src()` both return
`uint32_t` now, so the whole public DMA surface matches AK. EV88G73A went
52,242 -> 52,212 B (−30 B), data unchanged, and
`dspic33ck_spi_i2s_tdm_inst_rx_isr` is byte-identical at 0x340 (832 B).

Getting there produced a rule worth keeping, and a wrong first answer worth
recording. The ISR initially grew +16 B; I attributed that to the widened
`uint32_t` local in the ISR and wrote a comment saying so. Narrowing the local
back did not shrink it, which proved the comment false. The real cause was
`read_src()` returning 32 bits *from each case* of its `switch`: it is inlined
with a runtime channel index, so every case paid its own high-word clear.
Assigning a 16-bit local per case and widening once at the single `return` costs
nothing. The measurement, not the plausible explanation, found it.

`spi_i2s_tdm_diag.h`'s `rx_dma_last_status` stays deliberately 16-bit — it is a
`volatile` written once per RX block, and widening it would buy a wider store of
a value the ISR already has.

**Item 5 — migrated and verified on hardware.** `ev88g73a_board_init()` calls
`dspic33ck_reset_snapshot_capture(DSPIC33CK_RESET_LATCH_AND_CLEAR_RCON)` in place
of the open-coded `RCON` read plus `dspic33ck_reset_cause_clear()`, the seam
returns `(uint16_t)dspic33ck_reset_snapshot_raw()`, and `?sr` prints the portable
cause alongside the legacy string. Live, on the board:

    after programming:  reset cause this boot: EXTR(MCLR)  RCON=0x0083  portable=POR (power-on, cold)
    after *sr:          reset cause this boot: SWR(software reset)  RCON=0x0040  portable=SWR (software, warm)

The second line closes acceptance check 3. The first line is a finding the review
did not anticipate: **the two decoders disagree on real hardware, on every cold
start.** A Nano cold boot sets POR, BOR *and* EXTR at once because the debugger
holds MCLR while the supply rises. Most-specific-first calls that EXTR;
power-event-first calls it POR. Both are right for what they are asked — the
legacy string is a diagnostic and the only decode that can name TRAPR, the
portable cause is the cold/warm classification application logic branches on, and
calling a supply that just came up "warm" is the error that would matter. So
`?sr` prints both, and the argument in `ck_hal_ports_from_ak.md` that they "can
disagree only if POR and SWR are set together" was too narrow and has been
replaced. The audio path also came back after the reboot with the widened DMA
code running: `TDM1 max=36.0us(5.3%) margin=635.3us miss=0`.

Three of the seven snapshot functions still have no caller and are still
garbage-collected: `snapshot_cause()`, `is_power_on_class()`, `is_warm()`. That is
reported rather than fixed — inventing a caller to make a size table look
complete would be worse than the gap.

**Item 6 — both document claims fixed**, plus the two AK behaviour changes
(second call becomes a no-op; adopt CK's single RCON read) written into the AK
migration section as decisions to take before that work starts, not during it.

**Item 7 — statics renamed** to `s_snapshot_captured` / `s_snapshot_raw` /
`s_snapshot_cause`, and the commit bodies rewritten with these measurements.

#### Deliberately not done

* **DM330030 was not converted to `PRESERVE_RCON`.** It would change that seam's
  value from a live `RCON` read to a boot snapshot — a behaviour change on the
  board whose hardware verification is still deferred — and buy no runtime
  coverage, because that configuration is compile-only. Acceptance check 4 stays
  inspection-only and says so.
* **`app_traps.c`'s POR test was not converted to `is_power_on_class()`.** It
  would regress DM330030, which never captures a snapshot and would therefore
  report false for every boot. The trap path keeps bit-testing the latched word.
* **`reset_cause_str()` was not renamed to `reset_decode_cause_str()`.** With both
  decoders staying permanently, renaming the surviving one churns every existing
  caller for no gain.

#### Hardware smoke test

The committed tree was then rebuilt clean, flashed, and exercised end to end on
EV88G73A: [ck_hal_ports_from_ak.md](ck_hal_ports_from_ak.md).
Everything passed, including `miss=0` over 336,697 TDM blocks through the widened
DMA read path and both `?sr` decodes across a cold boot and a `*sr` warm boot.

That build also corrects one number in this document and in the two feature
commits' messages: the final image is **52,443 B** program / 13,989 B free, not
52,461 / 13,971. The earlier figure came from a dirty worktree, and the git
describe string is compiled into `.const`, so `-dirty-...` made the image 18 B
larger. Total cost against main is **+681 B**, not +699 B. The commit messages
were left as they are rather than rewritten a third time for build-ID noise;
[ck_hal_ports_from_ak.md](ck_hal_ports_from_ak.md) now states the clean figure and
the reason they differ.

## CK / AK reset snapshot API

### Status and goal

This is the implementation contract for the next shared reset API.  Names in
this document omit the target prefix: CK spells them `dspic33ck_*` and AK
spells them `dspic33ak_*`.  Apart from that prefix, each declaration below
must have the same name, return type, parameter type, enum order, and
behaviour on both targets.

The API exposes one early-boot RCON snapshot.  It does **not** claim that the
two devices have identical RCON bit layouts.  Raw bits stay diagnostic data;
the portable result is the common cause enum.

### Public contract

```c
typedef enum {
    RESET_LATCH_PRESERVE_RCON = 0,
    RESET_LATCH_AND_CLEAR_RCON = 1,
} reset_latch_policy_t;

typedef enum {
    RESET_CAUSE_UNKNOWN = 0,
    RESET_CAUSE_POWER_ON,
    RESET_CAUSE_BROWNOUT,
    RESET_CAUSE_EXTERNAL,
    RESET_CAUSE_SOFTWARE,
    RESET_CAUSE_WATCHDOG,
    RESET_CAUSE_OTHER,
} reset_cause_t;

bool          reset_snapshot_capture(reset_latch_policy_t policy);
bool          reset_snapshot_is_captured(void);
reset_cause_t reset_snapshot_cause(void);
uint32_t      reset_snapshot_raw(void);
const char   *reset_snapshot_cause_str(void);
bool          reset_snapshot_is_power_on_class(void);
bool          reset_snapshot_is_warm(void);
```

`reset_snapshot_capture()` is an early-boot operation.  It reads RCON exactly
once into module-private state and must run before any clock, port, or
peripheral setup.  It returns `true` only for the first valid capture after a
core reset.  A second call, or an invalid policy, returns `false` and performs
no read or clear.  This prevents an accidental second capture from replacing a
good snapshot with already-cleared RCON.

Before a successful capture, `reset_snapshot_is_captured()` is false,
`reset_snapshot_cause()` is `RESET_CAUSE_UNKNOWN`, `reset_snapshot_raw()` is
zero, and `reset_snapshot_is_warm()` is true.  Treating unknown provenance as
warm is deliberate: a conservative codec pre-shutdown is safe, while assuming
a cold start can pop an already-powered codec.

### Policy semantics

| Policy | RCON action | Portable cause | Intended user |
|---|---|---|---|
| `RESET_LATCH_AND_CLEAR_RCON` | Read once, classify, then clear only this family’s reset-cause flags | Authoritative common cause | A boot path that needs a reliable cold/warm decision |
| `RESET_LATCH_PRESERVE_RCON` | Read once; do not write RCON | Always `RESET_CAUSE_UNKNOWN` | Debugger-oriented or diagnostic boot paths that must leave RCON untouched |

The preserve policy intentionally does not classify even a raw word containing
one known bit.  RCON flags are sticky and may be left over from an earlier
reset; without clearing after the preceding capture there is no proof that a
bit names this boot.  The raw snapshot is still available for diagnostics, but
the portable cause remains unknown and therefore warm/conservative.

The clear policy may choose one cause from multiple flags because it owns the
latch/clear protocol.  Its portable precedence is:

1. `POWER_ON` when POR is set
2. `BROWNOUT` when BOR is set without POR
3. `EXTERNAL` when the external-reset/MCLR flag is set
4. `SOFTWARE` when SWR is set
5. `WATCHDOG` when WDTO is set
6. `OTHER` for a target-specific reset flag (CK trap/illegal-opcode, AK CM) or
   no portable flag after a valid capture

The power-event-first rule is NOT new: AK already decodes POR -> BOR -> EXTR ->
SWR -> WDTO in exactly this order, with a comment explaining the
MCLR-held-through-power-on case (`dspic33ak_reset.c`).  CK adopts AK's shipping
precedence; nothing here re-litigates it.

It is not a rewrite of CK's legacy raw decoder either, whose priority remains
available for existing board diagnostics -- and the two orders genuinely
differ.  The legacy CK ladder is most-specific-first: TRAPR, IOPUWR, WDTO, SWR,
EXTR, POR, BOR, so that a console reset command reads as SWR rather than as a
power-on.  This one is power-event-first.

**They disagree on real hardware, and that is the intended behaviour of both.**
An EV88G73A cold start reads `RCON=0x0083` -- POR and BOR and EXTR all set at
once, because the Nano's debugger holds MCLR while the supply comes up.  The
legacy ladder names that boot `EXTR(MCLR)`; the portable decode names it
`POR (power-on, cold)`.  Neither is a bug:

- The legacy string answers *"what is the most specific thing RCON can tell
  me"*.  It is a diagnostic, and it is the only decode that can say `TRAPR` --
  the sole evidence a stack overflow leaves behind (`app_traps.c`).
- The portable cause answers *"cold or warm"*, which is a question application
  logic branches on.  A supply that has just come up is cold regardless of what
  else was asserted during it.  Calling that boot warm is the failure that
  matters: it would skip a codec pre-shutdown on a genuinely cold start.

So neither order should be "fixed" to match the other, and no caller should
assume the two agree -- the console prints both side by side for this reason.
What WOULD be indefensible is classifying under `PRESERVE`, where bits may have
accumulated across several resets and no precedence can be justified at all.
That is why the decode runs only on the `AND_CLEAR` path; do not extend it.

`reset_snapshot_is_power_on_class()` is true only for `POWER_ON` and
`BROWNOUT`.  `reset_snapshot_is_warm()` is its inverse, including `UNKNOWN`
and `OTHER`.

`reset_snapshot_cause_str()` returns a non-NULL string literal and must use
the same portable labels on both targets:

```text
UNKNOWN (warm)
POR (power-on, cold)
BOR (brown-out, cold)
MCLR (external, warm)
SWR (software, warm)
WDT (watchdog, warm)
OTHER (warm)
```

Applications must not parse the string; use `reset_snapshot_cause()` for
logic.  The raw word is intentionally not a cross-target ABI: CK zero-extends
its 16-bit RCON value and AK returns its native 32-bit word.

### Non-breaking target migration

#### CK

Add the public enum types and the seven functions above while retaining these
legacy APIs unchanged:

```c
const char *reset_cause_str(uint16_t raw, bool is_latched);
void        reset_cause_clear(void);
```

The existing legacy decoder keeps its CK-specific detail and priority, such as
`TRAPR` and `IOPUWR`.  The snapshot implementation owns separate private
state (`captured`, `raw`, and common `cause`) and uses the existing clear
primitive only after it has saved the raw word.

EV88G73A has replaced its direct `RCON` read plus `reset_cause_clear()` with one
call to `reset_snapshot_capture(RESET_LATCH_AND_CLEAR_RCON)` (2026-08-06), and
its `board_reset_cause_raw()` seam now returns `(uint16_t)reset_snapshot_raw()`.
The seam keeps exposing the LEGACY string, and permanently: `app_traps.c` needs
`TRAPR` and `POR` bit-tested out of the latched word, and `TRAPR` has no member
in the portable cause enum.  The console prints both decodes rather than
choosing.

DM330030 must not be converted to the clear policy.  It may adopt
`RESET_LATCH_PRESERVE_RCON` if it wants an early stable raw diagnostic value;
that adds no RCON write and its common cause intentionally remains unknown.
This was considered and NOT done: its seam's value would change from a live
`RCON` read to a boot snapshot, which is a behaviour change on the board whose
hardware verification is still deferred, and it would buy no runtime coverage
because that configuration is compile-only.  So the `PRESERVE` branch stays
verified by inspection until a board wants it.

#### AK

The existing `reset_cause_t` enum already has the required members and order.
Implement the snapshot family over its present private `s_cause`/`s_raw`
state, and change the early boot call to:

```c
reset_snapshot_capture(RESET_LATCH_AND_CLEAR_RCON);
```

Keep AK's existing API as compatibility wrappers while applications migrate:

```c
void           reset_latch_cause(void);
reset_cause_t  reset_cause(void);
uint32_t       reset_raw(void);
bool           reset_is_power_on_class(void);
bool           reset_is_warm(void);
const char    *reset_cause_str(void);
```

`reset_latch_cause()` delegates to the clear-policy snapshot capture and
ignores its boolean result, preserving the existing void signature.  The
other legacy functions read the same snapshot state.  This avoids breaking the
audio transport and console call sites while making the common family the
canonical implementation.

Two things about the AK side are behaviour changes, not wrapping, and should be
decided before that work starts rather than discovered during it:

1. **Second-call behaviour.** AK's `reset_latch_cause()` re-reads an
   already-cleared RCON on a second call and latches `UNKNOWN`.  Over the
   snapshot, a second call is a no-op that preserves the good snapshot.  That is
   an improvement, but any AK caller relying on re-latching would change
   behaviour.
2. **One RCON read, not two.**  AK currently saves `s_raw` and then decodes from
   live `RCONbits` -- two reads of a register it is about to clear.  CK decodes
   from the saved word.  Adopt CK's single read; it is what makes the "reads RCON
   exactly once" sentence in this document true.

### Acceptance checks for the implementation phase

1. Prefix-normalized public declarations match exactly, including enum values.
2. A second capture does not change raw state or write RCON.
3. Clear policy saves raw RCON before clearing only reset-cause flags.
4. Preserve policy makes no RCON write and always reports `UNKNOWN`/warm.
5. Legacy CK decode and legacy AK entry points retain their present behaviour.
6. Build both CK board configurations and both AK device families.  No board
   access is required for these checks.

#### CK status (2026-08-06)

| Check | How it stands |
|---|---|
| 1 | Verified by inspection against `dspic33ak_reset.h`; the cause enum's members and order match, and there is no AK snapshot family to compare against yet. |
| 2 | By inspection: `capture()` returns false on `s_snapshot_captured` before any read or write.  Not reachable at run time -- the only caller is the first statement of `ev88g73a_board_init()`, so a second call cannot occur without adding one. |
| 3 | Verified on EV88G73A hardware via `?sr`, and re-verified on the committed tree by the smoke test in [ck_hal_ports_from_ak.md](ck_hal_ports_from_ak.md) (2026-08-06, build `6206894-20260806T153738751`).  After programming: `reset cause this boot: EXTR(MCLR)  RCON=0x0083  portable=POR (power-on, cold)`.  After `*sr`: `reset cause this boot: SWR(software reset)  RCON=0x0040  portable=SWR (software, warm)`.  The second line is the proof: the raw word is still reportable *after* the clear, and it contains only SWR -- so the preceding boot's clear did happen and this boot's snapshot was taken before its own clear. |
| 4 | By inspection only.  No board selects `PRESERVE` (see the DM330030 note above), so nothing executes it. |
| 5 | Verified: the legacy decoder is unchanged, and both boards' seams still route through it. |
| 6 | Both CK configurations build warning-free.  AK is not implemented yet. |

Checks 2 and 4 stay inspection-only deliberately.  Making them executable would
mean adding a self-test that calls `capture()` a second time -- which is the
exact thing the once-only rule exists to prevent -- or converting a board to a
policy it does not want.  There is no host-side test harness in this repo
(`tools/host_check` is empty), so the honest statement is that they are read,
not run.

## EV88G73A smoke test of feature/ck-hal-api-portability

Date: 2026-08-06.
Branch head: `6206894` (docs commit), tree clean.
Image: `CK64MC105_EV88G73A`, `-Full` rebuild, build ID
`6206894-20260806T153738751-2d35cddae4`.
Hardware: EV88G73A + Curiosity Nano `MC020023603RYN000842`, direct jumpers
(**A-XTAL**: WM8904 self-clocked from X1, dsPIC33CK is TDM slave). EV88G73A carries
CODEC-A only, so there is **no B-jumper** — the A jumper is A-XTAL, or A-extMCLK when
the dsPIC33CK is the TDM master. An earlier revision of this line said "B-XTAL", a name
borrowed from the sonora AK boards.
Console: `tools/list-serial-monitor.ps1` bridge on `http://127.0.0.5:8080`
(COM60 @230400). The COM port was never opened directly.

Sizes of the flashed image, from
`firmware.X/dist/CK64MC105_EV88G73A/production/memoryfile.xml`:
program 52,443 / 66,432 B used (13,989 free), data 4,746 / 8,192 B used.

### Board state on arrival (worth recording)

The board was running an unrelated image -- `Build ID = HOTKEYA`, the AVAS
build, about an hour of uptime at `max=404.8us(60.2%)`. No console traffic from
anyone for the preceding 12 minutes, so it was left over rather than in use. It
was muted with `*ts` (waiting for the exact `analog mute verified` phrase) before
programming, and this branch's image replaced it. Per workspace convention the
previous firmware was not restored; whoever needs HOTKEYA reflashes it.

### Results

| Check | Result |
|---|---|
| Flash + UART marker | `flash-curiositynano: programming succeeded`, marker `6206894-20260806T153738751-2d35cddae4` seen on the console |
| Boot: DMA selftest (widened `uint32_t` public API) | `DMA selftest (ch3, RAM->RAM, software CHREQ): PASS` |
| Boot: PPS routes via the new `pinmux_route_*` wrappers | `BCLK=RP50 FS=RP51 SDO=RP48 SDI=RP49, I2C1 ASDA1=RP56 ASCL1=RP57` -- the console itself is proof, since UART1 is routed through the wrappers |
| Boot: WM8904 over I2C1 | `dev ID is 0x8904(good)`, `DC servo startup done R4D=0x3f0f 281(ms)`, `apply=verified`, `analog output unmute` |
| Boot: transport | `passthrough running (TDM8/32-bit SLAVE, WM8904 master, path=gain ...)`, SW0 ramp 791 ms (asked 800) |
| `?gv` | `build ID = 6206894-20260806T153738751-2d35cddae4` |
| `?sr` after programming | `EXTR(MCLR)  RCON=0x0083  portable=POR (power-on, cold)` |
| `?dt` (SCCP1 high-res timer) | `256 samples: step min=55 max=83 backward=0 (worst=0) steps>=1024=0` |
| `?xl` | `last trap: none`, `traps since power-on: 0` |
| `?tp` | `path = gain`, `CODEC-IN peak slot0=3192 slot1=3226 of 32767, active slots=01......` |
| `?ts` | `WM8904 audio running (codec unmuted, TDM/DMA active)` |
| `*tp` x3 | `-> copy` `-> mute` `-> gain`, each confirmed by a following `?tp`; ends back on `gain` |
| TDM ISR load (the widened `dma_read_src()` path runs in this ISR) | `max=36.0us(5.3%) margin=635.5us (run,act,blk,miss)=(1,1,336697,0)` -- **miss=0 over 336,697 blocks** |
| `*sr` then `?sr` | `SWR(software reset)  RCON=0x0040  portable=SWR (software, warm)`, after a full clean re-boot (selftest PASS, codec re-verified) |

The last row is the substantive one for this branch. It closes acceptance check 3
of [ck_hal_ports_from_ak.md](ck_hal_ports_from_ak.md) on hardware: RCON is still
reportable *after* the snapshot cleared it, and it contains only SWR -- so the
previous boot's clear really happened and this boot's snapshot was taken before
its own clear. The `portable=` field on both boots also demonstrates the
documented disagreement between the two decoders live: legacy `EXTR(MCLR)`
vs portable `POR (power-on, cold)` on the cold start, and agreement on the warm
one.

`?tp` reports `slot0=1 slot1=1` on the repeat reads because nothing was driving
the codec input after the first (peak-hold) read; the 3192/3226 first sample is
the residue held since boot. Not a defect -- peak-hold is cleared by each read.

### Deliberately not run

`*xa`, `*xm`, `*xs` force an address error, a math error and a stack overflow
respectively. They are destructive by design (the last one is the case that can
only be reported through `TRAPR` after the reset) and none of them touch code
this branch changed. `?xl` confirming a clean trap history is the coverage this
branch needs.

DM330030 remains compile-only; it is unchanged by this branch except for the
DMA/PPS type widening, and it has no board here.

### Board-health spot check, A-XTAL (2026-08-09) — not this branch's image

Read-only re-check of the bench, requested as "a simple A-XTAL test". Nothing was
flashed and the board was not reset; the console was reached only through the
`serial-monitor` bridge on `http://127.0.0.5:8080` (profile `ck`, COM60 @230400),
already running and left running.

**What was on the board matters for reading this:** `Build ID = sizeref`, a
ROM-size reference image flashed by other work at 20:18:55 the same day — **not a
build of this branch**, which changed only comments and prose and was never built.
So this section is evidence about the board and the A-XTAL jumper, not about any
branch.

| Check | Reading |
|---|---|
| Boot banner (20:18:56, the boot still running) | `passthrough running (TDM8/32-bit SLAVE, WM8904 master, path=chain (TDMin -> AVAS -> Gain -> TDMout))` — the role A-XTAL implies |
| DMA selftest | `ch3, RAM->RAM, software CHREQ: PASS` |
| `?ts` | `WM8904 audio running (codec unmuted, TDM/DMA active)` |
| **Master clock, measured** | `blk` 5,642,432 -> 5,687,172 over 29.833 s = 1499.7 block/s x 32 frames = **47,990 Hz**, i.e. 48 kHz within 0.02 % — the WM8904 is self-clocked from X1 |
| `miss` | **0**, at `blk = 5,687,172` (~63 min of uptime) |
| TDM1 ISR load | `max=47.3us(7.0%) margin=623.1us`, AVAS stage stopped |
| `?tp` | `path = chain`, `CODEC-IN peak slot0=2953 slot1=2815 of 32767, active slots=01......` — real signal into the transport |
| `?dt` (SCCP1) | `256 samples: step min=43 max=73 backward=0 (worst=0) steps>=1024=0` |
| `?xl` | `last trap: none`, `traps since power-on: 0` |
| `?sr` | `EXTR(MCLR)  RCON=0x0083  portable=POR (power-on, cold)` |
| Listening | **PASS** — reported by the operator as all audio paths good |

`blk` advancing at exactly the codec rate is the check worth keeping: `blk=0` with
`run,act=1,1` means the master is not clocking (jumper first, firmware second),
and that failure looks identical to a healthy armed transport in every other
field.

Two corrections to how this was run, so the next session does not repeat them:
`?tl` has **no query form** — `*tl` performs the calls it reports, and the load
figures above come from the periodic `TDM1:` line instead. And `POST /wait` was
not used at all here: console replies land in `GET /log`, which keeps history, so
reading the log avoids the arm-before-send race entirely for quick queries.
