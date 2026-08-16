/**
 * lockdrv — HSJ08H lock-actuator driver (implementation). See lockdrv.h.
 *
 * Timing model:
 *  - pulse end  -> gptimer alarm ISR (hard guarantee, cache-safe)
 *  - guard end  -> esp_timer one-shot (policy only, not safety-critical)
 */
#include "sdkconfig.h"

#include "lockdrv.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gptimer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"

#include "freertos/FreeRTOS.h"

static const char *TAG = "lockdrv";

static struct {
    bool               inited;
    gpio_num_t         ina;
    gpio_num_t         inb;
    uint32_t           pulse_ms;
    uint32_t           guard_ms;
    gptimer_handle_t   pulse_timer;
    esp_timer_handle_t guard_timer;
    esp_timer_handle_t home_timer;
    volatile lockdrv_state_t state;
    volatile bool      skip_guard;  /* set by the raw-pulse test hook */
    portMUX_TYPE       lock;
} s = {
    .state = LOCKDRV_ST_IDLE,
    .lock  = portMUX_INITIALIZER_UNLOCKED,
};

/* Both outputs LOW = coast. Register-level, ISR-safe. */
static inline void outputs_coast_isr(void)
{
    gpio_ll_set_level(&GPIO, s.ina, 0);
    gpio_ll_set_level(&GPIO, s.inb, 0);
}

/* Pulse-end alarm: the ONE place a pulse is allowed to end. */
static bool IRAM_ATTR lockdrv_on_pulse_end(gptimer_handle_t timer,
                                           const gptimer_alarm_event_data_t *edata,
                                           void *user_ctx)
{
    (void)edata; (void)user_ctx;
    outputs_coast_isr();
    gptimer_stop(timer);

    portENTER_CRITICAL_ISR(&s.lock);
    if (!s.skip_guard && s.guard_ms > 0) {
        s.state = LOCKDRV_ST_GUARD;
        esp_timer_start_once(s.guard_timer, (uint64_t)s.guard_ms * 1000ULL);
    } else {
        s.state = LOCKDRV_ST_IDLE;
    }
    s.skip_guard = false;
    portEXIT_CRITICAL_ISR(&s.lock);
    return false; /* no context switch needed */
}

static void lockdrv_on_guard_end(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s.lock);
    if (s.state == LOCKDRV_ST_GUARD) s.state = LOCKDRV_ST_IDLE;
    portEXIT_CRITICAL(&s.lock);
    ESP_LOGD(TAG, "guard window over -> idle");
}

static esp_err_t start_pulse(lockdrv_dir_t dir, uint32_t pulse_ms, bool skip_guard)
{
    ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(pulse_ms > 0, ESP_ERR_INVALID_ARG, TAG, "pulse_ms = 0");

    portENTER_CRITICAL(&s.lock);
    if (s.state != LOCKDRV_ST_IDLE) {
        portEXIT_CRITICAL(&s.lock);
        ESP_LOGW(TAG, "rejected: busy (%s)", lockdrv_state_str());
        return ESP_ERR_INVALID_STATE;
    }
    s.state      = LOCKDRV_ST_PULSING;
    s.skip_guard = skip_guard;
    portEXIT_CRITICAL(&s.lock);

    const gpio_num_t active = (dir == LOCKDRV_DIR_EXTEND) ? s.ina : s.inb;
    gpio_set_level(active, 1);

    gptimer_alarm_config_t alarm = {
        .alarm_count = (uint64_t)pulse_ms * 1000ULL, /* timer runs at 1 MHz */
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s.pulse_timer, &alarm));
    ESP_ERROR_CHECK(gptimer_set_raw_count(s.pulse_timer, 0));
    ESP_ERROR_CHECK(gptimer_start(s.pulse_timer));

    ESP_LOGI(TAG, "%s pulse, %lu ms",
             dir == LOCKDRV_DIR_EXTEND ? "extend (open)" : "retract (close)",
             (unsigned long)pulse_ms);
    return ESP_OK;
}

#if CONFIG_LOCKDRV_HOME_ON_INIT
/* Boot parity: one retract pulse shortly after init (OEM drives the
 * actuator to its known rest position at power-up; open-loop control has
 * no position feedback). Runs in the esp_timer task context. */
