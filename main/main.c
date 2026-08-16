/**
 * lockdrv_test — console harness for components/lockdrv (ES24F project).
 *
 * UART commands (115200 8N1, devkit USB-Serial):
 *   home                 retract pulse (OEM boot behavior)
 *   open                 extend pulse  (INA, ~235 ms, OEM parity)
 *   close                retract pulse (INB, ~235 ms, OEM parity)
 *   cycle [dwell_ms]     open + dwell + close (OEM auto-relock, def 4600 ms)
 *   pulse <ext|ret> <ms> raw test pulse, custom width (bypasses guard)
 *   sweep <ext|ret> <min> <max> <step>   width margin sweep, 1 s apart
 *   stress <n>           n open/close cycles, verifies retrigger rejection
 *   abort                force coast NOW (fault path)
 *   state                driver state dump
 *
 * Bench wiring (devkit -> lock main board):
 *   GPIO7 -> SOL1 net (HSJ08H pin 2, INA)   <- wire already soldered for scope
 *   GPIO8 -> SOL2 net (HSJ08H pin 3, INB)   <- wire already soldered for scope
 *   GND   -> board GND
 *   FR8018H held in reset (its GPIOs stay hi-Z), NOT removed.
 *   GPIO7/8 chosen to keep GPIO4/5/6 free for the audio bench wiring.
 *   Edit the defines below if the adapter board ends up using other pins.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"

#include "lockdrv.h"

/* ---------- pin configuration (edit to match the bench/adapter) ---------- */
#define GPIO_SOL1_INA   7
#define GPIO_SOL2_INB   8

/* OEM auto-relock dwell, scope-measured: 4.58-4.82 s (lock_app territory,
 * replicated here only so the bench can capture the full OEM-like cycle). */
#define CYCLE_DEFAULT_DWELL_MS  4600

static const char *TAG = "lockdrv_test";

/* Handle of the one background task allowed at a time (cycle/sweep/stress).
 * `abort` cancels it so the command acts as a real panic button. */
static TaskHandle_t s_bg_task = NULL;

/* ---------- helpers ---------- */

static lockdrv_dir_t parse_dir(const char *arg, bool *ok)
{
    *ok = true;
    if (!strcmp(arg, "ext")) return LOCKDRV_DIR_EXTEND;
    if (!strcmp(arg, "ret")) return LOCKDRV_DIR_RETRACT;
    *ok = false;
    return LOCKDRV_DIR_EXTEND;
}

static void report(esp_err_t e)
{
    printf(e == ESP_OK ? "ok\n" : "rejected: %s\n", esp_err_to_name(e));
}

/* ---------- simple commands ---------- */

static int cmd_home(int argc, char **argv)
{
    (void)argc; (void)argv;
    report(lockdrv_home());
    return 0;
}

static int cmd_open(int argc, char **argv)
{
    (void)argc; (void)argv;
    report(lockdrv_open());
    return 0;
}

static int cmd_close(int argc, char **argv)
{
    (void)argc; (void)argv;
    report(lockdrv_close());
    return 0;
}

static int cmd_abort(int argc, char **argv)
{
    (void)argc; (void)argv;
    TaskHandle_t t = s_bg_task;
    s_bg_task = NULL;      /* clear BEFORE deleting: never touch a stale handle */
    if (t) {
        vTaskDelete(t);
        printf("background task cancelled\n");
    }
    report(lockdrv_abort());
    printf("note: actuator may be left extended; use 'close'/'home' to rest it\n");
    return 0;
}

static int cmd_state(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("state: %s | busy: %s | pins: INA=GPIO%d INB=GPIO%d\n",
           lockdrv_state_str(), lockdrv_is_busy() ? "yes" : "no",
           GPIO_SOL1_INA, GPIO_SOL2_INB);
    return 0;
}

static int cmd_pulse(int argc, char **argv)
{
    if (argc != 3) { printf("usage: pulse <ext|ret> <ms>\n"); return 1; }
    bool ok;
    lockdrv_dir_t dir = parse_dir(argv[1], &ok);
    if (!ok) { printf("direction must be 'ext' or 'ret'\n"); return 1; }
    report(lockdrv_test_raw_pulse(dir, (uint32_t)strtoul(argv[2], NULL, 10)));
    return 0;
}

/* ---------- cycle: open + dwell + close (runs in its own task) ---------- */

static void cycle_task(void *arg)
{
    const uint32_t dwell_ms = (uint32_t)(uintptr_t)arg;
    if (lockdrv_open() != ESP_OK) goto done;
    while (lockdrv_is_busy()) vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "dwell %lu ms (OEM auto-relock)", (unsigned long)dwell_ms);
    vTaskDelay(pdMS_TO_TICKS(dwell_ms));
    lockdrv_close();
    while (lockdrv_is_busy()) vTaskDelay(pdMS_TO_TICKS(10));
done:
    printf("cycle done\n");
    s_bg_task = NULL;
    vTaskDelete(NULL);
}

static int cmd_cycle(int argc, char **argv)
{
    if (argc > 2) { printf("usage: cycle [dwell_ms]\n"); return 1; }
    uint32_t dwell = (argc == 2) ? (uint32_t)strtoul(argv[1], NULL, 10)
                                 : CYCLE_DEFAULT_DWELL_MS;
    if (s_bg_task) { printf("a background task is already running ('abort' to cancel)\n"); return 1; }
    if (xTaskCreate(cycle_task, "cycle", 3072, (void *)(uintptr_t)dwell, 5, &s_bg_task)
        != pdPASS) {
        printf("could not start cycle task\n");
        return 1;
    }
    printf("cycle started (dwell %lu ms)\n", (unsigned long)dwell);
    return 0;
}

/* ---------- sweep: width margins on the bench ---------- */

