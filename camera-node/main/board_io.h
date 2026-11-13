#pragma once

#include <stdbool.h>

/* IR illumination LEDs (GPIO47, "IR_CT" on the schematic). Pin is set up on first use. */
void board_ir_set(bool on);
bool board_ir_get(void);

/* Call right before light sleep: forces IR off and latches the pin so it cannot float. */
void board_prepare_sleep(void);

/* Call right after waking: releases the latch so board_ir_set() works again. */
void board_resume(void);

/*
 * BOOT button (GPIO0) check used for the "factory reset" gesture. Polls for window_ms and
 * returns true as soon as the button has been held down continuously for hold_ms.
 * (Holding BOOT *while resetting* would start the ROM download mode, so the gesture is:
 * reset, then press and hold BOOT during the window that follows.)
 */
bool board_boot_button_held(unsigned window_ms, unsigned hold_ms);
