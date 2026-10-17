# CK256MC005 PWM bench controls

This board/application integration follows the AK Nano PWM bench behavior.
It uses the existing common `hal_pwm_audio` unchanged. Hardware: CK256MC005
Nano on the Click base, WM8904 Rev4/Rev5 in socket 1, POT in socket 2, and
the non-D MPQ6612A PCB in socket 3. Only the final left TDM channel is sent
to the PWM output; the CODEC analog output continues to work.

## Prepare an image without hardware

```powershell
# PCB absent: PWM H/L can be measured, normal nSLEEP stays low.
pwsh buildtools/prepare_pwm_bench.ps1 -Mode ScopeOnly -CarrierHz 192000

# PCB present: nSLEEP wakes and nFAULT is checked, PWM-only gain 0 dB.
pwsh buildtools/prepare_pwm_bench.ps1 -Mode Normal -CarrierHz 192000 -PwmGainDbX10 0
```

Both builds use FRCPLL, nominal FOSC 200 MHz / FCY 100 MHz. No X1-to-OSCI
jumper is needed. The helper performs a full build and saves a factory HEX,
build log and manifest under a unique `artifacts/pwm_bench/` directory.
It does not program or access a board. Verify a saved image with
`pwsh buildtools/verify_pwm_bench.ps1 -Manifest <manifest.json>`.

The normal image supports the three intended bench cases without changing
images: (1) no PWM PCB, boot then `*cp5501` and measure; (2) PCB fitted but
no 12 V, boot then `*cp5501` and measure; (3) PCB fitted and 12 V present,
boot normally and measure with nFAULT checking enabled. A normal boot without
a powered stage may latch `fault=1`; explicit lab start re-initializes the
HAL to clear that flag, bypasses nFAULT and enables PG/DMA/nSLEEP. No stage
reply or connected PCB is required for lab start. `*cp5500` leaves lab mode;
`*tr` then restarts the normal checked output.

For load-noise diagnosis, `*cq0000` suppresses all board-profile periodic
reports, `*cq0001` resumes them, and `?cq` reads the switch. Explicit queries
remain available. This switch does not stop the audio transport, PG, DMA,
power-stage fault checking, or FIFO correction. It is separate from
`*tq0000`, which suppresses only the TDM load line.

### Load-noise investigation (2026-10-14, ongoing)

With the MPQ6612A stage supplied at 9 V, the operator heard harsh noise over
music, while the CODEC headphone output was good. GND-to-A and GND-to-B
showed inverted PWM with duty changing with music volume. The operator
reported that the harsh noise nearly disappeared both when playback was
paused and when `*tp0001` selected exact digital zero. A separate brief buzz
remained roughly every 2.2 seconds at digital zero; disconnecting 9 V removed
it. These are two observations, not yet a proven common cause.

At digital zero the reported duty is exactly 521/1042 with PG4, DMA2 and
nSLEEP enabled. Normal operation reported no DMA errors, rejected blocks,
FIFO underruns or stage faults. During earlier music playback the observed
duty occupied only several counts around 521. Limited effective resolution
and sample-update timing remain hypotheses; the counters alone do not prove
pin waveform continuity or audio quality.

The complete board report burst runs approximately every two seconds.
The application-only report switch above was added for an ON/OFF listening
comparison. A Normal/FRC/192 kHz/0 dB image with build ID
`ccf930f-dirty-da97ff1-20261014T152751227-dbdcdb783b` was programmed after a
verified stop, and its boot ID matched. ROM SHA256:
`F3E400DACB1633B29391D607E69CBA14EABFA74EB7C961C01860C94A8227774E`.
Digital mute and report-OFF commands were accepted. The operator reported
that the periodic buzz nearly disappeared with reports OFF. A faint hiss
change synchronized with LED blinking remained audible only at close range;
the operator explicitly considered it acceptable and outside remediation
scope. Report processing/UART activity is strongly associated with the buzz,
but the electrical versus firmware-timing coupling mechanism is unproven.
Common `src/hal_pwm_audio` was not changed. Return to the normal
chain with `*tp0004`; restore periodic reports with `*cq0001`.

A same-source, same-PWM-gain carrier comparison was then programmed:
Normal/FRC/48 kHz/0 dB, build ID
`ccf930f-dirty-d7d140a-20261014T154602046-6e5be23da0`, ROM SHA256
`65B744FA3AA453B369A774E907F4268DC62CAC929185E890AFBF759E70CECC49`.
All manifest source hashes match the report-switch 192 kHz image. Carrier
factor changes from 4 to 1, period from 1042 to 4166 counts; the 100-count
minimum pulse remains unchanged, so usable modulation range also widens.
This compares carrier/resolution behavior, not a mathematically isolated
quantization-only variable. Verified stop and boot ID passed. Final state:
normal chain, periodic reports OFF, PG4/DMA2/nSLEEP ON, nFAULT checking ON.
The operator reported essentially unchanged harsh noise and recurrence of
the hum previously heard on AK at 48 kHz. This carrier reduction is not a
remedy; the 192 kHz report-switch image is retained for continued diagnosis.

