# dsPIC33CK silicon findings: defects, measurements, and what only hardware settled

Consolidated 2026-08-03 from `ck_spi_dma_core_audit.md`, `ck_trap_handling.md`,
`ck64mc105_ev88g73a_handoff.md`, `ck_ev88g73a_wm8904_phaseB.md`,
`ck_spi_tdm_feasibility.md` and `ck_feedback_from_ak_starter.md`. Those six files are
gone; this is their surviving content. Progress narration, staged plans whose stages
are all done, superseded file/symbol names and review-round logs were dropped — the
code and `git log` carry that. **What is kept is every finding that a future reader
could not re-derive without re-running hardware.**

Scope: the **shared** HAL (`src/hal_dma/`, `src/hal_spi_i2s_tdm/`, `src/hal_clock/`,
`src/app/app_traps.c`). Hardware used was EV88G73A (dsPIC33CK64MC105 Curiosity Nano);
every defect below is device-independent and was present on DM330030
(dsPIC33CK256MP508) too — that profile had simply never been run.

Primary documents (all in `_datasheets/`): **DS70005399D** CK64MC105 family,
**DS70005349** CK256MP508 family, **DS30009742C** FRM-DMA, **DS70005136** FRM-SPI with
Audio Codec Support, **DS70005255B** FRM-Oscillator, **DS70005517B** CK64MC105
Curiosity Nano user guide.

---

## Part 1 — SPI/DMA transport core, first-silicon audit (2026-07-29)

### The symptom, and why it pointed at the wrong layer

`inst_start()` returned success, `run=1 active=1`, every SPI/DMA/CLC register read back
exactly as programmed — and `blocks=0` forever with all `DMAINTn` status flags at zero.
That reads as "the peripheral never raised its event", which invites a hardware
conclusion (probe BCLK, suspect the board, suspect the part).

It was not a hardware fault. It was **five independent software defects**, four of which
produce this same all-zeros signature, stacked on each other. **The all-zeros signature
is not evidence about hardware; it is what a DMA-fed transport looks like whenever
anything upstream of the first transfer is wrong.**

What broke the deadlock was not a better guess but a **standalone DMA proof**: a RAM→RAM
transfer on a spare channel, software-triggered (`DMACHn.CHREQ`), no peripheral in the
loop. It found defects 2 and 3 in single flash cycles and removed the need for an
oscilloscope session entirely. It is kept as a permanent boot-time check.

### Defect 1 — `DMAINTn.CHSEL` was set to the CPU interrupt vector number

CHSEL is a **dedicated DMA trigger-source encoding** ("TABLE 10-1: DMA CHANNEL TRIGGER
SOURCES", DS70005399D p.175; DS70005349 §10.4). It is not the IRQ number.

| Instance | Correct CHSEL (RX/TX) | Was | What the wrong value selected |
| --- | --- | --- | --- |
| SPI1 | **0x02 / 0x03** | 9 / 10 | SI2C1 / MI2C1 |
| SPI2 | **0x10 / 0x11** | 29 / 30 | PWM Generator 2 / 3 |
| SPI3 (MP508 only) | **0x62 / 0x63** | 59 / 60 | ADC Done AN19 / AN20 |

Channels were armed against peripherals that were never enabled, so no trigger arrived.
Verifying "9 = SPI1RX" against the `.PIC` interrupt table *confirmed the wrong table*: 9
**is** SPI1RX as a CPU vector (`IFS0[9]`), which is what let the error survive review.
AK kept the two numberings apart deliberately; the CK port collapsed them into one number
and documented the collapse as fact (`// DMA CHSEL trigger = the peripheral IRQ number`).

### Defect 2 — `DMAL`/`DMAH` were never programmed

DS70005399D §10.1.1: the controller polices Data Space above the SFR range against the
`DMAH`/`DMAL` limit registers — "if a DMA channel attempts an operation outside of the
address boundaries, the transaction is terminated and an interrupt is generated". §10.2
"Typical Setup" lists programming them as **step 2**, before any channel work.

Both reset to **0x0000** (`DMAL` @ 0xAC0, `DMAH` @ 0xAC2). With `DMAH = 0` *every* RAM
address is above the limit, so the first element of every transfer is terminated. Because
the limit test happens **after** the access (DS70005399D, DMAINTn Note 2), `DMACNTn` has
already decremented by one — which is why the channel looked "started and stopped" rather
than "refused".

```
DMA selftest (ch3, RAM->RAM, software CHREQ): FAIL (no DONE: request never serviced)
  DMAINT3=0x0780 DMACNT3=0x0002 dst= 0x0000 0x0000 0x0000 0x0000
```
`0x0780` = CHSEL 0x07 + **HIGHIF**. One element attempted, none delivered, channel dead.

The HAL had asserted the opposite — *"CK has no DMALOW/DMAHIGH window to program (the
reset address window already covers RAM)"*. Both halves were wrong, and being written as
settled fact meant nothing re-checked it.

Fix: `global_init()` programs `DMAL`/`DMAH`, default `0x0000`/`0xFFFF` — fully permissive,
because the hardware's own fixed check against the top of data RAM still applies and the
RAM top differs per part, so hardcoding one part's top would forbid the other's upper RAM.
Overridable via `DSPIC33CK_DMA_ADDR_LIMIT_LOW/HIGH`. `dspic33ck_dma_global_is_ready()`
now also requires `DMAH != 0`, so a channel that cannot transfer is refused at config time
instead of reporting a successful start.

### Defect 3 — `DMACNTn` was programmed as `count - 1`

`DMACNTn` is the transfer count itself. DS30009742C §4.4.2: reset value `0001h`, and a
single One-Shot transfer decrements it to `0000h`. A 4-element request moved exactly 3:

```
DMA selftest: FAIL (DONE but data mismatch)  DMAINT3=0x0730 DMACNT3=0x0000
  dst= 0xA500 0xA501 0xA502 0x0000
```

**This is the only defect here that does not announce itself.** It transfers, it sets
DONE, it looks healthy — it is simply one element short every time, which on a ping-pong
audio buffer is a permanent phase slip.

### Defect 4 — `ENHBUF = 1` is not permitted with DMA

DS70005136, `SPIxCON1L` footnote 5, stated twice: "**SPI operates with DMA in Standard
Buffer mode only, ENHBUF = 0.**"

Beyond being out of spec it also breaks the trigger semantics the transport relies on. In
Enhanced Buffer mode `SPIRBF` means "receive FIFO **completely full**" and `SPITBE` means
"transmit FIFO **completely empty**" (DS70005136 §3.2.2), so `SPIRBFEN`/`SPITBEN` — enabled
as the DMA trigger events — fire at the FIFO endpoints rather than per element. In Standard
Buffer mode the same two bits carry exactly the per-element meaning a DMA needs.

### Defect 5 — a 32-bit slot cannot be fed by two writes to `SPIxBUFL`

DS70005136 §3.2: "If data length is greater than 16, then **both SPIxBUFL and SPIxBUFH
have to be used**"; "always write SPIxBUFL first and then write SPIxBUFH". §4: the most
significant byte "must be written last to the SPIxBUFH, as **writing the Most Significant
bit … triggers a change in the pointers of the transmit buffer**".

The FIFO push is triggered by the `SPIxBUFH` access. Writing `SPIxBUFL` twice never
pushes, so the module never has a complete word to shift. Hardware matched exactly — the
TX channel triggers, moves **one** element, stops forever:

```
DMACH1=0x0245 DMAINT1=0x0301 DMACNT1=0x03FF   (CHEN=1, CHSEL=3, no error flags)
SPI1STATL=0x00A8                              SPITBE=1, SPIRBE=1, SRMT=1, SPIBUSY=0
```

Chain: one halfword reaches `SPIxBUFL` → no FIFO push → nothing to shift → no BCLK →
`SPITBE` never returns 0 and so never produces another 0→1 edge → the trigger never
re-asserts. **Deadlocked with every register looking correct** — precisely the signature
read as a hardware fault. The CK DMA cannot paper over this: `DMACHn.SIZE` is one bit
(byte or 16-bit word), so a 32-bit element is unavailable on the 16-bit core.

Fix: `ENHBUF` cleared, `MODE16 = 1` / `MODE32 = 0`, `WLENGTH = 0`, `FRMCNT` derived from
**wire words** (`slots_per_fs × 2`) instead of slots.

**Two consequences, stated rather than absorbed silently:**

- **TDM32 (32 slots/FS) is no longer representable.** 32 slots × 2 wire words = 64 and
  `FRMCNT` stops at 32. `configure()` rejects it and `conf.h` `#error`s. TDM4/8/16
  unaffected; 16 slots × 32 bit is the widest this part can frame. Recovering TDM32 would
  need 16-bit slots, which the rest of the HAL does not support.
- **I2S `FS_50PCT` now produces a 25%-duty FS, not 50%.** `FRMSYPW = 1` is one *wire word*
  = 16 BCLK out of a 64-BCLK 2-slot frame; it was genuinely 50% while `MODE32` made one
  wire word equal one slot. The fix is the CLC half-frame-marker route the TDM case uses.
  **Not implemented** — an unverified I2S path would be one more untested claim. I2S
  `FS_PULSE` and every TDM shape are unaffected.

### The withdrawn defect 6 — `DONEIF` *does* set in Repeated One-Shot on this silicon

Kept because the reasoning that produced it was wrong twice in the same direction, and the
shape of that error is more useful than a shorter document.

The transport runs `TRMODE = Repeated One-Shot` and detects the second buffer half from
`DONEIF`. DS30009742C §6.1: "**Note that DONEIF remains cleared (= 0) when any Repeated
Transfer modes are being used.**" Taken at face value that makes the second-half detection
impossible, and the first audit concluded exactly that — and further concluded stage C
would need One-Shot re-arming inside one word time (~1.3 µs at 12.288 MHz).

External review pointed at two other statements in the same FRM: `HALFEN` ("0 = An
interrupt is invoked **only at the completion** of the transfer" — completion interrupts
either way), and Example 4-2's annotation "`DMACNT` reloaded to 4, **`DMA0IF=1`**". It
correctly identified that the audit had read §6.1's statement about the `DONEIF` *flag* as
a statement about the *interrupt*, predicting an interrupt that arrives flagless.