static void sweep_task(void *arg)
{
    const uint32_t *p = (const uint32_t *)arg; /* [0]=dir [1]=min [2]=max [3]=step */
    const lockdrv_dir_t dir = (lockdrv_dir_t)p[0];
    for (uint32_t ms = p[1]; ms <= p[2]; ms += p[3]) {
        while (lockdrv_is_busy()) vTaskDelay(pdMS_TO_TICKS(10));
        printf("sweep: %s %lu ms\n", dir == LOCKDRV_DIR_EXTEND ? "ext" : "ret",
               (unsigned long)ms);
        if (lockdrv_test_raw_pulse(dir, ms) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1000)); /* settle between pulses */
    }
    printf("sweep done\n");
    free(arg);
    s_bg_task = NULL;
    vTaskDelete(NULL);
}

static int cmd_sweep(int argc, char **argv)
{
    if (argc != 5) { printf("usage: sweep <ext|ret> <ms_min> <ms_max> <step>\n"); return 1; }
    bool ok;
    lockdrv_dir_t dir = parse_dir(argv[1], &ok);
    if (!ok) { printf("direction must be 'ext' or 'ret'\n"); return 1; }
    uint32_t *p = malloc(4 * sizeof(uint32_t));
    p[0] = (uint32_t)dir;
    p[1] = strtoul(argv[2], NULL, 10);
    p[2] = strtoul(argv[3], NULL, 10);
    p[3] = strtoul(argv[4], NULL, 10);
    if (p[3] == 0 || p[2] < p[1]) { printf("bad range\n"); free(p); return 1; }
    if (s_bg_task) { printf("a background task is already running ('abort' to cancel)\n"); free(p); return 1; }
    if (xTaskCreate(sweep_task, "sweep", 3072, p, 5, &s_bg_task) != pdPASS) {
        printf("could not start sweep task\n"); free(p); return 1;
    }
    printf("sweep started\n");
    return 0;
}

/* ---------- stress: n cycles + retrigger rejection proof ---------- */

static void stress_task(void *arg)
{
    const uint32_t cycles = (uint32_t)(uintptr_t)arg;
    uint32_t rejected = 0, failed = 0;
    for (uint32_t i = 0; i < cycles; i++) {
        if (lockdrv_open() != ESP_OK) { failed++; break; }
        /* mid-pulse retrigger must be rejected */
        if (lockdrv_open() == ESP_ERR_INVALID_STATE) rejected++;
        else failed++;
        while (lockdrv_is_busy()) vTaskDelay(pdMS_TO_TICKS(10));
        if (lockdrv_close() != ESP_OK) { failed++; break; }
        while (lockdrv_is_busy()) vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("stress done: %lu cycles, %lu retriggers rejected, %lu failures\n",
           (unsigned long)cycles, (unsigned long)rejected, (unsigned long)failed);
    s_bg_task = NULL;
    vTaskDelete(NULL);
}

static int cmd_stress(int argc, char **argv)
{
    if (argc != 2) { printf("usage: stress <cycles>\n"); return 1; }
    uint32_t n = (uint32_t)strtoul(argv[1], NULL, 10);
    if (n == 0) { printf("cycles must be > 0\n"); return 1; }
    if (s_bg_task) { printf("a background task is already running ('abort' to cancel)\n"); return 1; }
    if (xTaskCreate(stress_task, "stress", 3072, (void *)(uintptr_t)n, 5, &s_bg_task)
        != pdPASS) {
        printf("could not start stress task\n");
        return 1;
    }
    printf("stress started (%lu cycles)\n", (unsigned long)n);
    return 0;
}

/* ---------- app_main ---------- */

void app_main(void)
{
    const lockdrv_config_t cfg = {
        .ina_pin  = GPIO_SOL1_INA,
        .inb_pin  = GPIO_SOL2_INB,
        .pulse_ms = 0, /* 0 = Kconfig default (OEM parity: 235 ms) */
        .guard_ms = 0, /* 0 = Kconfig default */
    };
    ESP_ERROR_CHECK(lockdrv_init(&cfg));

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "lockdrv> ";
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl));

    const esp_console_cmd_t cmds[] = {
        { .command = "home",   .help = "retract pulse (OEM boot behavior)",        .func = cmd_home   },
        { .command = "open",   .help = "extend pulse, ~235 ms OEM parity",         .func = cmd_open   },
        { .command = "close",  .help = "retract pulse, ~235 ms OEM parity",        .func = cmd_close  },
        { .command = "cycle",  .help = "cycle [dwell_ms]: open+dwell+close (OEM)", .func = cmd_cycle  },
        { .command = "pulse",  .help = "pulse <ext|ret> <ms> raw, bypasses guard", .func = cmd_pulse  },
        { .command = "sweep",  .help = "sweep <ext|ret> <min> <max> <step>",       .func = cmd_sweep  },
        { .command = "stress", .help = "stress <n> cycles + retrigger rejection",  .func = cmd_stress },
        { .command = "abort",  .help = "force coast NOW",                          .func = cmd_abort  },
        { .command = "state",  .help = "driver state dump",                        .func = cmd_state  },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    esp_console_register_help_command();
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    printf("\n=== lockdrv_test ES24F (stage 5) ===\n");
    printf("Wiring: GPIO%d->SOL1(INA) | GPIO%d->SOL2(INB) | GND common | "
           "FR8018H held in reset\n", GPIO_SOL1_INA, GPIO_SOL2_INB);
    printf("Quick check: 'open' (actuator extends), 'cycle' (full OEM cycle),\n");
    printf("'state', 'stress 10'. NOTE: boot home pulse runs ~%d ms after boot.\n",
           CONFIG_LOCKDRV_HOME_DELAY_MS);
}
