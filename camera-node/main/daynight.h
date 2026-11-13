#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Automatic day/night switching of the IR illumination.
 *
 * Light level (0 = dark .. 100 = bright, log scale) comes from the LTR-308
 * ambient light sensor when it is found (lux, barely affected by the IR LEDs).
 * Otherwise it is derived from the camera's auto-exposure (exposure x gain);
 * the IR light then makes the scene look brighter, so CONFIG_CAMNODE_IR_COMP
 * is subtracted before comparing with the threshold.
 *
 *   IR off -> on  when level <  threshold              (3 samples in a row)
 *   IR on  -> off when level >  threshold + hysteresis (3 samples in a row)
 *   at most one change every 5 s
 */
void daynight_start(void);
void daynight_set_auto(bool enable);
void daynight_set_threshold(uint8_t thr);

/* Writes " auto=1 thr=40 ir=0 mode=day src=als lux=123.4 light=63 eff=63 exp=.. gain=.." (leading space).
 * src=als: LTR-308 ambient light sensor; src=cam: fallback from camera exposure. */
void daynight_status_string(char *buf, size_t len);
