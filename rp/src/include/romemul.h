/**
 * File: romemul.h
 * Author: Diego Parrilla Santamaría
 * Date: July 2023-2025, February 2026
 * Copyright: 2023-2026 - GOODDATA LABS SL
 * Description: Header file for the ROM emulator C program.
 */

#ifndef ROMEMUL_H
#define ROMEMUL_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "constants.h"
#include "debug.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/vreg.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "romemul.pio.h"

#define ROMEMUL_BUS_BITS 16
// ROM mode serves both banks: A1-A16 and !ROM4, 17 bits.
#define ROMEMUL_BUS_BITS_TWO_BANKS 17

// Function Prototypes
int init_romemul(bool copyFlashToRAM);

// ROM mode: serve ROM4 ($FA0000) from the lower 64 KB of the window and ROM3
// ($FB0000) from the upper 64 KB. The command channel is not started in ROM
// mode: the ST owns the whole window. Returns the read state machine, or -1.
int romemul_initTwoBanks(bool copyFlashToRAM);

// Stops whichever engine is running: its state machines off, its DMA channels
// aborted, the latch controls back at idle and the data lines as inputs.
void romemul_stop(void);

// The window offset of the last word the ST read, or -1 with no engine: the
// channel that serves the bus keeps its read address at the word it served.
int32_t romemul_lastReadOffset(void);

#endif  // ROMEMUL_H
