/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 2025, October 2026
 * Copyright: 2025-2026 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
#include "blink.h"
#include "constants.h"
#include "debug.h"
#include "download.h"
#include "ff.h"
#include "httpc/httpc.h"
#include "memfunc.h"
#include "network.h"
#include "pico/stdlib.h"
#include "romemul.h"
#include "sdcard.h"
#include "select.h"
#include "term.h"

#define WIFI_SCAN_TIME_MS (5 * 1000)
#define DOWNLOAD_START_MS (3 * 1000)
#define DOWNLOAD_DAY_MS (86400 * 1000)
#define SLEEP_LOOP_MS 100

#define MAX_ROMS 100
#define MAX_FILENAME_LENGTH 36
#define MAX_PATH_SIZE 128

#define AUTORUN_BLINK_MS 200

// A ROM file on the SD card as [B]rowse lists it: the name it opens by (the
// long name when it fits, the card's 8.3 alias otherwise) and the name shown,
// cut to the field. The list holds MAX_ROMS of them while it is open.
typedef struct {
  char open[MAX_FILENAME_LENGTH];
  char name[MAX_FILENAME_LENGTH];
} SdRom;

enum {
  ROM_MODE_DIRECT = 0,  // ROM direct (no delay)
  ROM_MODE_DELAY = 1,   // ROM delay
  ROM_MODE_SETUP = 255  // ROM setup
};

#define ROM_MODE_SETUP_STR \
  "255"  // ROM setup string for the config initialization

typedef enum {
  AUTORUN_OK = 0,
  AUTORUN_ERR_AUTORUN_NOT_FOUND = -1,
  AUTORUN_ERR_AUTORUN_EMPTY = -2,
  AUTORUN_ERR_ROM_NOT_FOUND = -3,
  AUTORUN_ERR_FLASH_STORE = -4
} AutorunResult;

enum {
  TERM_ROMS_MENU_MAIN = 0,
  TERM_ROMS_MENU_BROWSE_SD = 1,
  TERM_ROMS_MENU_BROWSE_NETWORK = 2,
  TERM_ROMS_MENU_LAUNCH = 3,
  TERM_ROMS_MENU_SETTINGS = 4,
  TERM_ROMS_MENU_EXIT = 5,
  TERM_ROMS_MENU_BOOSTER = 6,
  TERM_ROMS_MENU_SUBMENU = 256
};

typedef struct {
  int menuLevel;
  int submenuLevel;
} MenuState;

/**
 * @brief
 *
 * Launches the ROM emulator application. Initializes terminal interfaces,
 * configures network and storage systems, and loads the ROM data from SD or
 * network sources. Manages the main loop which includes firmware bypass,
 * user interaction and potential system resets.
 */
void emul_start();

// App commands for `tools/dev/swd.py app NAME [WORDS...]`, debug builds only;
// the name after DEVHOOKS_APP_ is the one the tool takes.
//   heap_hold KB   hold KB more kilobytes of heap (0 releases everything);
//                  answers 0 when the allocation is refused, so repeated calls
//                  walk the heap down to a known remainder.
#define DEVHOOKS_APP_HEAP_HOLD 1
//   download       download the URL the host wrote into devdownloadState
//                  (devdownload.h) to the app folder, then hash it; answers 1
//                  when started, 0 while one runs.
//                  tools/dev/download_harness.py drives it and reads the
//                  outcome.
#define DEVHOOKS_APP_DOWNLOAD 2
//   wifi 0|1       take Wi-Fi down (0, as a lost network) or connect it again
//                  (1), for the offline paths of [D]ownload.
#define DEVHOOKS_APP_WIFI 3
//   health N       provoke a failure from the main loop, 250 ms later
//                  (health.h's health_test_t): 1 panic, 2 HardFault, 3 hang,
//                  4 a 500 ms stall, 5 stack overflow. Each must end in a
//                  reboot that names itself, but the stall.
#define DEVHOOKS_APP_HEALTH 4

#endif  // EMUL_H