static void lockdrv_on_home_delay(void *arg)
{
    (void)arg;
    esp_err_t err = lockdrv_home();
    if (err != ESP_OK) ESP_LOGW(TAG, "boot home pulse failed: %s", esp_err_to_name(err));
}
#endif

esp_err_t lockdrv_init(const lockdrv_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(!s.inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s.ina      = cfg->ina_pin;
    s.inb      = cfg->inb_pin;
    s.pulse_ms = cfg->pulse_ms ? cfg->pulse_ms : CONFIG_LOCKDRV_PULSE_MS;
    s.guard_ms = cfg->guard_ms ? cfg->guard_ms : CONFIG_LOCKDRV_GUARD_MS;

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << s.ina) | (1ULL << s.inb),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config failed");
    gpio_set_level(s.ina, 0); /* coast from the first moment */
    gpio_set_level(s.inb, 0);

    const gptimer_config_t timer_cfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, /* 1 MHz -> 1 us ticks */
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_cfg, &s.pulse_timer), TAG,
                        "gptimer_new_timer failed");
    const gptimer_event_callbacks_t cbs = { .on_alarm = lockdrv_on_pulse_end };
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s.pulse_timer, &cbs, NULL),
                        TAG, "gptimer callbacks failed");
    ESP_RETURN_ON_ERROR(gptimer_enable(s.pulse_timer), TAG, "gptimer_enable failed");

    const esp_timer_create_args_t guard_args = {
        .callback = lockdrv_on_guard_end,
        .name     = "lockdrv_guard",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&guard_args, &s.guard_timer), TAG,
                        "guard timer failed");

    portENTER_CRITICAL(&s.lock);
    s.state  = LOCKDRV_ST_IDLE;
    s.inited = true;
    portEXIT_CRITICAL(&s.lock);

    ESP_LOGI(TAG, "init: INA=GPIO%d INB=GPIO%d pulse=%lu ms guard=%lu ms",
             s.ina, s.inb, (unsigned long)s.pulse_ms, (unsigned long)s.guard_ms);

#if CONFIG_LOCKDRV_HOME_ON_INIT
    const esp_timer_create_args_t home_args = {
        .callback = lockdrv_on_home_delay,
        .name     = "lockdrv_home",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&home_args, &s.home_timer), TAG,
                        "home timer failed");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_once(s.home_timer, (uint64_t)CONFIG_LOCKDRV_HOME_DELAY_MS * 1000ULL),
        TAG, "home timer start failed");
    ESP_LOGI(TAG, "boot home pulse scheduled in %d ms", CONFIG_LOCKDRV_HOME_DELAY_MS);
#endif
    return ESP_OK;
}

esp_err_t lockdrv_home(void)
{
    return start_pulse(LOCKDRV_DIR_RETRACT, s.pulse_ms, false);
}

esp_err_t lockdrv_open(void)
{
    return start_pulse(LOCKDRV_DIR_EXTEND, s.pulse_ms, false);
}

esp_err_t lockdrv_close(void)
{
    return start_pulse(LOCKDRV_DIR_RETRACT, s.pulse_ms, false);
}

esp_err_t lockdrv_abort(void)
{
    ESP_RETURN_ON_FALSE(s.inited, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    gptimer_stop(s.pulse_timer);
    esp_timer_stop(s.guard_timer);
    gpio_set_level(s.ina, 0);
    gpio_set_level(s.inb, 0);

    portENTER_CRITICAL(&s.lock);
    s.state      = LOCKDRV_ST_IDLE;
    s.skip_guard = false;
    portEXIT_CRITICAL(&s.lock);

    ESP_LOGW(TAG, "abort: forced coast");
    return ESP_OK;
}

bool lockdrv_is_busy(void)
{
    return s.state != LOCKDRV_ST_IDLE;
}

lockdrv_state_t lockdrv_get_state(void)
{
    return s.state;
}

const char *lockdrv_state_str(void)
{
    switch (s.state) {
    case LOCKDRV_ST_IDLE:    return "idle";
    case LOCKDRV_ST_PULSING: return "pulsing";
    case LOCKDRV_ST_GUARD:   return "guard";
    default:                 return "?";
    }
}

esp_err_t lockdrv_test_raw_pulse(lockdrv_dir_t dir, uint32_t pulse_ms)
{
    return start_pulse(dir, pulse_ms, true /* skip guard: bench margins */);
}
