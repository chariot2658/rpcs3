# GT5 2.11 force-feedback implementation evidence

The user's 2026-09-25 test confirms buttons and steering with the revision-2
input changes. Throttle and brake were exchanged; the encoder now sends
brake in analog channel 0 and throttle in channel 1. Clutch is not verified.

The local RPCS3.log (57,947,917 bytes, last write 2026-09-25 01:09:09 local time)
contains 1,126 opcode-05 updates and 102 opcode-03 updates, but the emulator
rejects the PS3 MAIN declarations and START commands. This is sufficient to
explain missing game effects regardless of whether the host haptic device
opened successfully. The log does not independently establish host FFB health.

## Observed commands

| Purpose / evidence | Example | Current obstacle |
|---|---|---|
| Slot 0 declaration, referencing opcode 03 parameters | `01 00 01 40 FF FF 00 FF FF 00 00 10 00 00 00` | Type `01` unsupported |
| Slot 1 declaration, referencing opcode 05 parameters | `01 01 07 40 FF FF 00 FF FF 20 00 30 00 00 00` | Type `07` unsupported |
| Slot 2 declaration, referencing opcode 05 parameters | `01 02 08 40 FF FF 00 FF FF 40 00 50 00 00 00` | Type `08` unsupported |
| Initial declarations | Same types, byte 3 and timing fields zero | Parser requires byte 3 to equal `40` |
| START slots 0, 1, 2 | `41 01 01 01` (slot 1) | Parser accepts START byte `41`, not `01` |
| STOP | `41 00 00 01`, `41 02 00 01` | Supported, but no valid declaration/start preceded it |
| Signed constant level | `03 00 00 FF` | Parameter stored; referenced effect never declared |
| Condition update | `05 20 00 2E 2E FE FF 00 00 24 24` | Parameter stored; referenced effect never declared |
| Global gain | `43 80` | Accepted with PC-derived scaling; PS3 scaling needs tracing |
| Additional control | `81 00 00` | Unsupported; meaning not established |

The condition updates dominate this trace. Implementing only a constant-force
alias would not reproduce all requested effects. The host configuration selects
an FFB device and enables emulation; missing declaration/start support is a
confirmed protocol problem, not evidence that a physical T500RS is required.

## Targeted next work

1. Trace the supplied GT5 binary's force wrappers into the packet builders.
   `0x00ADF080` feeds slot 0; `0x00ADED9C` and `0x00ADED70` pass types 7 and 8
   into `0x00ADEA50`. Establish their force semantics before assigning SDL
   spring/damper kinds. Trace play/stop through `0x00AE1174` and MAIN flags
   through `0x00AE1604`, including the initial zero flags.
2. Recover units, signedness, and rounding from the wrappers. For example,
   `0x00ADEB1C` onward maps condition coefficients from -10000..10000 to
   -100..100, center to -500..500, deadband to 0..1000, and saturation to
   0..100 before calling `0x00AE1814`. These are guest-side conversions;
   the existing PC-derived /127 coefficient and center*20 conversions must
   not automatically be reused as a verified PS3 physical interpretation.
3. Add a PS3 protocol interpretation with explicit identification/selection.
   Preserve PC capture and Linux-reference behavior. Cover MAIN flags/timing,
   parameter references (including their high byte), START/STOP/retrigger,
   gain range, and supported effects. Trace `0x81` via `0x00ADF26C` and
   `0x00AE1EE0`; do not silently acknowledge unknown nonzero force commands.
4. Translate decoded effects into the existing SDL haptic worker. It already
   provides constant/condition effects, updates, pause handling and teardown.
   Add diagnostics for host opening/capabilities and effect create/update/run
   results so guest protocol success can be distinguished from host failure.
5. Turn the observed startup/driving/stop sequence into regression fixtures.
   Verify signed levels, scaling limits, simultaneous slots, live updates,
   coalesced stop/start, reconfiguration, pause and unplug. Then perform a
   bounded-strength driving test on the user's host wheel to check direction,
   centering/damping, curb response and stopping forces on pause/exit.

Source: user-supplied `GT5_2.11_T500RS_analysis_v2/thrustmaster_module.asm` and
`input_init_evidence.asm`, BCAS20108 2.11, plus the local runtime log. No claim
of working GT5 force feedback or original-wheel force calibration is made.
