# lockdrv_test — Bench harness for the lockdrv library (ES24F project, stage 5)

Bench validation harness for the `components/lockdrv` library, the driver of
the OEM lock actuator of the **ELOCK ES242F (Tuya) smart lock, retrofitted
with an ESP32-S3**. The original board drives the actuator (a reversible
geared DC motor — clutch-pin style, NOT a solenoid) through an **HSJ08H**
single-channel H-bridge, pin-compatible with MX608E / HR1124S / TC118S /
YX9020AM / LGM9680.

All OEM behavior below is scope-proven; evidence lives in
`HANDOFF_Solenoid_HSJ08H.md` and the `lockdrv_*.png` captures.

**Status (2026-08-16): bench validation PASSED.** Build + flash OK on the
pinned toolchain (ESP-IDF v6.1-dev-6940-g08e0d30a74a). Full acceptance suite
green; waveforms match the OEM captures (see below).

![Test lab](docs/images/lockdrv_testlab.png)

## OEM behavior being replicated (D2 parity)

| Fact | Value | Evidence |
|---|---|---|
| Open pulse | INA high, **~235 ms** → OUTA high / OUTB low → actuator extends | `lockdrv_open_pulse_235mS.png` |
| Close pulse | INB high, ~235 ms → OUTB high / OUTA low → actuator retracts | `lockdrv_open_close_pulses_4.8Seg.png` |
| Auto-relock dwell | **~4.6 s** (4.82 s edge-to-edge), CPU-generated → app-level | same capture |
| Boot home pulse | one INB retract pulse at power-up, same 235–240 ms | `lockdrv_powerup_240mS.png` |
| Release | always **coast** (both inputs low); no brake, no PWM | flyback tail visible |
| Event matrix | card = keypad = Tuya remote: identical pulses | user-verified |
| Current | ~50–90 mA per pulse (bounded; MX608E margin ×10) | `lockdrv_pulse_current.jpeg` |
| Supply | VBAT = USB/battery direct (no series diode) | `lockdrv_VBAT_5V_USB.png` |

## Bench hardware

| Signal | GPIO | Destination |
|---|---|---|
| INA (SOL1) | GPIO7 | HSJ08H pin 2 (scope wire already soldered) |
| INB (SOL2) | GPIO8 | HSJ08H pin 3 (scope wire already soldered) |
| GND | — | board GND |

- **FR8018H held in reset (GPIOs hi-Z), NOT removed** — same approach as the
  audio bench.
- GPIO7/8 chosen to keep GPIO4/5/6 free for the existing audio bench wiring.
- Board powered by USB (5 V) for now; repeat key captures at battery (~6 V).

## Firmware

- **`components/lockdrv`** (vendored copy; copy it into the main repo's
  `components/` when integrating): 2 GPIOs + one **gptimer** whose alarm ISR
  terminates the pulse — a hung task can never leave the motor energized.
  State machine `IDLE → PULSING → GUARD → IDLE` rejects re-triggers.
  All timings in `components/lockdrv/Kconfig` (D11: no build flags).
- Console REPL over UART0, 115200 8N1, prompt `lockdrv> `.

| Command | Description |
|---|---|
| `home` | retract pulse (OEM boot behavior) |
| `open` | extend pulse, OEM parity |
| `close` | retract pulse, OEM parity |
| `cycle [dwell_ms]` | open + dwell + close (default 4600 ms = OEM auto-relock) |
| `pulse <ext\|ret> <ms>` | raw pulse, custom width, bypasses guard (bench only) |
| `sweep <ext\|ret> <min> <max> <step>` | width margin sweep, 1 s between pulses |
| `stress <n>` | n open/close cycles; mid-pulse retrigger must be rejected |
| `abort` | force coast immediately |
| `state` | state dump |

## Acceptance tests — results (2026-08-16, on the lock board + test lab)

| # | Test | Result | Evidence |
|---|---|---|---|
| 1 | `open` on scope = OEM open pulse | **PASS** — 237 ms measured vs 235.5 ms OEM (cursor placement tolerance; configured value 235 ms) | `docs/images/lockdrv_open_237mS.png` |
| 2 | `close` on scope = OEM close pulse | **PASS** — 235 ms | `docs/images/lockdrv_close_235mS.png` |
| 3 | `cycle 4600` = OEM auto-relock cycle | **PASS** — 4.7 s measured vs 4.58–4.82 s OEM | `docs/images/lockdrv_cycle_dwell_4600mS.png` |
| 4 | Boot home pulse | **PASS** — one retract pulse after reset, identical shape to the OEM power-up capture | (same shape as close; not re-captured) |
| 5 | `stress 20` | **PASS** — 20/20 cycles, 20/20 mid-pulse retriggers rejected (`ESP_ERR_INVALID_STATE`), 0 failures; open→close spacing 107 ms ≈ 235 pulse + 100 guard + polling | `docs/images/lockdrv_stress_dwell_100mS.png` |
| 6 | `abort` | **PASS (harness v1.3)** — cancels any running `cycle`/`sweep`/`stress` and forces coast. To test mid-pulse from the console: `pulse ext 2000`, then `abort` while it is still high — the scope shows the pulse cut at the abort instant | console log |
| 7 | Idle draw < 1 µA (coast) | not re-measured — guaranteed by design (both inputs low → MX608E standby < 0.1 µA) | — |

### `abort` semantics (clarified after the first bench run)

`abort` is a **driver-level panic button**: it terminates any active pulse at
the hardware level and forces coast. Harness v1.2 also cancels the background
`cycle`/`sweep`/`stress` task. Notes:

- Aborting during a `cycle` dwell cancels the pending close — the actuator
  may stay **extended** (de-energized, safe). Issue `close`/`home` to rest it.
- A 235 ms pulse is too fast to abort by typing; use a long raw pulse
  (`pulse ext 2000`) to test the mid-pulse path.
- First bench run (harness v1.0) confirmed `abort` is harmless when the
  driver is idle and does NOT implicitly cancel the cycle task — v1.2 fixed
  that coupling, and v1.3 fixed a stale-handle bug in v1.2 (cycle task did
  not clear its handle; aborting afterwards deleted a dead task -> panic).
