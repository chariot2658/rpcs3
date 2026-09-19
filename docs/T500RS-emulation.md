# Experimental Thrustmaster T500RS USB emulation

This patch adds a selectable T500RS guest identity backed by RPCS3's existing
SDL wheel mappings. It is a substantial implementation, **not a verified GT5
compatibility fix**. No PS3 game, physical wheel, or complete RPCS3 build was
available during development. USB/protocol tests and mocked host integration
checks are described below.

## Apply and configure

The patch targets RPCS3 commit
`8db660b185496f115701ef4c77c1ca2bef60e422`.

```sh
git clone --recursive https://github.com/RPCS3/rpcs3.git
cd rpcs3
git switch -c t500rs-emulation 8db660b185496f115701ef4c77c1ca2bef60e422
git submodule update --init --recursive
git apply --check /path/to/rpcs3-t500rs.patch
git apply /path/to/rpcs3-t500rs.patch
```

Build RPCS3 normally with SDL3 enabled. Both CMake and the Visual Studio
emucore project include the new sources; no-SDL builds retain passthrough.
There is no prebuilt executable in this package.

1. Open **Configuration > Emulated Wheel (Logitech / Thrustmaster)**.
2. Enable emulation and select **Thrustmaster T500RS (experimental)**.
3. Map steering, three pedals, two paddles, buttons and D-pad. These are the
   existing SDL mappings, including mappings across different host devices.
4. Select the physical device used for force feedback. Input-only operation is
   supported when no haptic device is selected.
5. Match **Host wheel rotation range** to the range configured in your real
   wheel's driver, e.g. 900 degrees for a wheel configured for 900 degrees.
6. Start with **Captured Windows firmware** protocol. This uses captured USB
   field positions. The alternative **hid-tmff2 reference driver** follows the
   differing PR #223 encoder, including its streamed periodic/ramp carrier.
7. Save and restart the game. Only the selected guest wheel is instantiated.
   As with existing wheel emulation, map the wheel here rather than also
   exposing the same inputs as an emulated gamepad.

The configuration remains in `LogitechG27.yml` so existing mappings are reused.
New keys: `t500rs: true`, `t500rs_protocol: 0` (captured) or `1` (PR encoder),
and `t500rs_host_range: 1080`. The model and protocol require a game restart;
host mappings, inversion, direction encoding and host range refresh at runtime.

In this tree, the existing force-feedback gain, steering deadzone and steering
smoothing controls remain available for Logitech emulation. They are disabled
when T500RS is selected; the T500RS backend handles guest-requested gain itself.

## Implemented behavior

- Direct enumeration as `044f:b65e`, including captured device/configuration/HID
  descriptors, strings, IN `0x82`, OUT `0x01`, and 15-byte report `0x07`.
- 16-bit steering, 10-bit separate pedals, 13 button bits and 8-way hat.
  Neutral input has released pedals and hat `0x0f`; byte layout and neutral
  polarity match the capture. Named button assignments and pedal roles still
  need game validation; each binding can be changed in the mapper.
- Captured vendor queries `0x42`, `0x48`, `0x49`, `0x4e`, `0x55`, `0x56`,
  attachment queries `0a 04 90 03`, `0a 04 12 10`, `0a 04 00 06`, queued
  report `0x14` replies, and the `41/48/0040` input-mode acknowledgement.
- Standard descriptor/status/configuration/interface requests; HID GET_REPORT,
  SET_REPORT, GET/SET_IDLE, GET/SET_PROTOCOL. Both HID output SET_REPORT and
  interrupt OUT feed the same decoder. The descriptor advertises report
  protocol only; boot-protocol requests are rejected.
- 16 independent hardware slots, parameter/envelope staging before MAIN,
  upload/live update/start/stop/reset, subtype low-byte wrap for higher slots,
  gain, autocenter and software steering-range scaling.
- Native constant force, spring and the shared `0x41` condition family. The
  latter is represented by SDL damper: the reference puts damper, friction and
  inertia on the **same** wire type and does not provide a discriminator.
- PR-format `0x04` streamed force, preserving any guest-synthesized waveform or
  ramp. No waveform reconstruction is necessary for this path.
- Captured native periodic parameters translated to SDL periodic effects;
  square waves have a software fallback when the host lacks that effect.
  Their interpretation is explicitly provisional (see below).
- Envelopes and timing translated using the selected interpretation; Linux
  reference mode leaves duration enforcement to explicit guest STOP commands.
- A 4 ms haptic worker coalesces force updates. Guest OUT callbacks never call
  the physical FFB driver. Unchanged effects are not re-uploaded; live updates
  do not restart effects. STOP/START and reset/redeclare transitions retain
  generation counters across coalescing.
- Software global gain works without host gain support. Autocenter uses the
  host API or a spring fallback. Stopped effects release scarce host slots.
- Pause/input-suppression handling, host-haptic replacement, physical steering
  detach/reattach, device shutdown, and deconfiguration stop forces. Physical
  steering disconnection resets the guest protocol session to avoid restoring
  stale torque when reconnected.
- Bounded USB replies, short-read support, truncated-command rejection,
  bounded reply queues, explicit failures for unknown operations, one warning
  per unknown request/command class, and full OUT/control logging at Trace.

## Evidence and uncertainty

The earlier assumption that the PR fully specifies the real wheel is too
strong. The PR encoder and the author's archived Windows USB packets disagree.
This patch retains two explicit interpretations rather than blending fields.