The CK TDM buffer stores each 32-bit slot as two 16-bit wire-order words:
MS half first, LS half second. The PWM hook calls
`nora_tdm_slot_decode_s32()` rather than casting the buffer to `int32_t`.
The CODEC driver selects a 32-bit AIF word; the existing audio contract
places its 24 significant bits at bits 31..8. The PWM core scales the
signed Q31 word into the configured period, then writes a single 16-bit
PG4DC value. No additional 24-to-16 shift is required. This source review
does not yet prove live sample continuity through DMA and PG updates.

A local host check compiled the actual encode/decode accessor bodies and
the unchanged common modulator, covering signed endpoints, zero, low-bit
values and a mixed-half pattern. Both period1042 and period4166 passed.
For left-aligned 24-bit positive full scale (`0x7FFFFF00`), returned duties
were 942 and 4066 respectively; zero returned 521 and 2083, negative full
scale returned 100. This verifies word assembly and numeric mapping for
those inputs; it does not verify live buffer ownership or peripheral timing.
After the unsuccessful 48 kHz comparison, the report-switch 192 kHz ROM
was reprogrammed with verified stop and fresh boot-ID match, and left on
the normal chain with reports OFF and nFAULT checking active.

### Root cause and fix (2026-10-14, resolved)

The harsh noise was lost or delayed PG4DC updates, not duty resolution.
DMA2 was triggered by PWM Event A at PG4 EOC, so each DMA write landed at
the period boundary, racing the SOC that applies the previous buffered
update; while PG4STAT.UPDATE is pending the data registers are not
writable. With the load removed, `*cp5803` (20%/80% alternating every
carrier period) showed runs of one width on the scope. The backend now
sets `PG4TRIGA = PWM_PERIOD / 2` and `PGTRGSEL = 1` (TRIGA), so every write
lands half a period before the SOC that applies it. After that change the
pattern alternated cleanly every period and the operator confirmed music
playback by ear. Build ID
`ccf930f-dirty-da97ff1-20261014T181755241-b6053e854b`.

None of the counters (DMA errors, underrun, fault, TDM miss) could see the
lost writes; they stayed zero throughout. The earlier resolution hypothesis,
the 48 kHz carrier trial and the noise shaper did not address the cause.
The legacy `ev08p02a_pwm_audio.c` path got the same fix and was then removed
(2026-10-15); the HAL backend is the only PWM path.

Added bench controls (HAL path):

| Command | Behavior |
| --- | --- |
| `*cp560N` | Error-feedback quantizer order N = 0 (round), 1, 2 (default `EV08P02A_PWM_NS_ORDER=2`) |
| `*cp57GG` | PWM-only gain, signed whole dB (`0C` = +12, `F4` = -12), -24..+24 |
| `*cp5800` | Normal duty stream |
| `*cp5801` | 1 kHz tone generated in the DMA2 ISR (only shaper, PWM and stage remain) |
| `*cp5802` | Elastic FIFO pass-through (no interpolation or rate servo) |
| `*cp5803` | 20%/80% duty alternating every carrier period (scope, load removed) |
| `*cp5804` | 20%/80% duty alternating every 16 periods (scope, load removed) |

`?cp` now also prints a `PWMregs` line (PG4 CON/IOCON/EVT/STAT/PER/TRIGA/DC,
PCLKCON, PWMEVTA, DMACH2, DMAINT2, DMACON).

`-CarrierHz` accepts 48000, 96000 or 192000. Each 48 kHz audio duty is repeated
for 1, 2 or 4 PWM periods. The even-count periods are 4166, 2084 and 1042;
nominal carriers are 48007.681, 95969.290 and 191938.580 Hz. FRC accuracy
determines the actual frequency. In all three modes a 256-sample elastic
FIFO, 128-sample startup threshold and bounded +/-1% consumption correction
accommodate the independent WM8904 crystal clock. Refill drains up to the
three available common-core blocks when local FIFO space permits. An empty
FIFO supplies neutral and re-primes.

Scope mode passes signed Q31 PCM unchanged at 0 dB. At the 192 kHz setting,
`INT32_MIN -> 1`, `0 -> 521`, `INT32_MAX -> 1041` counts with period 1042.
This is linear logical full-scale mapping, with the small end guard; it is
not automatic scaling by the observed audio peak. Normal mode keeps the
100-count minimum pulse (500 ns at nominal FOSC), so its range is 100..942
at that carrier. `EV08P02A_PWM_GAIN_DB_X10` defaults to 0, outside the HAL;
normal builds accept -24..+24 dB in 0.5 dB steps. The CODEC PRE 0 / POST +12 dB
chain is retained. AVAS-only scaling is raised by approximately 6 dB to
follow the AK source change; external input is not raised by that change.

## Console controls