Both predictions were then **measured** rather than adopted — `DMACH2 = 0x0215`, i.e.
`TRMODE = 01` and `RELOAD = 1` read back from the register, at 12.5 MHz BCLK:

```
evt 0: DMAINT2=0x0211  HALFIF=1 DONEIF=0  DMACNT2=0x0010  DMADST2=0x2138  (base+half)
evt 1: DMAINT2=0x0221  HALFIF=0 DONEIF=1  DMACNT2=0x0020  DMADST2=0x2118  (base, reloaded)
... repeating: HALFIF=4  DONEIF=4  flagless=0  OVRUNIF=0 over 4 cycles
```

**Two interrupts per buffer cycle, and the second one sets `DONEIF`.**
`dspic33ck_dma_half_from_status()` maps `HALF`→first half and `DONE`→second, which is
exactly this. The ping-pong keying was correct all along.

Two readings of the FRM, one a correction of the other, and the hardware agreed with
neither. What §6.1 asserts is not observable on this part in this configuration — and no
amount of further reading would have established that, because both readings were
defensible from the text. **The lesson is not "read more carefully"; it is that a claim
about what silicon does is only ever settled by the silicon.** Six lines of ISR logging
answered in one flash cycle what two rounds of document analysis could not — and would
have prevented an unnecessary re-architecture had either reading been believed.

### Two measurements worth keeping from the qualification stages

**16-bit slot geometry is right, halfword order is not verifiable by loopback.** Framed
TDM8, asymmetric pattern, both directions matched (`A1B20101 A1B20202 …`), addresses
advanced by exactly 0x40 — so 32-bit slot alignment and `FRMCNT = 4` are confirmed. But a
loopback returns whatever order it was given (`rx_wire[i] == tx_wire[i]` holds whether MSB
or LSB half goes first), so **no asymmetric pattern can distinguish the two**; an earlier
version of the audit claimed it could. Settled later by scope, below.

**`FRMERR = 0` is corroborating, not proof.** This SPI is the frame host, generating FS
from its own internal counter, so `FRMERR` is not an independent check on the FS waveform
— a self-consistent host has little opportunity to report a frame error. An earlier
version called it the load-bearing result, which overstated it.

**The rate check, which is the part worth doing.** 12.5 MHz BCLK ÷ 256 BCLK/frame = 48828
frames/s ÷ 32 frames/block = **1526 blocks/s expected**; measured 762 blocks per poll
interval with polls **500 ms** apart → **1524 blocks/s**. Read as 1-second polls it looks
like exactly half the expected rate, i.e. a convincing "every reload-boundary block is
dropped" — and the arithmetic, not the core, was wrong. **A factor of exactly two is just
the sort of coincidence that earns a confident wrong diagnosis.**

### Halfword wire order — settled by scope (2026-07-30)

Two independent tests on a 1 kHz sine TX generator:

1. Sine confined to the low 16 bits, `0x5A5A` OR'd into the high 16 → the `0x5A5A` marker
   read **first** in time.
2. Fixed near-complementary halves (`high=0x7FFE`, `low=0x8001`), content-only, not
   timing-dependent → `0x7FFE`'s wide high-level box read **first**.

Both agree: **the high (MSB) halfword goes out first.** The concern the `hw.c` comment
carried — that CK DMA's linear walk through a little-endian `int32_t` would put the LOW 16
bits out first — did not hold against the SPI shift register's actual behaviour (triggered
by the `SPIxBUFH` access, not the memory layout DMA stepped through). **No halfword swap
is needed anywhere in the shared core.**

### `fs_clc.c` RPORx addressing — a wild SFR write on both parts

Found by inspection before any hardware run, unrelated to defects 1-6. `fs_clc_rpor()`
computed the RPORx register as `(&RPOR0) + (rp - 32) / 2`, assuming RP32..RP79 map onto
`RPOR0..23` contiguously.

| | physical pins | virtual pins (RP176+) |
|---|---|---|
| CK256MP508 | RP32-79 contiguous → `RPOR0..23` | formula said `RPOR24`+48, past the bank |
| CK64MC105 | contiguous only to RP61; `RPOR15/16` = `{RP65,RP72},{RP74,RP77}` | formula said `RPOR17`+55, past the bank |

So `fs_clc_write_rp(RPV0, …)` — the **very first write `engage()` makes** — was a wild SFR
write 48-72 words past `RPOR0` **on both parts, in every previous version of the file**. It
had never run, so nothing had ever exercised it. Fixed with an explicit per-device
`RPORx slot → RPn` table; a pin absent from the table returns `false` rather than writing to
unrelated memory. (This table has since moved into `dspic33ck_pps.c` — the PPS HAL owns the
register map; see `ck_src_layout.md`.)

The header's doc comment also still described AK's CLC10/RPV8/dsPIC33CK512MPS512 model
verbatim while the code implemented CLC1/RPV0/CK64MC105.

### CLC1 50%-duty FS, measured

```
SPI1CON1H=0x30A3   FRMCNT=3  FRMSYPW=0  FRMSYNC=0  FRMPOL=1  FRMEN=1
CLC1CONL=0x8086 -> 0x80C6 -> 0x8086 ...  (LCEN=1 LCOE=1 MODE=110, LCOUT alternating)
CLC1CONH=0x000A   G1POL=0 G2POL=1 G3POL=0 G4POL=1   (CLK=Gate1, J=K=1, R released)
```

`FRMCNT = 3` (not the full-frame `4`) confirms the half-frame-marker geometry took: a
marker every `slots_per_fs/2 = 4` slots = 8 wire words = log2(8) = 3, half the frame
cadence, which is what a J-K toggle needs to reconstruct a 50%-duty FS.

`LCOUT` read different values on polls seconds apart, which **rules out a frozen
flip-flop but is not proof of a stable, correctly-phased waveform** — 500 ms polls cannot
resolve 48 kHz. All three datasheet assumptions the file flagged as HW-to-verify are now
confirmed: `MODE[2:0]=110` is Register 21-1's "JK flip-flop with R"; `DS1[2:0]=000` is
Register 21-3's "CLCINA I/O pin"; Table 8-4 lists RP176 as `Virtual RPV0`, a valid
`CLCINAR` source.

### CLOSED (2026-08-03): FS-vs-data alignment is correct; the reported shift was an artifact

For one day this section carried an open question — a scope showed the ramp data shifted
relative to FS, and the leading hypothesis was the J-K divide-by-2's inherent phase
ambiguity: markers arrive every 4 slots and the FF toggles on each, so nothing binds
Q-high to the *frame-start* marker rather than the mid-frame one. **Measured, that is not
what happens. There is no misalignment.**

What made it measurable was fixing the *stimulus*, not the hardware. The exerciser
transmitted a per-sample monotonic ramp, so **the data carried no frame landmark** — every
slot looked like its neighbours and nothing said "this is slot 1". Replacing it with a
fixed `0x8001_7FFE` **in the first two slots only**, leaving the remaining six silent,
turns the frame into an isolated 64-BCLK burst followed by a 192-BCLK gap. The first
rising edge out of that gap is unambiguously the start of slot 1.

Measured on EV88G73A, TDM8/32-bit master, BCLK 12.5 MHz, both FS shapes:

| | measured | expected | |
|---|---|---|---|
| FS period | 49.21 kHz ≈ 20.3 µs | 256 BCLK = 20.48 µs | ✓ |
| FS duty (`FS_50PCT`) | ≈50% (10.2 µs high / 10.2 µs low) | 4 slots high / 4 low | ✓ J-K working |
| data burst width | ≈5.15 µs | 2 slots = 64 BCLK = 5.12 µs | ✓ |
| **FS rising edge → burst start** | **≈60–80 ns ≈ 1 BCLK** | conventional 1-bit delay | ✓ |
| burst position within FS | **start of the HIGH half** | slots 1–2 = start of high half | ✓ |

The 1-BCLK gap is the **standard I2S/TDM 1-bit delay**, which `SPIFE=0` is what produces —
it is the convention, not a defect. A mid-frame J-K lock would have put the burst in the
FS **LOW** half: a half-frame displacement of 10.2 µs, impossible to mistake for this.
**10 consecutive `*sr` resets all produced the identical picture**, so the J-K startup
phase is deterministic *and* correct. (The earlier 10-reset test could not say this: with
the landmark-free ramp, "correct phase" and "mid-frame phase" were indistinguishable, so
10/10 established only that the lock was not random. It is now known to be both.)

`FS_PULSE` (CLC entirely out of the path, `FRMCNT` 3→4, FRMSYNC straight out on RP51) was
measured first and shows the same correct 1-bit delay. **So both legs are right**, and the
A/B's known confounder — CLC on/off and the `FRMCNT` cadence change together — never had
to be resolved, because neither leg is broken.

**The FS framing needed no change** — not in the CLC configuration, and not in the
frame-phase bits. Note how narrow that claim is: it is about FS and the slot *boundary*, and
nothing more. Part of the reported *appearance* of a shift was an observation artifact — the
one candidate the handoff note listed and could not eliminate: the ramp carried no frame
landmark (as above), and the capture was at 1.0 µs/div, **under half of a 20.5 µs frame**,
which cannot resolve a 4-slot offset even in principle.

**But "FS is aligned" is not "nothing was wrong", and this entry must not be compressed into
that.** The same jig immediately found defect 7 below: the two 16-bit halves of every 32-bit
slot went out in the **wrong order**. That is a 16-BCLK displacement *inside* each slot, and
against a landmark-free ramp it would look very much like "the data is shifted relative to
FS" — a live candidate for what the original observer was actually seeing. The framing was
never the problem; the slot contents were. The report was not mistaken.

> **The lesson is about the stimulus, not the part.** Three sessions reasoned about J-K
> phase ambiguity, `FRMCNT` arithmetic and `SPIFE`/`CKP`/`CKE` from a waveform that was
> incapable of showing the answer. A test pattern with no landmark is not a weak
> measurement, it is an **unfalsifiable** one: every hypothesis survives it. The fix was
> one line in a demo's TX callback. Before suspecting silicon, check that the stimulus can
> distinguish the outcomes being argued about.

