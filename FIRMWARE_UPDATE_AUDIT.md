# Firmware reliability update audit

## Audited inputs

- Source snapshot: `RP2350-HID-bridge-main(3).zip`
- Snapshot archive comment / upstream revision: `681772187307889b306bfe90638d94170c680b86`
- Requirements: `RP2350_HID_Bridge_Firmware_Update_Handoff(1).md`

The handoff was checked against the implementation in `src/main.c`, TinyUSB
configuration, descriptors, build configuration, and README before changes were
made.

## Corrections to the handoff

1. **Periodic status was intended, but probably not executing.** The old main
   loop used `absolute_time_diff_us(last, now) <= -1000000`. Pico time
   differences are positive when `now` follows `last`, so this condition was
   reversed. The block was removed as requested, but it was probably not the
   source of observed status lines.
2. **The action array has 512 slots but usable ring capacity is 511.** One slot
   is reserved to distinguish a full ring from an empty ring. Capacity checks
   and documentation now use the usable value.
3. **A 512-byte TinyUSB TX buffer alone does not cover response bursts.** It
   fixes one long response but can still run out when several commands are read
   before USB drains. The update combines the larger TinyUSB buffer with a
   non-blocking 2 KiB firmware TX queue and RX backpressure. Protocol lines are
   queued whole, never partially.
4. **Documenting smooth step size was not enough to preserve displacement.**
   The old planner calculated an arbitrary integer step, clamped it to signed
   8-bit inside `enqueue_report_after`, and then advanced its internal position
   by the *unclamped* value. `final_correct` therefore could not recover lost
   motion. Large generated deltas are now split into exact signed 8-bit reports.

## Additional issues found and fixed

- Overlong CDC input previously reset the buffer and then parsed the trailing
  fragment as a new command. It now emits one `ERR LINE_TOO_LONG` and discards
  everything through the newline.
- Queue deadlines used a raw 32-bit millisecond comparison that becomes wrong
  around the 49.7-day counter wrap. They now use Pico SDK absolute deadlines.
- Composite commands accepted numeric syntax through `sscanf`, allowing extra
  text and risking integer overflow or extremely long enqueue loops. Numeric
  commands now use bounded, exact integer-token parsing.
- `CLICK` could partially enqueue and could derive its eventual button state
  from the current point inside a drag rather than the queue's projected final
  state. It now preflights/rolls back atomically and uses projected state.
- Explicit `RELEASE`, `PRESS`, or `BUTTONS` during a queued drag did not cancel
  that drag, so later queue entries could overwrite the requested button state.
  These explicit state commands now preempt scheduled mouse work.
- `rand_range` and jitter delay arithmetic could overflow at extreme accepted
  integer values. The range and delay arithmetic are now widened and bounded.

## Implemented reliability behavior

- Accepted HID-changing/scheduling commands and `HEARTBEAT` re-arm the
  two-second watchdog. Diagnostic queries do not.
- `CLICK`, `MOVE_SMOOTH`, and `DRAG` are transactional on queue failure.
- A rejected preemptive smooth move or drag leaves no new queued prefix; a
  rejected drag cannot own or press its requested button.
- Immediate `MOVE` and `SCROLL` retain their signed 8-bit clamp.
- Smooth paths split large deltas and preserve total movement when the complete
  operation fits.
- `STATUS` is request-driven. `WATCHDOG RESET` remains an asynchronous safety
  event.
- `QUEUE?` reports depth plus active/idle state.
- CDC response draining is non-blocking so a slow CDC consumer cannot stall the
  watchdog or HID scheduler.

## Automated verification completed

Run:

```bash
tests/run_host_tests.sh
```

The host suite exercises:

- complete long CDC `STATUS` and `BOARD?` responses through a simulated 7-byte
  write window;
- repeated watchdog trip, command re-arm, and second trip/release;
- atomic click, smooth-motion, and drag queue failures;
- exact 1,000-count smooth displacement split over signed 8-bit reports;
- whole-line discard after CDC input overflow;
- strict numeric command parsing and range rejection;
- button-state preemption of an active drag;
- `QUEUE?` active/idle results;
- queue timing beyond the old 32-bit millisecond wrap point.

## Verification still required on hardware

This development environment did not contain the Pico SDK, CMake, or the ARM
embedded compiler, so it could not produce or flash a UF2. Before deployment:

1. Build with the documented Pico SDK workflow.
2. Flash the Waveshare RP2350-USB-A and confirm automatic selection of DP 12 / DM 13.
3. Repeat the handoff's CDC completeness, no-status-spam, keyboard, mouse,
   repeated-watchdog, queue-failure, relative-range, and `QUEUE?` loopback tests.
4. End every physical test session with `RESET` and verify all target inputs are
   released.

High-level action plans, approval policy, capture/vision, cursor tracking, and
Hermes integration remain controller-side and were intentionally not added to
this firmware.