| Area | Evidence / limitation |
|---|---|
| USB identity, descriptors and neutral report | Exact bytes extracted from `plug_t500_in.pcapng`; tests compare them directly. |
| Vendor and rim identification | Captured replies, including firmware `0x2f`. These reproduce one observed firmware/rim identity. |
| Streamed force | PR #223 packed structure: level is byte **4**, length 8. The Markdown offset table is inconsistent. |
| MAIN timing | Both use duration at 4; PR delay at 6; Windows capture suggests delay at 7 (`0xffff` treated as no delay). Windows timing interpretation is not hardware-validated. |
| Envelope | PR attack-length/level at 2/4, fade at 5/7; captured packets indicate 3/5 and 6/8. Native level scale 127 is inferred from recorded maximum values. |
| Native periodic | Captures explicitly contain per-slot `0x22` MAIN and `0x04` parameter packets, despite newer PR comments saying they cannot occur. Amplitude at 3, signed offset at 4, phase at 5, period at 6 are an interpretation of those packets. Period-as-ms, phase scaling, and waveform selectors `0x20/21/23/24` remain provisional. `0x22` is directly observed; no claim is made that every waveform is verified. |
| Condition coefficients | PR maps positive coefficients to 0..10; archived manual dumps also contain negative signed coefficients. Captured mode uses signed /127 scaling, reference mode positive /10. Captured coefficient scaling is provisional. |
| Condition deadband | `/65` in the reference is itself marked unconfirmed. The inverse is bounded and tested but not physically calibrated. |
| Friction versus inertia versus damper | Indistinguishable in the supplied wire format; all map to damper. There are no fabricated distinct opcodes. |
| Physical rotation stops | SDL offers no portable wheel-range command. Only guest steering input is rescaled. Set the physical range in the wheel driver. |
| PS3 initialization | Windows enumeration is reproduced. GT5 may issue additional PS3-specific requests; these are logged and rejected rather than given fabricated successful replies. |

Not included: the initial `b65d` boot personality, firmware programming,
standalone TH8RS/TH8A gear-shifter USB emulation, F1-rim variants, or a physical
force mixer for hosts with too few effect slots. Extra Logitech shifter/dial/LED
bindings do not become T500RS base features. HID idle rates are stored and
reported; interrupt input is still refreshed at the endpoint polling interval.

A real T500RS is **not required** for the next useful test: run GT5 with your
ordinary SDL-compatible wheel, see whether the guest recognizes the virtual
T500RS, then inspect the `T500RS` log channel. No GT5 recognition or successful
force-feedback session has been observed for this patch yet.

## Validation

Standalone tests use the real protocol code:

```sh
bash rpcs3/tests/t500rs/run_tests.sh
```

The runner uses C++20, `-Wall -Wextra -Werror -pedantic`, AddressSanitizer and
UndefinedBehaviorSanitizer. It covers captured descriptor/input bytes,
initialization replies, constant and periodic uploads, parameters-before-MAIN,
stream offsets, negative saturation, all 16 slots, per-slot START/STOP,
truncation, and 100,000 deterministic random packets. This passed.

Development also compiled the real modified `LogitechG27.cpp` and new SDL
adapter against RPCS3's pinned SDL headers, with explicit test-only RPCS3
shims and a mocked SDL backend. The integration suite passed bounded control
and interrupt transfers, identification, control/OUT parity, gain, autocenter,
pause/resume, live updates without restart, coalesced STOP/START, host update
failure handling, and reset/deconfiguration. The additional harness is in the
package's `validation/` directory; it is not linked into RPCS3.

This is **not** a complete RPCS3/Qt build, a real SDL driver test, a thread race
test, or a GT5/GT6 compatibility test. LeakSanitizer cannot run inside the
execution environment's ptrace wrapper; `ASAN_OPTIONS=detect_leaks=0` was used
there, while address and undefined-behavior instrumentation remained enabled.

## Pinned sources

- RPCS3 base: https://github.com/RPCS3/rpcs3/tree/8db660b185496f115701ef4c77c1ca2bef60e422
- PR stack: https://github.com/Kimplul/hid-tmff2/pull/223
- Reference encoder: https://github.com/cazzoo/hid-tmff2/tree/f4eed4ecb104c6c148774d72ae519d1b2c422e61/src/tmt500rs
- Archived captures: https://github.com/cazzoo/hid-tmff2/tree/92315ba6145aebe01e5efd7442664873bf231e89/captures
- HID/descriptor capture: `plug_t500_in.pcapng`, Git blob `088abdefb828c0ec31938b6bb11d1db85d61ccaa`.
- Periodic fixtures: `ctl_panel_boing.pcapng`, blob `097dadcde89e6e15f177e0ffef9755237111b602`;
  `ctl_panel_bumpy_road.pcapng`, blob `c52583a537fed8d25d35519a1dde6b64d10c835f`.
- Constant fixture: `device_const_force_left_start_stop_multiple_times.pcapng`, blob `dbad8e4cf74931212d662a7b0a7a439f51e8a65b`.
- SDL submodule: https://github.com/libsdl-org/SDL/tree/fa2c02bb6e21974a89ea9824bc53c9932abe5f9c

Protocol knowledge comes from Casimir Bonnet's T500RS reverse engineering and
captures, with RPCS3's existing Logitech code providing host mapping/lifetime
infrastructure. New source files use GPL-2.0-or-later, compatible with RPCS3.