**Do not reintroduce the ramp** in this exerciser without also restoring a landmark; and
note that a *constant* pattern in *every* slot is equally landmark-free — with
`0x8001_7FFE` everywhere, a 4-slot frame reads as 8 near-identical 16-bit wire words
(bit31 and bit16 each give a 1-BCLK spike), which is how "the data is coming out 8
channels wide" gets misread off a 4-slot frame. The silence in slots 3-8 is what makes the
burst legible.

### DEFECT 7 — FIXED (2026-08-03): the DMA emitted the LOW half-word first

Found while investigating FS alignment, measured with the same jig, and **fixed and
hardware-verified the same day** — the fix is in the buffer element's type; see below.

`dspic33ck_spi_i2s_tdm_hw.c` carried a block headed *"SETTLED: the HALFWORD ORDER within a
slot (scope, EV88G73A, 2026-07-30)"* asserting that the high (MSB) half-word goes out first
and **no half-word swap is needed anywhere**. **That is the opposite of what the wire does.**

The naive prediction it argued against is the correct one: dsPIC is little-endian, the DMA
walks ascending addresses moving 16-bit elements into `SPIxBUFL`, so **the LOW 16 bits of
each `int32_t` reach the wire first** — backwards from the MSB-first convention a TDM/I2S
wire expects. Every 32-bit slot goes out with its two halves exchanged.

**Why it survived.** A loopback cannot see it: SDO→SDI returns whatever order it was given
and RX un-swaps it symmetrically (stage B said as much). It only bites against a real codec,
and no codec has ever been attached to EV88G73A. Against one, every sample is garbage.

**Evidence.** `DEMO_TDM_TX_PATTERN = 0xFFFF0000` in slots 1-2, rest of the frame silent. The
halves are all-ones and all-zeros, so the orders predict opposite openings:

| order | predicted opening of the 2-slot burst | |
|---|---|---|
| high half first | 16 BCLK **high** immediately, then low | not observed |
| low half first | 16 BCLK **low**, then the high block | **← observed** |

Measured at 12.5 MHz BCLK: low for ≈1.27 µs (≈16 BCLK) from the FS edge, then high 16, low
16, high 16, then silence. Two independent captures.

**It is a swap inside the slot, not a 16-BCLK delay of the stream** — for a single slot the
two are indistinguishable, so the discriminator is where the burst *ends*: ≈64 BCLK
(≈5.17 µs) after the FS edge, exactly two slots. A global 16-BCLK delay would run to FS+80
and spill into slot 3. So the FS-to-slot-boundary framing is correct (see the FS-alignment
entry above); the fault is entirely within the slot.

**This is the other half of defect 5, and it was left behind by that fix.** Defect 5 forced
`MODE16`, making a 32-bit audio slot two *independent* 16-bit wire words whose order is
whatever order the DMA reads memory in. Nothing then re-established the wire's MSB-first
convention over that pair — the fix corrected `FRMCNT` (cadence, in wire words) and stopped
there. **Letting the hardware order the halves is not available:** per defect 5,
`DMACHn.SIZE` is one bit (byte or 16-bit word), so a 32-bit DMA element does not exist on
this core, and `MODE32` cannot be DMA-fed at all.

**CK-specific — do NOT port the fix to AK/sonora.** dsPIC33AK drives its SPI in `MODE32`
with a single 32-bit `SPIxBUF` and 32-bit DMA elements (`..._DMA_SIZE_WORD`), with the DMA
count in samples. An `int32_t` goes out as *one* element there and is never split, so the
AK HALs have no equivalent problem and need no equivalent type. This is a **porting defect
introduced by CK's `MODE16` + `SPIxBUFL` + two-16-bit-DMA-elements arrangement**, not a
family-wide one.

### The fix: the buffer element is a type that names the wire

Rather than swap halves somewhere, the DMA buffer stopped being `int32_t`:

```c
typedef struct { uint16_t wire[2]; } dspic33ck_tdm_wire_slot_t;  /* wire[0] goes out first */
_Static_assert( sizeof(dspic33ck_tdm_wire_slot_t) == 4u, ... );
```

`dspic33ck_tdm_slot_encode_s32()` / `_decode_s32()` convert, and a `union` lives *inside*
those inline helpers as an optimisation detail — never in the public type. **`dst[i] =
sample;` is now a compile error**, which is the point: the defect cannot recur in the shape
it took.

**Why conversion happens at the DSP's own load/store, not in a HAL pass.** Measured, `-Os`,
loop-body instructions per sample, real header and real helpers:

| direction | plain `int32_t` | wire slots | delta |
|---|---|---|---|
| store (encode, folded into a DSP store) | 17 | 18 | **+1** |
| load (decode, folded into a DSP load) | 9 | 11 | **+2** |
| combined decode → gain → encode | 26 | 29 | **+3** |
| raw passthrough (`dst[i] = src[i]`) | 9 | **9** | **±0** |

Passthrough compiles to the identical `mov [w4++],[w3++]` memory-to-memory pair as before:
two slots already in wire order copy verbatim. A *separate* swap pass over the block would
have cost 3-4× the above, which is why the helpers are placed where the data is already being
touched.

**The instruction counts are measured; the microsecond figure below is arithmetic from them,
not a hardware timing.** +3/sample × 256 samples per block ≈ 768 instructions ≈ 7.7 µs at
100 MIPS against a 655 µs block period, so **≈1.2 % of the block period** — assuming one
cycle per instruction, which is optimistic for the `mov`s. To measure it for real rather than
derive it, read `dspic33ck_spi_i2s_tdm_inst_get_status()`'s `load` (the block-ISR monitor,
reported in 0.1 µs units) with and without the conversion in a build that actually exercises
the gain path. That has not been done: the loopback demo's own `load` is not a valid A/B here
because the pre-fix and post-fix demos differ in more than the conversion.

**Deferred by decision (2026-08-03).** Nothing rides on the real number today, and +3
instructions per sample is not where a margin problem will come from. Revive this
measurement when the block-ISR margin actually becomes a close call — at that point it is
the first thing to measure, and the recipe below is what makes it cheap to restart.

**How to reproduce the counts** (no probe code is kept in the tree — this recipe replaces it,
because defect 7's four-day life came from exactly the opposite choice: a confident
measurement whose evidence file had been deleted). Write a throwaway TU that `#include`s
`dspic33ck_spi_i2s_tdm.h` and defines two loops over N samples doing the same arithmetic —
one on `int32_t*` buffers, one on `dspic33ck_tdm_wire_slot_t*` via the encode/decode helpers
— then:

```
xc-dsc-gcc -S -Os -mcpu=33CK64MC105 -omf=elf -mdfp=<DFP>/xc16 \
           -I src -I src/hal_spi_i2s_tdm -I src/app  probe.c -o probe.s
```

and count instructions in each loop body (label to the backward `bra`). Use the REAL header,
not a copy of the helpers: the point is what the firmware compiles to. Nothing is linked or
run — this is a codegen inspection, not a benchmark.

Two decisions worth keeping:

- **A `struct`, not a one-element `union`, is the public type** — the concern that XC-DSC
  might generate worse code for a struct was checked and is unfounded (store actually
  improved to +1). A shift-based decode (`(src[0] << 16) | src[1]`) is the formulation to
  avoid: it compiles to `sl`/`clr`/`ior` at **+6**, 3× the union-based helper.
- **`app/gain_ctrl.c` was NOT converted.** It is a fixed-point port of sonora's float
  `gain_ctrl` with zero HAL dependencies — the reason it could leave `boards/ev88g73a/` at
  all — and coupling it to one part's DMA layout would undo that. It keeps its `int32_t`
  signature; `gain_ctrl_next_frame_gain()` was added so a caller that must traverse wire
  slots itself can still get the per-frame ramp value, and `wm8904_audio.c` owns that loop.

**Acceptance is a scope reading, not the loopback.** With `DEMO_TDM_TX_PATTERN=0xFFFF0000`
in slots 1-2 the burst must open with **16 BCLK high** immediately after FS, where before the
fix it opened with 16 BCLK low; burst position and length are unchanged (FS+0 to FS+64). A
loopback matches either way — TX and RX swap symmetrically — so passing it proves nothing.

**How this one was nearly re-buried, twice in one day.** First, the old note's confident
"SETTLED" was believed for four days despite citing a mechanism — "the `SPIxBUFH` access" —
that **does not occur in the code** (`spi_buf` is `&SPIxBUFL`, the sole DMA port) and
evidence in `ev88g73a_tdm_master_loopback.c`, a board-private demo file since deleted.
Second, on re-measuring, this session read a `0xFFFF0000` trace while believing the board
still ran `0x8001_7FFE`, mapped the real "opens with 16 BCLK low" onto the wrong bit layout,
and wrote *"VERIFIED: HIGH half-word first"* into the HAL — the same wrong answer as the note
it had just discredited. It was caught by checking the **boot banner**, which now prints the
pattern (`TX 0x%08lX in slots 1-2`) precisely so a trace can never be matched to a
misremembered stimulus. **State the stimulus in the artefact, not in your recollection.**

### Method note — what actually did the work

1. **Read the primary document for the actual part, not the family memory.** Four of the
   five defects were single lines in a data sheet or FRM. The wrong values were all
   plausible-looking, and three carried a confident source comment asserting they were
   right. **Comments are not evidence.**
2. **Prove one layer in isolation before reasoning about the stack.** The DMA self-test
   turned "the transport does not stream" into two answered questions in two flash cycles
   and replaced a deferred oscilloscope session entirely.
3. **Measure a documented behaviour instead of adopting a reading of it.** See the
   withdrawn defect 6.

