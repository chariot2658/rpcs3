# GT5 2.11 force-feedback implementation evidence

The user's 2026-09-25 test confirms buttons and steering. The reported pedal
exchange is corrected separately: brake bytes 3..4, throttle 5..6, clutch 7..8.
The 2026-09-29 driving test produced vibration but very light steering; the
spring correction below addresses that and needs another physical-wheel test.

## Firmware scales (2026-09-30)

Static analysis of the T500RS firmware v47 (a PIC24FJ GB USB controller that
also evaluates effects and drives the motor PWM) replaced the inferred scales
of the PS3 path. The firmware itself is not distributed.

- Spring: force / full torque = `dx * c * 4r / (90 * 127)`, where `dx` spans
  +-512 over the half guest range and `r = guest range / 1080`. In degrees,
  force / full is about `c * degrees off center / 3015` at any range. One host
  slope over the half guest range is coefficient `6027.5 / guest range`:
  6.7 at 900 degrees, 5.6 at 1080 degrees. The previous value was 10.
- Saturation `s` limits condition force to `s / 127` of full torque, not
  `s / 100`. Springs are additionally limited to
  `4r * min(100, s + s/8 + s/16) / 127`, which only matters below about 340
  degrees.
- Constant level `L` becomes `255 * L / 127`, so |L| = 64 is full torque.
  Envelope levels use the same scale. GT5's -13..10 road texture is therefore
  about +-20% torque, not +-10%. Direct force (opcode `81`) stays `L / 127`.
- Damper coefficients act on steering speed, whose units could not be
  recovered, so coefficient 10 remains one SDL slope. Damper saturation uses
  the firmware's `s / 127`.

The firmware sums all effects into one +-127 clamp before gain; SDL mixes
effects in the host driver instead. The firmware also negates constant force
on the steering axis relative to condition force; the absolute motor direction
cannot be read from the code and is not changed here. The sections below
describe the earlier inferred model, now superseded for these scales.

## Spring servo correction (2026-09-29 log)

During driving GT5 keeps slot 1's spring running and sends about 60 opcode-05
updates per second. Its center follows the steering word GT5 received:
across 1,467 samples, `center / 500` matched the guest steering fraction
(median ratio 0.993). The offset between center and wheel carries the aligning
torque. Coefficients were typically 45..72 with saturation close to 0.79 times
the coefficient. Constant force stayed within -13..10 of 127, which explains
the remaining vibration.

The previous translation made two mistakes:

- It mapped center 500 to the host's full axis. With a 900-degree guest range
  and a 1080-degree host, the center was placed about 20% beyond the wheel,
  so 35% of off-center samples pushed outward.
- It mapped coefficient 100 to one SDL slope. hid-tmff2's Windows-derived
  scale is 10 per full slope, and GT5's saturation/coefficient ratio then puts
  saturation within about 8% of the guest range. SDL/DirectInput cannot
  express more than one slope.

`place_condition()` now converts PS3 conditions into host units. Center and
deadband are scaled by guest range / host range; coefficient 10 was one host
slope (now `6027.5 / guest range`, see above). If a spring needs more than one slope and steering is mapped to an axis
of the FFB device, the adapter runs a full-slope host spring around a virtual
center every 4 ms. The host force at the sampled position equals the modeled
wheel force, bounded by GT5's saturation. Otherwise the coefficient is clamped
to one slope. A reversed steering mapping mirrors center, coefficients and
saturations. Force reversal no longer negates PS3 condition coefficients,
because negative spring/damper coefficients repel or add energy.

Replaying the log's driving spring updates with the logged steering gives a
median restoring torque of 12.5% of maximum (90th percentile 48%), versus 1.9%
(8.3%) before. The coefficient scale and center interpretation are inferred
from GT5 behavior and the PC reference, not from a real T500RS measurement.

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
  `0x00ADEA50`. The `{value, name}` enum table at `0x016B9CB8` (8-byte
  entries) gives `THRUSTMASTER_SPRING_X_POS_K` ID 0x1F (entry `0x016B9D58`)
  and `THRUSTMASTER_DAMPER_X_POS_K` ID 0x30 (entry `0x016B9DE0`). These enum
  IDs are not wire parameter references; slot 1's spring block is 0x20.
  The corresponding caller loads feed slot 1 spring and slot 2 damper.
- `0x00ADEB1C` onward scales condition coefficients to signed -100..100,
  center to -500..500, deadband to 0..1000 and saturation to 0..100.
  Builder `0x00AE1814` packs coefficients at 3/4, LE center at 5, LE deadband
  at 7, saturation at 9/10. Center and deadband are relative to the guest
  rotation range (500 and 1000 are half the range); see the spring servo
  correction above. Original-wheel feel is unverified.
- MAIN builder `0x00AE1604` establishes field positions above. START caller
  `0x00AA0058` and builder `0x00AE1174` establish enable/retrigger and count 1.
- Gain builder `0x00AE214C` clamps to 128; the caller also supplies 128.
- Envelope builder `0x00AE1D14`: reference at 1, attack length at 3, level at
  5, fade length at 6, level at 8. Levels use 0..127.
- FORCE_X/Y enum IDs 0x0F/0x10 feed `0x00ADF26C` through caller
  `0x00AA0F44`, then builder `0x00AE1EE0` emits opcode 81 and signed X/Y,
  replacing -128 with -127. Direct force lifetime is modeled as persistent
  until zero/reset; this path's nonzero physical behavior needs validation.

## Validation and next driving test

Protocol and mocked USB/SDL suites pass with ASan/UBSan, including signed
scales, upper-slot references, parameters before MAIN, finite duration/delay,
envelopes, live updates without restart, gain, simultaneous effects, direct
force, pause/input suppression, host failures, reconfiguration and unplug.
Both PC fallbacks remain covered, as do 100,000 deterministic random packets.
Spring regressions use the logged 900/1080-degree values: range conversion,
virtual-center force below and at saturation, soft native springs, deadband,
dampers, mirrored mappings and the fallback without FFB-device steering. The
mocked adapter test moves the SDL steering axis and checks live updates without
restarting the effect.
A replay of all 1,254 OUT packets from the user's driving log has zero rejected
packets. Acceptance alone does not prove force fidelity or host-driver support.

No new analysis tools or MCP installation was needed. Capstone was available
for targeted checks beyond the supplied disassembly. No local full RPCS3 build,
physical FFB test or thread-race test was performed for this change.

For the next test, reduce strength in the physical wheel's driver and keep
hands clear on startup. Steering must be mapped to an axis of the selected FFB
device for full spring stiffness; the log reports which mode is active. Drive
briefly and test pause/exit. The spring should now restore firmly toward
center. If the wheel pulls toward full lock or oscillates, stop immediately and
report whether the steering mapping is reversed. The reverse-effects setting no
longer changes PS3 springs or dampers. Also retest the clutch.

With `T500RS: Trace`, the log now reports PS3 detection, host haptic readiness
and capabilities, and per-slot create/update/run results. A missing SDL haptic
handle produces a warning. Preserve `log/RPCS3.log` after exit and remove Trace
overrides after testing. These diagnostics distinguish guest protocol errors
from host-driver failures.
