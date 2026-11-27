#pragma once

#include <stdint.h>
#include "esp_log.h"

/*
 * ONE FLAG TO SILENCE EVERYTHING
 * ------------------------------
 * 1 = all node logs are printed (development)
 * 0 = every NLOGx / NSTEP call compiles to nothing and the IDF log output is
 *     muted at start-up (see app_main). Use for the final battery build.
 *
 * For the smallest/fastest build also set, in menuconfig:
 *   Bootloader config -> Bootloader log verbosity = No output
 *   Component config -> Log -> Default log verbosity = No output
 */
#define LOGS_ENABLED 1

#if LOGS_ENABLED
#define NLOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define NLOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define NLOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
/* Numbered flow step, printed with the time since the start of the cycle. */
#define NSTEP(num, fmt, ...) node_log_step((num), fmt, ##__VA_ARGS__)
#else
#define NLOGI(tag, fmt, ...) ((void)0)
#define NLOGW(tag, fmt, ...) ((void)0)
#define NLOGE(tag, fmt, ...) ((void)0)
#define NSTEP(num, fmt, ...) ((void)0)
#endif

/* Marks "t = 0" for the elapsed-time column of NSTEP. Call when a cycle starts. */
void node_log_cycle_start(void);

/* Milliseconds since node_log_cycle_start(). Always available (also with logs off). */
int64_t node_log_elapsed_ms(void);

/* Implementation behind NSTEP: prints "[STEP n] text (+123 ms)". */
void node_log_step(int num, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
