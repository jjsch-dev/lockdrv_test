# lockdrv — HSJ08H lock-actuator driver (ES24F project, stage 5)

Driver for the OEM door-lock actuator: a **reversible geared DC motor**
(clutch-pin style, "semi-automatic lock motor" class) behind an **HSJ08H**
single-channel H-bridge (pin-compatible family: MX608E / HR1124S / TC118S /
YX9020AM / LGM9680 — MX608E datasheet is the reference).

## Hardware facts (scope-proven, see HANDOFF_Solenoid_HSJ08H.md)

- Control: two static direction inputs. `INA=H` → OUTA high (extend),
  `INB=H` → OUTB high (retract), both low → coast (Hi-Z, < 0.1 µA standby).
  Both high = brake — **never used by the OEM**, unreachable through this API.
- Pulses: flat DC, **~235 ms** in both directions, no PWM hold-current.
- Boot: OEM issues one retract pulse at power-up (open-loop control needs a
  known start position). Replicated when `CONFIG_LOCKDRV_HOME_ON_INIT=y`.
- The ~4.6 s auto-relock dwell is **application logic** (`lock_app`), not
  part of this driver.

## Design

- **2 GPIOs + 1 gptimer.** The pulse ends in the gptimer alarm ISR, so pulse
  termination does not depend on any task staying alive.
- State machine `IDLE → PULSING → GUARD → IDLE`: re-triggers during a pulse
  or its guard window are rejected with `ESP_ERR_INVALID_STATE`.
- All pre-compile parameters in `Kconfig` (D11).

## API

See `include/lockdrv.h`: `lockdrv_init/home/open/close/abort/is_busy/
get_state/state_str`, plus the bench-only hook `lockdrv_test_raw_pulse()`.

## Kconfig

| Option | Default | Note |
|---|---|---|
| `LOCKDRV_PULSE_MS` | 235 | OEM parity (235–240 ms measured) |
| `LOCKDRV_GUARD_MS` | 100 | our protection, no OEM equivalent |
| `LOCKDRV_HOME_ON_INIT` | y | OEM boot parity |
| `LOCKDRV_HOME_DELAY_MS` | 500 | NOT OEM-measured; adjust when captured |

## Bench harness

`lockdrv_test/` (sibling project) — console REPL: `open`, `close`, `home`,
`cycle`, `pulse`, `sweep`, `stress`, `abort`, `state`.
