/**
 * File: emul.h
 * Author: Diego Parrilla Santamaría
 * Date: January 20205
 * Copyright: 2025 - GOODDATA LABS SL
 * Description: Header for the ROM emulator core and setup features
 */

#ifndef EMUL_H
#define EMUL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aconfig.h"
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
#define MAX_ROMS_PER_PAGE 20
#define MAX_FILENAME_LENGTH 36
#define MAX_PATH_SIZE 128

typedef struct {
  char filename[MAX_FILENAME_LENGTH];
  // You can add other fields (e.g. file size, type, etc.)
  char path[MAX_PATH_SIZE];
  char name[MAX_FILENAME_LENGTH];
  char description[MAX_PATH_SIZE];
  char tags[MAX_FILENAME_LENGTH];
  int size;

} ROM;

enum {
  ROM_MODE_DIRECT = 0,  // ROM direct (no delay)
  ROM_MODE_DELAY = 1,   // ROM delay
  ROM_MODE_SETUP = 255  // ROM setup
};

#define ROM_MODE_SETUP_STR \
  "255"  // ROM setup string for the config initialization

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

#endif  // EMUL_H