And one caution earned the hard way, repeatedly. Defect 1 existed because a comment
asserted something the code then relied on, and the first version of the audit reproduced
that same shape three times over (`main.c` described gating it did not perform; a stage
claimed a derived `FRMCNT` while writing `4u`; the trigger fields still said "peripheral
IRQ number" after the values were corrected). Stage D found a fourth in the opposite
direction: an unexercised address formula (`(&RPOR0) + (rp-32)/2`) is itself a
comment-shaped claim that nothing had checked. **A comment that disagrees with its code is
not a documentation problem, it is the mechanism by which the next defect gets believed** —
and an unexercised formula is that same mechanism wearing code's clothes.

---

## Part 2 — Clock: two defects behind "230400 is garbled"

The console was garbled at every host baud rate. Two independent defects, both in the
clock rather than the UART.

**1. 230400 is unreachable at the FRC boot clock.** At Fcy = 4 MHz the 16× baud divisor
for 230400 rounds to 1, giving 250000 baud: +8.5%, far outside the ~2% a UART frame
tolerates. No divisor lands near 230400 at 4 MHz (next is 200000, −13%), so Fcy had to
rise — FRC → PLL to Fosc 200 MHz / Fcy 100 MHz, where 230400 resolves within half a
percent.

**2. The PLL formula was missing a fixed divide-by-2**, so the part ran at exactly half
the intended speed. Per DS70005255B Figure 1-2 the PLL output `FPLLO` passes a fixed `/2`
before the clock-switch mux: `Fosc = FPLLO/2`, `Fcy = FPLLO/4`. The FRM confirms it by
capping FPLLO at 400 MHz and calling that 100 MIPS. The HAL computed `Fosc = FPLLO`, so it
recorded 100 MHz Fcy while the silicon delivered 50 MHz — and every derived baud divisor
was consequently 2× wrong.

> **This failure is quiet, which is what made it expensive.** `OSCCON.LOCK` reads 1 and
> `COSC` shows FRC+PLL, because the PLL genuinely *is* locked. Only the frequency is
> wrong. **Do not treat LOCK as evidence of the right frequency.**

Also corrected: `solve_pll()` returned the *first* exact solution, landing hard on the
minimum legal Fvco (400 MHz). It now returns the **highest** Fvco, matching Microchip's
practice and leaving margin — for 200 MHz from an 8 MHz FRC that is PLLPRE 1 / PLLFBDIV
200 / POST1DIV 2 / POST2DIV 2, i.e. Fvco 1600 MHz. The PLL limits were all verified
against DS70005255B §8.0 and were correct; the one **missing** constraint has been added:
`FPFD` must not exceed `Fvco/16`.

### `PLLKEN = OFF` is intentional

`PLLKEN = ON` gates the PLL output away when lock is lost, which stops the core with no
clock and no way to report why. This profile instead checks `OSCCON.LOCK` in software and
falls back to the FRC, and **that fallback can only run if the hardware has not already
removed the clock.** The console baud follows the outcome: 230400 on target, 9600 on the
FRC fallback (within 0.2% at both 4 MHz and 100 MHz), so a failure stays readable.

Trade-off left open: `PLLKEN = ON` additionally covers *losing* lock at runtime, which
software polling cannot catch. Decide per application.

### DM330030's own version of the same failure class (investigated 2026-07-29, no board)

`FCKSM = CSDCMD` disables clock switching, so `CLOCK_Initialize()`'s FRC→PLL switch is a
**silent no-op on real hardware**: `OSCCONbits.OSWEN` never sets, the wait-clear loop
passes immediately, and the call returns `DSPIC33CK_CLOCK_OK`. `g_fosc_hz` is then
unconditionally set to the intended 200 MHz, so `get_fcy_hz()` reports 100 MHz while the
silicon almost certainly never left the boot-time primary oscillator. **Same class as the
EV88G73A bug — software believes a clock the hardware never reached** — but by a blocked
switch rather than a wrong formula.

The crystal's exact frequency is unconfirmed: `XTCFG = G3` only bounds it to 24-32 MHz and
no `#define` in the repo states it. When a board arrives: decide primary-direct (measure
the real Hz) vs FRC→PLL (`FCKSM = CSECMD` to unblock switching), then fix two `FCY` macros
(`src/main.c`, `board_components/wm8904_port.h`, both `100000000UL` and only correct if
the switch takes effect) and hardware-verify with the same LOCK-check approach.

---

## Part 3 — Traps: three findings that only measurement produced

Full design rationale (policy split, the two halves, persistent latch) is in
`ck_src_layout.md`. What is kept here is what hardware taught.

### 1. A misaligned read inside valid RAM does not trap on this core

The first attempt read a misaligned word inside valid RAM and the board carried straight
on. Misalignment is not what this core faults on; **an address with no memory behind it
is**. RAM on CK64MC105 is `0x1000..0x2FFF`, so `0x3000` is the first non-existent address.
The trap ID string therefore reads `ADDRESS ERROR (unimplemented address)` and deliberately
does not promise misalignment detection — it said "bad or misaligned access" until this was
measured, which was a promise the hardware does not keep.

### 2. A stack overflow is structurally unreportable by software

`*xs` never reaches `_StackError`. W15 is past SPLIM, so the trap handler's **own context
push overflows too**; the hardware sees a trap within a trap and resets immediately,
setting `RCON.TRAPR`. **No software handler can report a stack overflow, because a handler
needs stack to run.**

So the `RCON.TRAPR` branch is the **only** evidence for that entire fault class — genuinely
load-bearing, not a duplicate of the latch. Verified: after `*xs` the trap counter did
**not** advance (no handler ran), which is the correct behaviour and is why `?xl` reads 2
rather than 3 after `*xm`, `*xa`, `*xs`.

### 3. The repeat counter read 1 forever (defect, fixed)

Three successive forced math traps read `traps since power-on: 1` every time. Cause: the
counter shared one magic word with the record. `traps_clear()` zeroed that magic so a
consumed record cannot be reprinted — and the next handler then read "no valid record",
reset the count to 0, and reported 1 again. **The counter's own comment claimed it
survived; clearing the shared magic destroyed the only evidence that it was valid.**

Fix: a **second magic** guarding the counter alone, which `traps_clear()` leaves untouched.
The two fields have different lifetimes — the record is consumed per report, the count is
per power-on — so they need separate guards. `report_previous()` also handles `POR`
unconditionally and first, since a power-on leaves that RAM undefined and random RAM
matching the count magic would carry a garbage count across a genuine power cycle.

Verified: 1, then 2/3/4 with `<- repeating, so this is deterministic`, including across a
change of trap type. And the POR pairing that makes it conclusive:

| Reset | `RCON` | count before → after | Meaning |
|---|---|---|---|
| `*sr` warm reset | `0x0040` (SWR only) | 3 → **3** | persistent RAM survives `reset` |
| flash boot | `0x0083` (POR+BOR+EXTR) | 3 → **0** | POR branch clears the counter |

The same mechanism preserves across a warm reset and clears across a POR, so it
discriminates on the actual `POR` bit rather than being wiped by every restart.

### All eight vectors, not MCC's four

MCC generates four (`_OscillatorFail`, `_AddressError`, `_StackError`, `_MathError`). This
family has **eight**: `_HardTrapError`, `_SoftTrapError`, `_ReservedTrap5`,
`_ReservedTrap7` were falling through to the default vector on *both* boards — and worse
than silently, since not even a flag gets cleared. Both linker scripts declare all eight
(verified by grep on `p33CK64MC105.gld` / `p33CK256MP508.gld`, not assumed from one part),
each at its own vector address in the map.

### `EXTR(MCLR)` in the banner does not mean "not a power-on"

Measured: `?sr` reported `EXTR(MCLR)` with `RCON=0x0083` — **POR + BOR + EXTR all set at
once**. The cause string reports a single cause by priority, so a genuine power-on that
also asserts MCLR is announced as MCLR. **The banner's cause string cannot distinguish a
power-on from an MCLR reset**; read raw `RCON` bit 0. An earlier conclusion here — that
`EXTR(MCLR)` flash boots were therefore not power cycles — was drawn from the string alone
and was unsound. The trap code itself tests the `POR` **bit** directly, which is why it
behaved correctly throughout.

---

## Part 4 — Two diagnostic traps that each cost a session

Recorded because they will happen again, and because both are cases where **the
instrument, not the firmware, was lying.**

### A USB re-plug does NOT power-cycle the target

The first POR attempt looked like one and was not. Proof is the block counter: it ran
**continuously from 198317 to 656075** across the unplug with `run,act=(1,1)` throughout —
about 7.2 minutes of unbroken runtime spanning the moment the cable was pulled. Only the
debugger re-enumerated. A *long* disconnection (~30 s, so the rail actually collapses) does
produce a POR, visible as the counter dropping back to ~3007.

**Do not infer a reboot from a low counter value alone** — a low value is also just an early
point on a continuous climb, which is exactly how this was misread once. Look for the
**drop**.

### A POR banner cannot be captured by a monitor that must reopen the port

The board boots and prints faster than the host enumerates the CDC. Measured: 12 seconds of
`open COM5 failed` retries, then `connected COM5`, and the first line captured is already
`blk=3007` (~2 s of runtime). The banner went out while no port existed. The banner is
**not** always lost — flashes that kept the port open throughout came through intact; the
difference is whether the host re-enumerates the CDC, which is not under our control.

Consequence: verifying anything printed at POR needs either a terminal holding the port
across the cycle (opened *before*), or a console command that reports on demand. The latter
was cheaper and exists: `?xl` prints `traps since power-on` alongside the id, which is what
made the POR table above measurable at all.

### The "dead RX after POR" scare was a debugger fault, not firmware

**Symptom:** after a power cycle the console stopped answering. Three commands got no
reply. TX looked fine — telemetry streamed and the monitor logged `>> ?sr`, so bytes reached
the *port*. The board was healthy. The console had answered on this exact image half an hour
earlier, so it looked like RX/PPS init that survives a warm `reset` but not a cold start.

**It was not.** Reflashing the *identical* image restored RX immediately; the firmware never
changed, so the firmware was never at fault. The Nano's debugger came back from
re-enumeration **partly working**:

| Debugger interface | State |
|---|---|
| Mass storage (drag-and-drop) | works |
| CDC device→host (telemetry) | works |
| CDC host→device (console input) | **broken** |
| Programming / MCLR reset | **broken** |

The two broken paths are the tell. `reset_pkob4.exe` printed `Initialization failed: Failed
getting emulation database information` and then **`Operation Succeeded` with exit code 0**
— while the block counter climbed straight through, proving no reset happened. **Do not
trust that tool's exit code alone**; confirm a reset by watching the counter drop. Recovery
is a drag-and-drop flash, which goes through the one interface that still worked.

**Diagnostic rule: before suspecting firmware RX, reflash the same image. If RX returns with
unchanged code, it was the debugger.** A monitor that logs `>>` proves only that bytes
reached the port, never that the CDC delivered them.

### Tooling note

The stack-overflow test wedged the Nano's USB completely once — no CURIOSITY drive, no CDC,
nothing with `VID_03EB` present. Recovery is a cable re-plug, and the CDC may return on a
different COM number.

---

## Part 5 — ISR cost and optimization, measured

### The load monitor had never worked on any CK board

`dspic33ck_high_res_timer` was a **Timer2/3 paired-32-bit** implementation carried from AK,
gated on `#if defined(T2CON) && defined(T3CON) && defined(TMR3HLD)`. **Neither CK part has
any of them** — dsPIC33CK has Timer1 plus the CCP modules, so the gate was 0, `init()`
returned `ERR_NOT_PRESENT`, and `get_load()` reported zeros on **both** CK boards from the
start. The demo discarded the init return, so nothing said so. `load last=0 max=0` was
**absence wearing a measurement's clothes.** Rebuilt on **SCCP1** as a plain 32-bit Fcy
counter.

### Optimization levels, measured on hardware

Loopback config, TDM8 at 48.83 kHz, 8-slot sine callback. Every build in this repo had been
`-O0`.

| level | ISR time | load | program bytes | note |
|---|---|---|---|---|
| `O0` | 136.5 µs | 20.8% | 53,676 | what every build here used to be |
| `O1` | 40.6 µs | 6.1% | 41,058 | |
| `O2` | 47.5 µs | 7.2% | 43,530 | **slower than O1** |
| `O3` | **16.5 µs** | **2.5%** | 57,429 | fastest, largest |
| `Os` | 52.7 µs | 8.0% | **36,555** | smallest |

`miss=0` at every level.

- **The ordering is not monotonic.** `O2` is measurably slower than `O1`. Whatever `O2`
  enables beyond `O1` costs more than it buys on this 16-bit core for this code, so
  **"higher is faster" cannot be assumed here** — it has to be measured.
- **`O3` is 8.3× faster than `O0`**, almost certainly by unrolling the sine-table loop, and
  pays for it in flash.
- The free-licence claim recorded earlier ("only `O1` is available") was **wrong**; the
  licence in use is full and `O2`/`O3`/`Os` all build.

Default is **`Os`** for this repo: CK64MC105's 64 KB is the binding constraint (the WM8904
config had reached 90% under `O0`) and 8% ISR load is ample headroom — the switch takes that
config from **90% to 63%**. **`O3` is the recommendation for the production part,
CK256MP508**, where flash is not a constraint and the 8.3× speedup is free; recorded rather
than applied because this repo's default build is the CK64MC105 board.