| Command | Behavior |
| --- | --- |
| `?cp` | PWM mode, PG4/DMA2, nSLEEP output latch, nFAULT input, carrier/period, duty range, PWM gain, HAL and FIFO counters |
| `*cp00` | Stop PWM (scope build) |
| `*cp01` | Start PWM with nSLEEP low (scope build) |
| `*cp02` | Running neutral duty (scope build) |
| `*cp03` | Resume audio duty (scope build) |
| `*cp5501` | Explicit lab start: nSLEEP high and nFAULT bypass, including fault re-init |
| `*cp5500` | Stop PWM, nSLEEP low, leave lab mode |
| `*cp5901` | Inject a stage fault: same EMERGENCY_SAFE + STAGE latch as a low nFAULT; `write()` returns FAULT until re-init (`*cp5501`, `*tr`, reset) |
| `*ts` | Stop speaker PWM first, then verify CODEC analog mute and stop TDM/DMA |
| `?ts` | Report CODEC stop state and PWM state without changing either |
| `*tr` | Existing CODEC/TDM restart, including its PWM output start hook |
| `?cm` | Mute request/applied/ramp status |
| `?cP` | Existing POT status |
| `?cs` | AVAS pitch status |

`*cp5501` intentionally bypasses the board's nFAULT input. Clock and DMA faults
remain effective. PCB-absent scope measurements normally use `*cp01`, not lab
start. `*cp5500` returns to normal stage checking; `*ts` stops the current output
but retains the selected lab/neutral settings for an explicit later restart.

The `?cp` duty min/max values hold peaks between reports; the report snapshots
and resets those values with DMA2 IRQ briefly masked, then prints with it
restored. They describe calculated DMA duties, not a scope measurement.

`*ts` calls the board stop independently of CODEC configuration, so it also
stops a PWM-only lab run after failed CODEC initialization. The board sequence
is nSLEEP low, DMA2 off, DMA2 IRQ off, both H/L overridden low, PG4 off. Repeated
stops are valid. The existing `analog mute verified, TDM/DMA halted` flash-gate
text is preserved, and PWM status is printed after the stop. A CODEC mute
failure cannot keep PWM running, but does not count as verified analog mute.
The flash helper's bounded best-effort/recovery policy is unchanged.

## SW0, LED and pitch

SW0 uses 15 ms debounce. A short release toggles the existing approximately
800 ms master mute ramp. A 1500 ms hold advances TY -> OFF -> LB -> OFF once;
release after that hold does not toggle mute. A start rejected during fade or
at an unsupported rate does not consume the next voice selection.

The LED is on 500 ms / off 500 ms normally, or on 100 ms / off 900 ms while mute
is requested. Up/down cursor keys change pitch by +/-10 cent (CSI and SS3
sequences, with a 250 ms incomplete-prefix timeout). Existing POT direction,
calibration and cursor takeover until the next AVAS key are retained.

## Source and verification boundary

The AK reference is commit `8d810e35d67e2094530ce2c374ef07e78eba8072` plus
the locally reviewed uncommitted changes. The reviewed 19-file content-set
fingerprint is SHA256
`0958C076A8A7E6D481F187661C5906A930255F9AEB13F9ABE525E41415CEDAE5`.
This identifies the inspected source set, not a published AK commit.

Hardware-free checks:

```powershell
python tools/pwm_audio_host/run_pwm_audio_host_test.py
python tools/pwm_audio_host/test_ck_pwm_bench.py
python tools/pwm_audio_host/test_ck_preflash_stop.py
python tools/pwm_audio_host/check_pwm_audio_parity.py <AK-reference-checkout>
```

The bench test compiles unchanged extracted CK refill/control functions and
the real common core: factors 1/2/4, scope/normal, +/-0.3% producer mismatch,
30000 DMA halves, FIFO starvation/re-prime, SW0, LED, cursor and command
grammar. The stop test runs the real command branch, CODEC stop, common core
and board stop with mocked hardware, checking PWM-only/ordinary/repeated
stops, I2C mute failure, query and malformed payload. These checks do not
establish actual pin, speaker or ISR timing behavior.

On 2026-10-14, the Normal/FRC/192 kHz/0 dB image was programmed to CK256MC005
kit. The boot banner matched
`ccf930f-dirty-da97ff1-20261014T132023365-0d6058fee2`; ROM SHA256 was
`23A20BAB3ED02037899EC6323B7E1471F1FAAB8C87E9E481FDA6973E3704CAA1`.
The previous live firmware returned `$01cp5501` (command not found). The
new image returned `$80cp5501` and reported `lab=1 pg=1 dma=1 nSLEEP=1`.
`*cp5500` reported `lab=0 pg=0 dma=0 nSLEEP=0`. Two `*ts` commands both
reported PG/DMA off and nSLEEP low; `*tr` restarted transport and PWM.
The final state was audio running with lab PWM enabled for scope observation.
The observed nFAULT input was high during this software verification; the
PCB/12 V arrangements and physical waveform were not changed or measured by
the task. Host checks separately cover nFAULT low in normal/scope/lab modes.
The ScopeOnly image was prepared but not programmed. After the Normal-image
flash and lab command verification, the operator confirmed "signal OK" on
2026-10-14. This confirms PWM output in the arrangement then connected; it
does not independently qualify all three PCB/12 V arrangements above.
Acoustic acceptance and the remaining arrangements are separate measurements.
