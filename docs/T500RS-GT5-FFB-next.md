# GT5 2.11 force-feedback implementation evidence

The user's 2026-09-25 test confirms buttons and steering. The reported pedal
exchange is corrected separately: brake bytes 3..4, throttle 5..6, clutch 7..8.
The new PS3 FFB decoder is experimental and still needs a physical-wheel test.

## Confirmed cause and implemented behavior

The driving log contains 1,126 opcode-05 condition updates and 102 opcode-03
constant updates. Previously the emulator rejected MAIN types 1/7/8, initial
zero flags and START value 1. This prevented effects regardless of host health.
The new implementation auto-detects PS3 FFB with either configured PC fallback.

| Command | Interpretation and implementation |
|---|---|
| MAIN `01 slot 01 ...` | Constant force, referenced opcode-03 parameters |
| MAIN `01 slot 07 ...` | Spring, referenced opcode-05 parameters |
| MAIN `01 slot 08 ...` | Damper, referenced opcode-05 parameters |
| MAIN flags `00` / `40` | Initial / active declarations; zero duration creates no force |
| START `41 slot 01 01` | Start once; repeated START retriggers |
| STOP `41 slot 00 01` | Stop and release the host effect |
| Gain `43 80` | Full gain; PS3 range is 0..128, unlike PC 0..255 |
| Direct force `81 X Y` | Persistent signed X force independent of 16 numbered slots; Y ignored for a one-axis wheel; X=0 stops it |

MAIN parameter references are full LE16, bounded to 16 slots * 32 bytes;
X parameter and envelope/Y references do not alias slots 8..15 to lower slots.
MAIN duration is bytes 4..5, direction byte 6, repeat interval 7..8 and start
delay 13..14. Infinite duration is FFFF. Only direction 0 and interval 0/FFFF
are supported. Iteration counts other than 1 and PS3 periodic types 2..6 remain
unsupported. Existing PC capture/Linux-reference decoding remains available.

## Binary evidence

Addresses are guest virtual addresses in BCAS20108 2.11. The supplied
`thrustmaster_module.asm`, `input_init_evidence.asm` and analysis ELF were used;
the game executable is not distributed with this patch.

- Constant wrapper `0x00ADF080` scales signed -10000..10000 to -127..127 at
  `0x00ADF148` onward and supplies type 1 at `0x00ADF1B4`.
- Wrappers `0x00ADED9C` and `0x00ADED70` supply types 7 and 8 to common builder
  `0x00ADEA50`. The named field table maps `THRUSTMASTER_SPRING_X_POS_K` to
  ID 0x20 at `0x016B9D5C`, and the damper field to ID 0x31 at `0x016B9DE4`.
  The corresponding caller loads feed slot 1 spring and slot 2 damper.
- `0x00ADEB1C` onward scales condition coefficients to signed -100..100,
  center to -500..500, deadband to 0..1000 and saturation to 0..100.
  Builder `0x00AE1814` packs coefficients at 3/4, LE center at 5, LE deadband
  at 7, saturation at 9/10. These normalize to SDL signed coefficients/center
  and unsigned full-axis deadband/saturation; original-wheel feel is unverified.
- MAIN builder `0x00AE1604` establishes field positions above. START caller
  `0x00AA0058` and builder `0x00AE1174` establish enable/retrigger and count 1.
- Gain builder `0x00AE214C` clamps to 128; the caller also supplies 128.
- Envelope builder `0x00AE1D14`: reference at 1, attack length at 3, level at
  5, fade length at 6, level at 8. Levels use 0..127.
- FORCE_X/Y table IDs 0x10/0x11 feed `0x00ADF26C` through caller
  `0x00AA0F44`, then builder `0x00AE1EE0` emits opcode 81 and signed X/Y,
  replacing -128 with -127. Direct force lifetime is modeled as persistent
  until zero/reset; this path's nonzero physical behavior needs validation.

## Validation and next driving test

Protocol and mocked USB/SDL suites pass with ASan/UBSan, including signed
scales, upper-slot references, parameters before MAIN, finite duration/delay,
envelopes, live updates without restart, gain, simultaneous effects, direct
force, pause/input suppression, host failures, reconfiguration and unplug.
Both PC fallbacks remain covered, as do 100,000 deterministic random packets.
A replay of all 1,254 OUT packets from the user's driving log has zero rejected
packets. Acceptance alone does not prove force fidelity or host-driver support.

No new analysis tools or MCP installation was needed. Capstone was available
for targeted checks beyond the supplied disassembly. No local full RPCS3 build,
physical FFB test or thread-race test was performed for this change.

For the first test, reduce strength in the physical wheel's driver and keep
hands clear on startup. Select the intended FFB device, restart GT5, then drive
briefly and test pause/exit. If forces pull in the wrong direction, stop the
test and check the existing reverse-effects setting. Do not judge calibration
from mock tests. Also retest the pedal correction and clutch.

With `T500RS: Trace`, the log now reports PS3 detection, host haptic readiness
and capabilities, and per-slot create/update/run results. A missing SDL haptic
handle produces a warning. Preserve `log/RPCS3.log` after exit and remove Trace
overrides after testing. These diagnostics distinguish guest protocol errors
from host-driver failures.