### `___muldi3`: a typing decision costing 88 cycles per sample

The Q31 gain path's cost is not in the loop — it is one library call, present at `-O0` and
`-O1` alike:

```asm
rcall   0x60c <___muldi3>        ; per SAMPLE
```

`___muldi3` is a full **64×64 signed** multiply (a sign-handling wrapper around
`___umuldi3`'s ten `mul.uu` partial products), about **88 cycles per sample** — 63% of the
per-slot cost at `-O0` and 81% at `-O1`. It is there because of the C, not the optimiser:

```c
dst[base + slot] = (int32_t)(((int64_t)src[base + slot] * gain) >> 31);
```

The cast promotes `src` to `int64_t` and `gain` follows, so the compiler is asked for a
64×64 product when **32×32 → 64** is what the algorithm needs. Nothing in `-O1` can fix
that. The fix is a **Q15 gain**: a 300 ms ramp at 48 kHz is ~14,400 steps, so Q15 leaves the
ramp visually identical while turning the scaling into a 32×16 product — two `mul`
instructions and a shift. That changes the gain type's representation, so it is a design
decision, not a unilateral refactor. **Done 2026-08-06 — see the next section for what it
measured.**

### The Q15 hot path: 309.5 µs → 48.4 µs, measured on EV88G73A (2026-08-06)

`-O2`, TDM8/32-bit slave at 48 kHz, 32-frame blocks (256 slots per block, 666 µs period),
same board and same session before and after. Numbers are the `?tq` / periodic `TDM1:`
line's `max=`, i.e. worst block ISR in the window:

(`-O2`, not `-Os`: the EV88G73A configuration was switched from `-Os` to `-O2` in `ff2e13f`,
which this measurement descends from, so **both** columns above are `-O2`. An earlier
revision of this section said `-Os` — wrong, and worth stating rather than quietly editing,
because a per-commit attribution that credits the Q15 change with an optimisation-level
change would overstate it. The 6.4× below is Q15 against Q31 at the same `-O2`.)

| path | before | after | note |
|---|---|---|---|
| `mute` (digital zeros) | 19.9 µs (2.9%) | 19.7 µs (2.9%) | untouched; the fixed overhead |
| `copy` (verbatim) | 21.0 µs (3.1%) | 20.9 µs (3.1%) | untouched; the baseline |
| **`gain` (unity, ramp idle)** | **309.5 µs (46.1%)** | **48.4 µs (7.2%)** | **6.4× faster** |

`miss=0` throughout, both images. Subtracting the `mute` overhead, the scaling work itself
went from **289.6 µs to 28.7 µs — 10.1×**, or 1.13 µs → 0.11 µs per slot (~113 → ~11
cycles at Fcy 100 MHz). Program flash cost **+276 bytes** (49,353 → 49,629, 74% either
way); RAM unchanged at 4,728 bytes.

**Reducing the NUMBER of multiplies was the wrong direction.** On this core a 16×16
multiply is one cycle and a 32-bit ALU operation is two, so the replacement uses *more*
multiplies than the arithmetic strictly needs, and wins by never leaving 16-bit operands:

```asm
mov.w  [w8], w1          ; wire[0] -- MS half
mul.su w1, w0, w6        ; hi = (int16)wire0 * g   (1 cycle)
mov.w  [w8+2], w1        ; wire[1]
mul.uu w1, w0, w2        ; lo = wire1 * g          (1 cycle)
lsr.w  w3, #0x0, w2      ; lo >> 16 == naming the high word: FREE
mov.w  #0x0, w3
add.w  w6, w2, w2 / addc.w w7, w3, w3
add.w  w2, w2, w2 / addc.w w3, w3, w3   ; << 1
mov.w  w3, [w9-2] / mov.w w2, [w9]      ; MS half out first (defect 7)
```

`x * g / 2^15 = 2·(x_hi·g) + (x_lo·g)/2^15` is exact; only bit 0 of the 32-bit slot is
dropped, and the codec word is 24-bit left-justified inside it, so bit 0 is not an audio
bit. `g == 0x8000` therefore reproduces the input bit for bit — which is the point, because
**unity is 100% of the running time** and the gain stage is now the default path, i.e. the
measurement subject rather than a thing to be avoided. `g == 0` is a true digital zero with
no branch. The gain is fetched **once per block** (`rcall` per block, not per sample) and
lives in `w0` across the loop, which is what makes the per-slot cost 11 cycles.

**The same change removed a divide from the audio ISR.** The linear ramp needed
`gain_diff / ramp_samples` inside `gain_ctrl_mute_set()`, and `mute_set()` is called *from
the block callback* (that is where SW0's level is applied) — so a `___divdi3` ran on
exactly the block where the button was pressed. The exponential curve is
`state ± ((state >> sh1) + (state >> sh2))`: shifts and adds, no multiply, no divide, and
the ms→curve arithmetic happens once in `gain_ctrl_init()`. Constant-dB per unit time is
also the better curve for a mute ramp. 300 ms asked → **299 ms** delivered after snapping
to the nearest shift pair (`(6,7)`; the four available at this rate are ~180/299/358/594 ms).

**Verification gate, corrected.** "No `___muldi3`/`___divdi3` in the `.map`" cannot be the
check: `dspic33ck_clock.c` already links both in. The
usable check is **per object** — `xc-dsc-objdump -d` of `gain_ctrl.o` and `wm8904_audio.o`
shows no `call`/`rcall` to any `___*di3` at all.

**The ramp itself is nearly free, measured over 12 SW0 presses.** The press is applied in
the block callback, so a press block is the worst block of its window:

| | ISR max | margin |
|---|---|---|
| steady (unity or muted, ramp idle) | 48.4 µs (7.2%) | 622.7 µs |
| window containing a press + its 299 ms ramp | 48.8–48.9 µs, worst **50.3 µs** (7.4%) | 620.6 µs |

`miss=0` on every press. **+1.9 µs worst case, +0.3 percentage points** — that is the whole
ramp: `gain_ctrl_mute_set()` (four comparisons and a table read) plus one shift-add step per
block. The `___divdi3` that used to run in this exact spot is gone, and nothing replaced it.
The two 50.3 µs samples out of twelve are the presses whose console line landed in the same
window, not a ramp cost that varies.

Listening test at both ends of the 299 ms exponential: **no click and no residual** (checked
by ear on headphones, 2026-08-06). Mute is a true digital zero, so there is nothing left to
hear at the bottom of the curve.

### After Q15, the remaining cost was the LOOP, not the arithmetic: 8× unrolling (2026-08-06)

Once the gain kernel stopped calling `___muldi3`, the three callback paths were close enough
together to ask what they still had in common. The three measurements above answer it:

    mute 19.7 µs = 7.7 cyc/slot     copy 20.9 µs = 8.2 cyc/slot     gain 48.4 µs = 18.9 cyc/slot

**`copy − mute` is 0.47 cycles per slot.** Copy does everything mute does and additionally
*reads* two words per slot — and those two extra loads cost half a cycle. A loop that is
store-bound or memory-bound cannot behave like that. So the shared ~7.7 cycles were not the
data movement at all: they were the per-slot loop itself — the index compare, the branch, and
the two pointer increments, none of which move audio. That is an unrolling problem, and it is
the reason **lever B (loop efficiency) was chosen over lever A (transmitting fewer slots)**:
lever A would have cut the work by dropping 6 of 8 slots, i.e. by giving up the 8-channel
capability this configuration exists to measure.

`WM8904_AUDIO_UNROLL = 8`, all three loops rewritten as down-counting `do/while` over
whole groups of eight slots with constant-offset bodies (`dst[0] = src[0] … dst[7] = src[7]`,
then one `+= 8` on each pointer). A `_Static_assert` ties it to the geometry: a ping/pong
half must be a whole number of groups, so a future `SLOTS_PER_FS`/`BLOCK_FRAMES` that does
not divide by 8 fails the build instead of running off the end of a half.

Measured on EV88G73A, same board, same `-O2`, 48 kHz / 32-frame blocks, `miss=0` throughout:

| path | per-slot loop | 8× unrolled | Δ | cyc/slot |
|---|---|---|---|---|
| `mute` | 19.7 µs (2.9%) | **9.6 µs (1.4%)** | **−51%** | 7.7 → 3.8 |
| `copy` | 20.9 µs (3.1%) | **14.9 µs (2.2%)** | **−29%** | 8.2 → 5.8 |
| **`gain`** | 48.4 µs (7.2%) | **36.0 µs (5.3%)** | **−26%** | 18.9 → 14.1 |

Margin on the gain path went 622.7 µs → **635.3 µs**. `mute` gains the most, which is what
the `copy − mute` argument predicts: it is the path with the least real work, so it is
almost entirely the overhead being removed.

**One prediction did NOT hold, and it is worth recording.** The expectation was that
unrolling removes a *fixed* per-slot overhead, so `gain − copy` — the scaling arithmetic
itself — should survive unchanged. It did not: it fell from 27.5 µs to **21.1 µs**
(10.7 → 8.2 cyc/slot). Unrolling made the *arithmetic* cheaper too, because with eight
bodies in a row the compiler addresses slots by constant offset from one base and reuses
loaded registers, instead of re-deriving two pointers every slot. So "unrolling only removes
loop scaffolding" is too weak a model for this core.

**The sampled peak is not under-reporting — checked, not assumed.** Every figure in this
document is the `max=` of a profiler that times **1 block in 16**
(`DSPIC33CK_TDM_ISR_TIMING_SAMPLE_LOG2 = 4`). Rebuilt with `-Define
DSPIC33CK_TDM_ISR_TIMING_SAMPLE_LOG2=0` so that *every* block is timed, and re-measured:
**36.0 / 14.9 / 9.6 µs — identical, to the resolution of the line.** These loops are
branch-free and constant-cost, so there is no rare expensive block for a 1-in-16 sample to
miss. That validates the sampling for every measurement above.

> **Trap met while doing this:** a build that only changes a `-Define` **must be `-Full`.**
> Without it the makefiles are regenerated but no source is newer, so objects compiled with
> the old flags are silently relinked. The first `LOG2=0` image came out at 51,804 B where a
> `-Full` build of the same thing is 51,750 B, and it produced "identical" numbers for the
> honest-looking reason that it was largely the same firmware. The `-Full` LOG2=0 image is
> **30 bytes smaller** than `-Full` LOG2=4 *despite* a 7-character-longer build ID — that
> size drop is the removed countdown branch, and it is the evidence the define took effect.

Cost of the unrolling: measured back to back against `9f8b40d` with the same configuration,
**49,611 → 51,780 program bytes (74% → 77%)** and **4,728 → 4,740 data bytes**.
(The 49,629 B quoted earlier in this document was a build with a different `-BuildId` length;
49,611 B is the same-conditions baseline.)

That +2,169 B is *not* the unrolled bodies alone — the same working tree also adds the `*ts`
mute-verification strings below. Both `.map` files carry per-function `.text.<name>` sections,
so the split is measurable; 317 functions are present in both images and only eight changed
size at all:

| `.text` section | before | after | Δ | what changed |
|---|---:|---:|---:|---|
| `wm8904_audio_block_cb` | 450 B | 1,125 B | **+675** | the 8× unrolling (all three `path_*` inline here) |
| `dsp_load_print` | 447 B | 645 B | +198 | wall-clock window guard + the `n/a` branch |
| `wm8904_audio_stop` | 78 B | 150 B | +72 | three-way stop state |
| `wm8904_audio_start` | 753 B | 789 B | +36 | unmute-unverified warning |
| `wm8904_set_analog_output_mute` | 390 B | 423 B | +33 | `bool` return of the sticky I/O health |
| `wm8904_audio_poll` | 282 B | 306 B | +24 | idle line three-way |
| `dsp_load_reset` | 45 B | 60 B | +15 | `s_ref_ticks` stamp |
| `wm8904_audio_stop_report` | 42 B | 51 B | +9 | three-way switch |
| **sum** | | | **+1,062** | |

So **the unrolling itself is +675 B and everything else is +387 B of code.** The remaining
1,107 B of the total is const/string, not `.text`: no function grew by it, and the diff adds
a net 1,064 characters of string literal (the long "analog mute NOT verified" wording and the
new curve comment strings) — about one counted program byte per character. `gain_ctrl_init`
and `gain_ctrl_mute_set` did not change size at all, which is the expected shape for the
791 ms curve: it is a fifth row in a `const` table, not new code.

### The static cycle estimate was 22% optimistic — and size is not a proxy for ISR cost

A disassembly-based estimate (1 cycle/instruction, `mov.d`=2, `rcall`=2, `return`=3, taken
branch=2) gave 106.1 µs against **136.2 µs measured** on the same callback. The model was
right in direction and about right in magnitude (it called itself ±20%), but its published
percentages should be read **~1.28× higher**:

| configuration | as published | calibrated (×1.28) |
|---|---|---|
| L/R only (2 slots), `-O0` | 26.1% | ~33% |
| all 8 slots, `-O0` | **59.5%** | **~76%** |
| all 8 slots, `-O1` | 45.8% | ~59% |
| all 8 slots, `-O1` + non-library 32×32 multiply | 15.3% | ~20% |

Caveat on the calibration: it comes from a *different* callback (sine table lookup and
stores) than the gain path (`___muldi3` per sample), and different instruction mixes stall
differently, so 1.28× is a rough correction, not a measurement of the gain path.

**Size was the wrong proxy.** An earlier note used "the 8-slot change got slightly *smaller*"
to imply the cost was fine; the loop lost a branch per slot but gained six more multiplies
per frame. Widening 2 → 8 slots cost **+33 percentage points** of the block period. A boot
log showing no regression is not evidence about ISR headroom.

---

## Part 6 — AK → CK feedback: traps CK has not fallen into yet (2026-07-21)

From `dspic33ak-hal-starter`, which advanced after the CK port. All items were applied
2026-07-21 (all 30 sources compile+link, zero warnings in ported files). Kept because
**items 1-3 are traps AK found on hardware that CK has not yet hit — CK simply lacks the
corresponding code, so the risk arrives with the feature.**

**1. Interrupt-driven UART: the per-instance IRQ map, with `#else #error`.** AK's real bug
(commits `ddc1e47`/`28c5d1b`/`0678dbf`): UART interrupt flags hardcoded to IFS3 while the
device used IFS2 → the descriptor collapsed to `{NULL,NULL,0}` → RX ring enable failed →
init rolled back → **UART dead with no error and no output**, while the polling path stayed
alive so nothing noticed. CK's UART is polling-only today and touches no `IFSx`, so it is
not mis-mapped — **but the moment CK goes interrupt-driven, the per-UART RX/TX flag bank
must be re-derived from the CK datasheet/DFP and guarded with `#else #error`.**
(cf. memory `ak128-uart1-ifs2-vs-ifs3-bug`)

**2. TDM frame-slip / connector-glitch detection (`SPIxSTAT`).** AK samples SPIROV/SPITUR/
FRMERR per RX block and counts `err_rov/tur/frm_block_count` plus
`frmerr_consecutive_blocks`. Why it matters: **an electrical glitch on the connector can
bit-slip the TDM frame while DMA keeps running and the transport looks healthy — the audio
just goes silent.** The consecutive-error count is what gives the app a restart trigger.
Split: **detection in the HAL, recovery in the app.** CK has no sampling or counting of
these yet; the logic ports, but the `SPIxSTAT` bit positions need re-confirming on CK
silicon.

**3. W0C acknowledge discipline.** Observe all three flags but acknowledge only
`SPIROV|FRMERR` — **`SPITUR` is R/HSC (hardware self-clearing) and must not be written.**
And acknowledge `sw_clearable_mask & ~observed`, never the status snapshot itself:
**writing back a whole snapshot can clear a sticky bit the hardware raised between the read
and the write (a torn write).** This is a general register idiom and applies to CK's SPI,
I2C and DMA flag handling.

**4. DMA bus priority — CK silicon differs.** AK sets `BMXINITPR.DMAPR` to give DMA SRAM
priority over the CPU, because an audio SPI requests every word with a single CHREQ slot, so
CPU delay becomes `DMAxSTAT.OVERRUN` (dropped samples). CK's `dma_reg.h` has **no OVERRUN
status bit at all** (HALF/DONE only). Whether CK has an equivalent bus-matrix priority
mechanism is unconfirmed — if it does, the same dropped-sample gotcha applies.

**5. UART pin-init observability.** AK keeps both `uart_init()` and `board_uartN_pins_init()`
results in `volatile` storage, because **when the console itself is the failing peripheral it
cannot print its own failure.** CK captures the driver init status but not the pin stage;
adding it would separate "init OK but no output" (a PPS mis-route) from a driver failure.

**Already at parity, no action:** `gpio_event` `port_index` is already `unsigned`; the timer
tick flag is already `volatile`; `.gitattributes` already carries the fleet `text=auto
eol=crlf`. **Deferred by plan:** I2C slave role (CK is master-only; the common/device/master
split is parity, so the foundation exists).

---

## Part 7 — Feasibility facts worth not re-deriving

Established before any of the above, and still the reason the design has the shape it does.

**The peripheral has what TDM needs.** SPI1/2/3 expose the framed-mode fields `FRMEN`,
`FRMCNT`, `FRMSYNC`, `FRMPOL`, `FRMSYPW`, `SPIFE`; the width controls `MODE32` and
`WLENGTH`; and the enhanced-buffer/event fields `TXELM`, `RXELM`, `SPIRBFEN`, `SPITBEN`.
PPS carries `SCK1R`, `SS1R` inputs and the SDO/SCK/SS outputs. (`ENHBUF` turned out to be
unusable with DMA — defect 4 — and `MODE32` unusable for DMA-fed 32-bit slots — defect 5.)

**The SPI cannot emit a 50%-duty FS for TDM by itself.** `FRMSYPW` selects a 1-BCLK pulse
or one *wire word*; neither is half of a multi-slot TDM frame. Hence the two-stage design:
a half-frame marker from the SPI, doubled by a CLC J-K flip-flop, returned to the same
external FS pin by PPS — so application code never sees CLC or PPS.

**Frame geometry, for TDM8/32-bit.** One 32-bit slot = **two 16-bit wire words** (the wire
is MODE16, defect 5). `FRMCNT` counts **wire words, not slots**, and encodes the cadence as
**log2(N)**. Full frame = 8 slots × 2 = 16 wire words → `FRMCNT = 4`. Half-frame marker for
the CLC path = 4 slots × 2 = 8 wire words → `FRMCNT = 3`. Getting this wrong puts FS in the
middle of a slot. The `default:` arm **clears `FRMEN`** rather than substituting a plausible
value — an unframed stream is obviously broken, a mis-framed one looks like it works.

**Clock math.** BCLK = Fp / (2 × (BRG + 1)). At Fcy 100 MHz, `BRG = 3` → 12.5 MHz BCLK →
TDM8/32-bit fs = 12.5 MHz / 256 = **48828.125 Hz** (48 kHz nominal needs 12.288 MHz, which
this PLL point cannot produce exactly; 12.5 MHz is the nearest realizable point above it).

**`slots_per_fs` bounds.** Must be a power of two (FRMCNT is log2), and **≤ 16**: 32 slots
× 2 wire words = 64 exceeds FRMCNT's maximum of 32. Enforced by `#error` in `conf.h` and
rejected by `configure()`.

---

## Part 8 — A block-ISR overrun is invisible, and two toolchain traps (2026-08-06)

From the Type_TY AVAS port (`src/app/dsp/avas_type_ty_ck.c`). Nothing here is
specific to that engine.

### The load monitor cannot report an overrun. Structurally.

Selecting a block callback heavier than the block period did not produce
`miss>0`, a negative margin, or a trap. It produced **total silence**: no load
line, no command echo, no `?xl` trap report, nothing on the wire ever again.

The mechanism is worth stating because the instinct is to read it as a crash.
When the ISR takes longer than the block period, the next DMA interrupt is
already pending the moment it returns, so it re-enters immediately and
**forever**. Everything that prints -- the load line, the console echo, the trap
reporter -- is foreground. The foreground never runs again. So:

> **`max=`/`margin=`/`miss=` can only tell you about a callback that fits.** The
> one case you most want measured is the one case the measurement cannot survive.
> `miss=0` on a silent console means nothing.

`?xl` after recovery read `last trap: none, traps since power-on: 0`, which is
consistent: nothing faulted. Do not go looking for a stack overflow.

**Recovery costs a reprogram.** This kit has no reset button, `*sr` needs the
foreground, and the analog output is live the whole time. Drag-and-drop
programming does reset the target (`RCON=0x0083`, EXTR/MCLR) -- but that also
overwrites any reset-cause evidence, so read `?sr` before assuming it means
something.

**What to do instead.** Time the candidate in the FOREGROUND before it ever
reaches a callback (`?ta` in `wm8904_audio.c` is the worked example: same
arithmetic, same state, same optimisation level, no ISR, no audio). And do not
put a path that might overrun into a cycling command like `*tp` -- one keystroke
too many while reading input levels then bricks the console.

### `__attribute__((optimize("O3")))` works, and is the way to mix levels

Verified on XC-DSC 3.31.01 / 33CK64MC105: with the project at `-Os`, the
attribute produces code identical to a real `-O3` build of the same file apart
from register allocation, while `-Os` alone builds a stack frame and a worse
loop. This is how a `-Os` project (which CK64MC105's 64 KB forces -- see Part 5)
gives one hot file the 3.2x that Part 5's table measures.

The earlier claim that only `-O1` was licensed was already corrected in Part 5;
this adds that per-FUNCTION selection is available too.

### XC-DSC emits assembly its own assembler rejects, under register pressure

```
add     w4,[w0],[w0++][bad code=82]     ; Error: missing right bracket / bad expression
addc    w5,[w0],[w0++][bad code=82]
```

A 32-bit accumulate-in-place (`x = x + c` on a struct member) where the compiler
allocates **one pointer register as both source and destination**. There is no
valid encoding, and the assembler fails the build -- it does not miscompile.

Two things make it expensive to diagnose:

- **It is register-pressure dependent, so it has no minimal reproducer.** The
  same statement in isolation compiles at `-Os`, `-O1`, `-O2` and `-O3`. Eight
  variants of the statement shape (in-place vs read/modify/write, flash const vs
  immediate vs register addend, 16- vs 32-bit, pointer vs file-scope state,
  pointer-walking) all compiled clean. It only appeared inside a loop that also
  held two table interpolations and a multiply-accumulate.
- The error text points at a compiler temporary file, not at your source. Use
  `-S -g -fverbose-asm` and read the `.loc` directive above the bad line.

**Workaround: do the state stores EARLY**, before the register-hungry work in the
same loop body, rather than in the natural accumulate-then-advance order. That
shortens the live ranges enough for the allocator to find a second register. It
costs nothing -- the old values are already in registers, which is what the
output needs anyway.

### The flash README's safety marker had drifted twice

`buildtools/README.md` documents stopping audio with `*ts` before flashing and
checking for an exact reply string. That literal matched **neither** the image on
the board **nor** the source in the tree -- three different sentences. Following
the README verbatim yields "do not flash yet" on a board that is correctly muted,
which trains people to skip the gate. A safety check keyed on a full sentence
will drift; key it on the part that carries the meaning.

## Part 9 — One RX overrun ends the console for good, and the gate is why (2026-08-07)

Found by walking into it, not by reading code: a board that had been up for ten
minutes printed its periodic block-ISR line perfectly and accepted **no input at
all** -- not a command, not a bare CR, not even an echo. The image had been flashed
in a previous session and had answered commands then.

### What the evidence ruled out before any code was read

| observation | what it removes |
|---|---|
| `TDM1: max=36.0us(5.3%) margin=635.1us miss=0`, `blk` climbing | the firmware is running and healthy; this is not a hang |
| a USB unplug/replug changed nothing -- `blk` continued at **exactly** 1499 blocks/s across it, no boot banner | the target never lost power, so "power-cycle it" was not what the replug did. The debugger re-enumerated; the application did not restart |
| the bridge's `/command` does `ser.write()` + `flush()` and only then logs `>>` | the bytes physically left the host. Not a host, driver or lease problem |
| nothing echoed | the failure is on the board's RX side, in the running image |

The single-writer lease the bridge keeps (an old terminal window holding TX) is the
usual cause of "printf works, keys do not" on this bench and it was **not** this:
`/status` showed no TCP client at all for the first attempts.

### The mechanism, and it is a gate ordering bug

The console's RX path is the POLLED HAL, not the ISR ring (`rx_isr_*` has no caller
outside the HAL -- worth knowing before reading the wrong file, as happened here):

    console_in_ready()  -> dspic33ck_uart_rx_ready()   -> !URXBE, and nothing else
    console_in_read()   -> dspic33ck_uart_read_byte()  -> clears OERR, then reads

`OERR` halts the receiver: once latched, no further character enters the FIFO until
software clears it. The only code that cleared it sat **behind** the gate:

  1. the FIFO fills and one more character arrives -> `OERR` latches
  2. the reader drains what is buffered -> `URXBE` becomes set
  3. `rx_ready()` now answers false forever, so `read_byte()` is never called again
  4. nobody clears `OERR`; reception is over for the life of the image

TX is untouched throughout, which is why the board looks fine. **The one component
still running was the one that could not get out**, and no counter existed to say so.

### The fix, and why it clears only with the FIFO empty

`dspic33ck_uart_rx_ready()` now clears a latched `OERR` **when it finds the FIFO
empty**, which is exactly the stuck state and provably lossless: on the classic
dsPIC UART, clearing `OERR` resets the receive buffer, so clearing it with characters
still queued would discard a command line instead of receiving it. With the FIFO
empty there is nothing to discard. Cost in the steady state: one extra register read
per poll when the FIFO is empty, no writes.

`?du` prints how many times that has happened (`console_in_overrun_recovered()`
through the console_out seam, so the diagnostics module still does not know which
UART instance the console is on). **0 is the healthy answer**; non-zero means the
failure occurred and was survived -- and a rescue is not a fix for whatever
overran the FIFO, so the count is the thing to watch rather than to dismiss.

The same class of hole exists in the ISR ring HAL (`dspic33ck_uart_rx_isr_ring.c`),
which this board does not use: there the ISR is the only thing that clears `OERR`,
and an ISR that stops being entered clears nothing. It now recovers from the reader's
side too, with two counters -- `stall_recov` and `ie_lost` -- where the second
discriminates the other candidate mechanism, the RX interrupt enable bit lost to a
read-modify-write on an IEC word shared with every other peripheral
(`*irq->iec = *irq->iec | mask`, the deferred F1 item). **That path is untested on
hardware**; it is written because the deadlock is the same one and leaving it in
place after finding it here would be the same mistake twice.

### Costs and status

    buildtools/build.ps1 -Full -Configuration CK64MC105_EV88G73A -BuildId uartrxfix
    -> program 64 425 B (96 %), data 5 290 B (64 %)

Against the shipped default's 63 969 B / 5 278 B: **+456 B flash, +12 B RAM**, most of
it `?du`'s text on a part already at 96 %.

**VERIFIED ON HARDWARE (same day).** Two images carrying this fix were flashed and
run: `?du` printed **0** on both, the console answered throughout, and -- the part
worth keeping -- the AVAS engine came back **identical to the pre-fix image**,
294.4 / 139.0 us and `sink=68957`, so the added register read in the foreground poll
costs the ISR nothing (`ck_avas_type_ty_feasibility.md` section 24).

What that does NOT establish: that the overrun cannot recur. It was never provoked --
the wedge was walked into, not reproduced on demand -- so the standing task is to watch
`?du`. A recurrence now shows up as a number instead of as a dead console, which was
the point.

**What ended the wedge was a RESTART, and the block counter is how we know.** The
deaf image kept counting continuously across the first USB replug (matched to the
block, 1499/s), and the count that appeared afterwards starts fresh about a minute
after the last failed console test -- so reception returned only with a restarted
application, and the debugger re-enumeration on its own never touched the target.

## Part 10 — The audio path became a chain, and what each stage costs (2026-08-07)

The block callback used to be five mutually exclusive writers of the TX half. It is now a
chain — **TDMin -> AVAS -> Gain -> TDMout** — with the four single-purpose positions kept
beside it as the cost baselines this document already quotes. The reason was audible rather
than architectural and is recorded in `ck_avas_type_ty_feasibility.md` section 26: as a
*path*, the synth's ~3 s release fade could never be heard, because stopping it switched the
path away in the same call and the ISR stopped calling the engine.

### Measured on EV88G73A, one image (`chain0002`), 48 kHz / 32 frames / 667 us block

| `*tp` position | `?tl` max | of block | margin | miss |
|---|---|---|---|---|
| mute | **9.7 us** | 1.4 % | 661.6 us | 0 |
| copy | **15.2 us** | 2.2 % | 655.9 us | 0 |
| gain | **36.2 us** | 5.3 % | 635.1 us | 0 |
| chain, synth stopped | **38.3 us** | 5.7 % | 633.0 us | 0 |
| chain, synth started | **321.2 us** | 47.8 % | 350.5 us | 0 |
| chain, synth fading out | **326.9 us** | 48.6 % | 345.0 us | 0 |

The first three are Part 5's figures reproduced to a few tenths of a microsecond on a build
that reorganised everything around them — which is the point of not touching them.

**The chain costs 2.1 us more than the gain path it replaced as the default**, and that is
cheaper than it looks like it should be: it adds a 256-slot verbatim copy, 64 decodes and 64
encodes, and it *removes* the gain multiply from 192 slots (the gain stage now runs on the
two slots that reach a converter instead of all eight). Those nearly cancel.

**The synth is +282.9 us** on top of the idle chain, against the 289.2 us the engine itself
measured in the foreground (`ck_avas_type_ty_feasibility.md` section 24) — so the stage adds
no measurable cost of its own beyond the engine, and the mixing add is inside the noise.

**The fade costs 5.7 us MORE than the fully open gate** (326.9 vs 321.2), consistently.
Recorded, not explained: both are peak-hold maxima over a 2 s window, and 5.7 us is where a
saturating add that actually clamps, or a rebuild coinciding with the window, would show up.
Nothing acts on it — the margin is 345 us either way — but it is the kind of small,
repeatable difference that is worth having written down before someone re-derives it.

### The fade is visible in the load line, which is how it was verified without ears

    18:09:09.633  >> a                  *ta: avas synth stop -- fading out over ~3 s
    18:09:10.798  max=326.9us(48.6%)    still rendering
    18:09:11.055  ?tp: avas stage = fading out (~3 s release, still rendering)
    18:09:12.778  max=326.7us(48.6%)    still rendering
    18:09:14.758  max=38.3us(5.7%)      stage skipped -- costs nothing
    18:09:20.704  ?tp: avas stage = stopped (skipped entirely -- costs nothing)

The engine kept costing its full 48.6 % for at least 3.1 s after the stop and was idle by
5.1 s; the design figure is 5.8 tau = 2.9 s. The uncertainty is the load line's 2 s cadence,
not the fade. Before this change the same keystroke took the load to 2.2 % (copy) within one
block — which is exactly the bug, seen from the load monitor rather than from the speaker.

`?du` read 0 across the whole session, i.e. no console RX overrun (Part 9).

### Two things a reader should not have to re-derive

* **The AVAS stage is skipped entirely when the synth is idle**, rather than called
  unconditionally the way sonora's chain calls its sources. Two reasons, both local to CK:
  the ISR would otherwise pay 32 calls per block for nothing on a core with no headroom to
  spare, and `?ta` benchmarks the same engine **in the foreground** — if the ISR could still
  be touching it, the benchmark and the audio would corrupt each other. The flag that gates
  it is set by the foreground and **cleared by the ISR**, so the fade always renders to its
  end and the foreground never has to guess when to stop.
* **Flash went to 98 %** (65 448 of 66 544 B; data 5 504 of 8 192). The chain cost ~1.1 KB,
  a third of it console strings. The two measured levers are unspent and stay documented in
  the AVAS doc: `AVAS_TYPE_TY_CK_CORDIC_N=10` (0.03 dB) and `TABBITS=8` (-1 280 B for 0.1 dB).
* **Then the merge into `main` spent most of what was left: 99 %** (66 213 of 66 544 B,
  **331 B free**; data 5 510 of 8 192). Nothing in the AVAS work grew — the +765 B is
  `main`'s own side of the merge (the CK reset-snapshot family, the PPS additions, and the
  25 lines `main` added to `system_console.c`), which had been costed on a `main` image at
  14 % on DM330030 and never against the 98 % one. The two branches were textually disjoint
  in `src/` (AVAS touched `app/dsp`, `hal_uart`, `uart_app`; `main` touched `hal_gpio`,
  `hal_dma`, `hal_reset`, `hal_spi_i2s_tdm`) so the merge itself was clean and needed no
  conflict resolution — **the collision was in the linker, not in the text**. DM330030 is
  unaffected at 38 673 B (14 %). Practical consequence: on EV88G73A the next feature of any
  size has to spend a lever first, and `TABBITS=8` alone (-1 280 B) buys back more than the
  merge cost.
* **The merged image was then flashed and smoke-tested on the board, and the owner passed it
  by ear** (music OK, AVAS OK, mute OK, mix OK). Build `52c786c`, verified the way the README
  requires — `*ts` first and gated on `analog mute verified`, then drag-and-drop, then
  `Build ID = 52c786c-20260807T193926213-24e4e2a930` read back over UART, so the verdict is
  `Success` rather than the `Indeterminate` that `STATUS.TXT` alone can give. Boot evidence:
  `PLL at target` / `PLLstatus=0` / `LOCK=1`, DMA selftest PASS, WM8904 dev ID `0x8904`,
  DC servo `[7f 7f fd 0b]`, `apply=verified`, and `path=chain` as the boot default.
* **Every load figure reproduced the pre-merge baseline exactly, which is the useful part of
  this test**: chain idle **38.3 µs (5.7 %)** margin 633.0, chain+synth **321.0-321.2 µs
  (47.8 %)** margin 350.4, `miss=0` throughout, `?du=0`, CODEC-IN peak slot0=3794 slot1=3491.
  The recorded pre-merge pair was 38.3 and 321.2 µs at margin 350.5. So **`main`'s HAL side
  cost zero cycles** — the whole price of the merge was the 331 B of flash above, and the
  three-second fade still renders to its end (48.6 % held for ~6 s after the stop, then
  5.7 %), which is the load-line check that replaces an ear for that one property.
* **One free check is deliberately still open: `?ta`'s `sink=69200`.** A valid reading needs
  `*sr` immediately before it (§17: after a live AVAS path, `gate_on()` skips `reset_phase()`
  and the checksum becomes meaningless), and resetting a board mid-listen was the wrong trade.
  It costs one command whenever the console is next quiet.
* **And then it went back to 85 %, without spending either lever** (56 793 B, **9 639 B
  free**). Do not carry the 331 B away from this section — it stood for one day. The 9.5 KB
  was `printf`, pulled in by a single `#define TRACE printf` in the ported WM8904 driver;
  gating that one line dropped the whole stdio block, and unifying the console line endings
  took another 120 B. The AVAS levers (`CORDIC_N=10`, `TABBITS=8`) are still unspent and are
  still the right first thing to reach for. Both changes plus the AVAS review are verified on
  the board — see `ev88g73a_rom_budget.md` sections 6-8 and `ck_avas_type_ty_feasibility.md`.
  One operational rule came out of that session and is easy to misread as a regression:
  **after flashing an EV88G73A, power-cycle before judging the audio** — the WM8904 does not
  lose power on an MCLR reset and a warm codec can come up as a square wave. It also means
  the bullet above is describing the *previous* image: `52c786c` was passed by ear before the
  power-cycle rule was known, on a board that had been power-cycled in the ordinary course of
  the session.

## 19. IFSx/IECx are shared words, and the C does not show you which writes are atomic

The check is mechanical and the fix is fleet-wide; `tools/irq_atomicity_source_lint.py`
is what enforces it.

The two facts worth carrying:

* On both CK parts here, `IFS0`/`IEC0` holds `T1IF` (the 1 ms tick), `DMA0/1IF` (the audio
  block ISR), `CCP1/CCT1IF` (the load-monitor time base), `SPI1RX/TXIF` and `U1RX/TXIF`.
  The tick, the audio and the console share one 16-bit word — there is no "just the UART's
  interrupt bit" on this board.
* **A DFP bit alias is atomic only when the value assigned is a compile-time constant.**
  `_T1IE = 0` is one `bclr.b`; `_T1IE = saved_ie` is load/mask/or/store over the whole word
  and erases whatever else changed in between. `_DMA0IE = v` is safe — a runtime *value* into
  a constant *bit* becomes one `bfins` — so the distinction is the destination, not the
  value. Measured, not reasoned: `mov.w wN, 0x8xx` in the objdump is the defect, and it is
  invisible in the source.
