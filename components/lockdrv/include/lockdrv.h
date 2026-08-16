/**
 * lockdrv — HSJ08H lock-actuator driver (ES24F project, stage 5)
 *
 * Drives the OEM door-lock actuator: a reversible geared DC motor
 * (clutch-pin / bolt style, "semi-automatic lock motor" class), controlled
 * by an HSJ08H single-channel H-bridge (pin-compatible with
 * MX608E / HR1124S / TC118S / YX9020AM / LGM9680).
 *
 * OEM behavior (scope-proven, see HANDOFF_Solenoid_HSJ08H.md):
 *  - open:  INA high for ~235 ms (OUTA high, OUTB low) -> actuator extends
 *  - close: INB high for ~235 ms (OUTB high, OUTA low) -> actuator retracts
 *  - boot:  one retract pulse to reach a known rest state (open-loop control)
 *  - release is always COAST (both inputs low); no brake, no PWM
 *  - the ~4.6 s open-window dwell is application logic (lock_app), NOT here
 *
 * Safety design:
 *  - Pulse termination is guaranteed by a gptimer alarm ISR: a hung task can
 *    never leave the motor energized (pulse timeout by construction).
 *  - The state machine (IDLE -> PULSING -> GUARD -> IDLE) rejects re-triggers
 *    while a pulse or its guard window is active.
 *  - The API exposes directions, never raw pins: INA and INB high
 *    simultaneously (H-bridge brake) is impossible through this API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOCKDRV_DIR_EXTEND = 0,  /**< INA pulse: actuator extends (open window) */
    LOCKDRV_DIR_RETRACT,     /**< INB pulse: actuator retracts (rest/home)  */
} lockdrv_dir_t;

typedef enum {
    LOCKDRV_ST_IDLE = 0,     /**< coast, ready for a new pulse            */
    LOCKDRV_ST_PULSING,      /**< a pulse is being driven                 */
    LOCKDRV_ST_GUARD,        /**< pulse finished, no-retrigger window on  */
} lockdrv_state_t;

typedef struct {
    gpio_num_t ina_pin;  /**< SOL1 net (ex FR8018H PA4) */
    gpio_num_t inb_pin;  /**< SOL2 net (ex FR8018H PA5) */
    uint32_t   pulse_ms; /**< 0 = Kconfig CONFIG_LOCKDRV_PULSE_MS (OEM: 235) */
    uint32_t   guard_ms; /**< 0 = Kconfig CONFIG_LOCKDRV_GUARD_MS           */
} lockdrv_config_t;

/**
 * Initialize the driver: GPIOs (both LOW = coast), pulse timer and guard
 * timer. Does NOT move the actuator by itself, except when
 * CONFIG_LOCKDRV_HOME_ON_INIT=y, which schedules one retract pulse
 * CONFIG_LOCKDRV_HOME_DELAY_MS after init (OEM boot parity).
 */
esp_err_t lockdrv_init(const lockdrv_config_t *cfg);

/** Retract pulse to the known rest position (OEM boot/home behavior). */
esp_err_t lockdrv_home(void);

/** Extend pulse: opens the access window (INA, OEM ~235 ms). */
esp_err_t lockdrv_open(void);

/** Retract pulse: back to rest (INB, OEM ~235 ms). */
esp_err_t lockdrv_close(void);

/**
 * Emergency stop: cuts any active pulse immediately and forces coast.
 * Safe to call from fault paths; also returns the driver to IDLE.
 */
esp_err_t lockdrv_abort(void);

/** true while a pulse or its guard window is active. */
bool lockdrv_is_busy(void);

/** Current state-machine state. */
lockdrv_state_t lockdrv_get_state(void);

/** "idle" | "pulsing" | "guard" — for console diagnostics. */
const char *lockdrv_state_str(void);

/**
 * TEST HOOK (bench only): raw pulse of arbitrary width. Bypasses the guard
 * window so margin sweeps can run back-to-back. Never use from lock_app.
 */
esp_err_t lockdrv_test_raw_pulse(lockdrv_dir_t dir, uint32_t pulse_ms);

#ifdef __cplusplus
}
#endif
