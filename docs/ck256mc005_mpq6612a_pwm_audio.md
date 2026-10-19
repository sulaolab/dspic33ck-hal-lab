# CK256MC005 Nano: WM8904 CODEC to socket 3 MPQ6612A

For the current 0 dB, selectable-carrier bench builds, SW0/LED controls and
pre-flash PWM stop, see [PWM bench controls](ck256_pwm_bench_controls.md).

The EV08P02A image can mirror the WM8904's final left TDM output to an
MPQ6612A board in mikroBUS socket 3. The ordinary WM8904/FRCPLL build stays
the default; build this route with:

```powershell
pwsh buildtools/build.ps1 -Configuration CK256MC005_EV08P02A -Define EV08P02A_PWM_AUDIO=1 -Full
```

This always uses the common `hal_pwm_audio` core with the CK backend
(`ev08p02a_pwm_audio_hal.c`). To run it on the MPQ6612A PCB without an
X1-to-OSCI jumper, select the FRCPLL elastic clock:

```powershell
./buildtools/build.ps1 -Configuration CK256MC005_EV08P02A `
    -Define @('EV08P02A_PWM_AUDIO=1', 'EV08P02A_PWM_AUDIO_HAL_FRC=1') -Full
```

This build uses 200 MHz FOSC and a 4166-count PG4 period (about 48.008 kHz).
The WM8904 still supplies the 48 kHz TDM clock independently. After the HAL
maps the final L-channel Q31 PCM to duty, the CK backend holds up to 256 duty
samples, waits for 128 samples at start, and interpolates its read position by
at most 1% to keep the FIFO near 112 samples. This is the same rate-matching
policy as the older FRC scope image. The common HAL core is unchanged.
The MPQ stage is active in this build: nSLEEP must wake, nFAULT must be high,
and its 500 ns minimum pulse remains in force. The current PWM-only gain
defaults to 0 dB; the 2026-10-09 first-light image below used approximately
-20 dB. The periodic `Socket 3 MPQ (hal_pwm_audio)` line reports HAL
block counts, fault flags, and FRC FIFO level/min/max, rate correction,
prefill, local underrun, stretch, and compress counts. The original ECPLL HAL
build remains available by omitting `EV08P02A_PWM_AUDIO_HAL_FRC=1` and wiring
X1-to-OSCI.

On 2026-10-09 the FRCPLL HAL image was programmed to the CK256MC005
EV08P02A kit. Its boot ID matched the saved build; after roughly
one minute, `written=89841`, `rejected=0`, `underrun=0`, `fault=0`,
`local_underrun=0`, TDM `miss=0`, and the observed FIFO interval was 79..112.
The user confirmed both speaker audio and the PWM signal and accepted the
conservative first-light level. These observations establish first-light
operation, not extended electrical or thermal qualification.

For a board-less logic-output test, build a separate scope-only ROM image:

```powershell
./buildtools/build.ps1 -Configuration CK256MC005_EV08P02A `
    -Define @('EV08P02A_PWM_AUDIO=1', 'EV08P02A_PWM_SCOPE_ONLY=1') -Full
```

Leave the MPQ6612A PCB out of socket 3 and probe PWM/RB7 and CS/RB15 against
Nano ground. Scope-only mode keeps RST/RB13 (nSLEEP) low, does not require an
nFAULT level on INT/RB12, and still stops on DMA or clock failure. A silent
input produces a 48 kHz, approximately 50%-duty complementary logic waveform;
audio changes the duty. Scope-only mode maps the completed TDM L Q31 value
linearly to nearly the full duty range, with a four-count guard at each end
(approximately 0.096% to 99.904% for the FRC period). Zero maps to exactly
50%; no additional PWM gain or normalization is applied. The normal MPQ
build retains its approximately -20 dB first-light level and 500 ns minimum
pulse width. The Rev4 or Rev5 CODEC, X1-to-OSCI jumper, and running WM8904
TDM path remain required because PWM starts after the transport starts. The
banner and periodic status explicitly say `SCOPE ONLY`. This test establishes
the Nano pin, PPS, PG4, clock, and DMA path; it does not exercise the bridge or
speaker. Rebuild without `EV08P02A_PWM_SCOPE_ONLY=1` for the actual PCB test.

The first-light `ev08p02a_pwm_audio.c` path, including its independent-clock
scope variants (`EV08P02A_PWM_SCOPE_FRC`, `EV08P02A_PWM_ELASTIC_FIFO`), was
removed on 2026-10-15; it is in git history before that date. The jumper-free
equivalent is the HAL build with `EV08P02A_PWM_AUDIO_HAL_FRC=1` above, which
carries the same 256-word elastic FIFO and rate matching.

This is a first-light image. Leave `EV08P02A_WM8904_DSPIC_IS_MASTER=0`, place
the POT Click in socket 2, and use a Rev4 or Rev5 CODEC in socket 1. Rev4
hard-wires the 12.288 MHz X1 signal to mikroBUS pin 2 (`XTAL_OUT`), with no
output-disable jumper. Rev5 provides jumper U16 and R55 for that output, so
close U16. Add one jumper
from socket 1 RST/RA0 (mikroBUS pin 2) to the Nano RB0/OSCI edge pin. Set U8
to the X1 side so the WM8904 and Nano use the same source. The Rev4 board has
a separate known L/R swap; this clock statement does not validate its audio
channel mapping. Socket 3 IN1/PWM=RB7/RP39,
IN2/CS=RB15/RP47, nSLEEP/RST=RB13, and nFAULT/INT=RB12. Socket 3 AN/RA4 is
VISEN and is not sampled by this implementation.

In this build `POSCMD=EC`, `FCKSM=CSECME`, and `DUPPWM=PPS_ONLY`. The clock HAL
selects Primary PLL at 196.608 MHz FOSC from the 12.288 MHz input. PG4 uses
FOSC directly. On this FOSC path the default carrier factor 4 uses period 1024
(192 kHz) and factor 1 uses period 4096 (48 kHz). PWM4H/L are mapped by
PPS to RB7/RB15. PG4 period end selects PWM Event A, which triggers DMA2 to
write the next PG4DC. `UPDTRG=01` requests the buffered duty update on that
write. PWM output starts at both inputs low with nSLEEP low, then wakes the
bridge, checks nFAULT, and releases the PWM override. DMA2 maintains a
64-word ping-pong duty buffer from a 128-word single-producer/single-consumer
FIFO. Underrun fills a half-buffer with 50% duty; overflow drops an entire
32-frame audio block. The periodic `Socket 3 MPQ` line reports those counts.

The source is slot 0 after the selected WM8904 path completes, including
analog-input gain, AVAS mix, mute, and post gain. The normal MPQ output is
attenuated approximately 20 dB for first light; scope-only maps the final
Q31 stream across nearly the full duty range while nSLEEP stays low.
`*ts` also stops the bridge and `*tr` restarts it. A detected
nFAULT, DMA2 overrun, or clock loss lowers nSLEEP. The oscillator-fail trap
lowers it before the trap policy resets the chip.

The 192 kHz default carrier and bipolar 50% idle are for scope verification, not a
filterless-speaker quality or thermal acceptance point. Before connecting a
speaker, verify XTAL_OUT level/startup on RB0, 196.608 MHz PLL lock, both
socket-3 PWM pins and complementary polarity, nSLEEP/FAULT sequencing,
DMA2 movement, and buffer counters. Then verify current, switching voltage,
temperature, and audio at a conservative supply/level. No hardware result is
implied by a successful firmware build.
