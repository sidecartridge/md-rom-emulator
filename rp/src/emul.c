/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025, October 2026
 * Copyright: 2025-2026 - GOODDATA LABS
 * Description: The ROM Emulator: its setup menu, on the template's bring-up
 *              order and polling rules, and its ROM mode, which serves a ROM
 *              from both cartridge banks.
 */

#include "emul.h"

#include <ctype.h>
#include <stdint.h>

// included in the C file to avoid multiple definitions
#include "aconfig.h"
#include "catalog.h"
#include "chandler.h"
#include "commemul.h"
#include "constants.h"
#include "debug.h"
#include "devdownload.h"  // Debug-only test download, driven over SWD
#include "devhooks.h"     // Debug-only SWD mailbox; include in this file only
#include "display.h"
#include "ff.h"
#include "gconfig.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "health.h"
#include "memfunc.h"
#include "navlist.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "romstore.h"
#include "sdcard.h"
#include "select.h"
#include "target_firmware.h"  // Include the target firmware binary
#include "term.h"

// How long a sentinel command the ST must act on before this side moves on
// is held. The ST reads the sentinel once per pass of its menu loop (measured
// on an ST: within 42 ms); the hold is well over that.
#define SENTINEL_HOLD_MS 500
// The longest the jump to Booster waits for the ST to reboot: past main.s's
// PRE_RESET_WAIT at 8 MHz (about 2.4 s).
#define BOOSTER_HANDOVER_MAX_MS 4000
// The cartridge header TOS reads at boot: a read below it means the ST has
// rebooted.
#define CARTRIDGE_HEADER_BYTES 0x100
#define BYTES_PER_KB 1024U
#define US_PER_MS 1000U
// A 32-bit number with its thousands commas, and the terminator.
#define NUMBER_TEXT_BYTES 16
// The download message's prefixes: the name gets what is left of the line.
#define DOWNLOADED_PREFIX "Downloaded: "
#define DOWNLOADING_PREFIX "Downloading: "
// How often the main loop gives lwIP's network stack a turn.
#define NETWORK_POLL_MS 10

// The largest file ROM_TEMP takes: two 64 KB banks, plus the 4 zero bytes a
// STEEM cartridge image starts with.

// Command handlers
static void cmdMenu(const char *arg);
static void cmdClear(const char *arg);
static void cmdExit(const char *arg);
static void cmdHelp(const char *arg);
static void cmdCard(const char *arg);
static void cmdNetwork(const char *arg);
static void cmdLaunch(const char *arg);
static void cmdBooster(const char *arg);
static void cmdDelay(const char *arg);
static void cmdUnknown(const char *arg);
#if defined(_DEBUG) && (_DEBUG != 0)
static void cmdFirmware(const char *arg);
static void netTestWifi(bool connect);
#endif

// Command table
static const Command commands[] = {
    {"m", cmdMenu},
    {"h", cmdHelp},
    {"b", cmdCard},
    {"d", cmdNetwork},
    {"l", cmdLaunch},
    {"r", cmdDelay},
    {"e", cmdExit},
    {"x", cmdBooster},
    {"?", cmdHelp},
    {"s", term_cmdSettings},
    {"settings", term_cmdSettings},
    {"print", term_cmdPrint},
    {"save", term_cmdSave},
    {"erase", term_cmdErase},
    {"get", term_cmdGet},
    {"put_int", term_cmdPutInt},
    {"put_bool", term_cmdPutBool},
    {"put_str", term_cmdPutString},
#if defined(_DEBUG) && (_DEBUG != 0)
    // Debug builds only, and not in the menu: hands the ST to userfw.s, where
    // tools/dev/st_harness.py puts its command-path tests (sttest.s).
    {"f", cmdFirmware},
#endif
    {"", cmdUnknown},
};

// Number of commands in the table
static const size_t numCommands = sizeof(commands) / sizeof(commands[0]);

// ROMs folder. Initialize with the default value.
static char romsFolder[MAX_PATH_SIZE] = "/roms";

// Menu status
static MenuState menuState = {0, 0};

// Keep active loop or exit
static bool keepActive = true;

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// Delay/ripper mode?
static bool delayMode = false;

// The network, brought up behind the menu: the menu never waits for it.
typedef enum {
  NET_OFF = 0,     // no Wi-Fi settings, or Booster's AP mode
  NET_CONNECTING,  // network_wifiStaConnectPoll() from the main loop
  NET_UP,
  NET_FAILED,
} NetState;
static NetState netState = NET_OFF;
static const char *netReason = "Wi-Fi is not configured.";
static int netAttempts = 0;
#define NET_CONNECT_ATTEMPTS 3

// The catalog on the card, <FOLDER>/roms.csv, refreshed once the network is
// up; the card's copy is replaced only by a complete download.
typedef enum {
  CATALOG_REFRESH_NONE = 0,
  CATALOG_REFRESH_RUNNING,
  CATALOG_REFRESH_DONE,
  CATALOG_REFRESH_FAILED,
} CatalogRefresh;
static CatalogRefresh catalogRefresh = CATALOG_REFRESH_NONE;
static char catalogReason[TERM_SCREEN_SIZE_X] = "";

// Where the catalog came from, after its redirects: a ROM is fetched from
// the catalog's host, whatever was downloaded since.
static struct {
  char protocol[DOWNLOAD_PROTOCOL_SIZE];
  char host[DOWNLOAD_HOSTNAME_SIZE];
  uint16_t port;
  bool known;
} catalogOrigin;

// One download at a time: the catalog refresh, or a ROM from it.
typedef enum {
  DOWNLOAD_KIND_NONE = 0,
  DOWNLOAD_KIND_CATALOG,
  DOWNLOAD_KIND_ROM,
} DownloadKind;
static DownloadKind downloadKind = DOWNLOAD_KIND_NONE;
// The ROM being downloaded, copied when it started: selected on success, and
// its size in the catalog, which the file must match.
static char downloadRomName[CATALOG_SAVED_NAME_BYTES] = "";
static uint32_t downloadRomSizeKb = 0;
// The last download's outcome, a line on the menu.
static char downloadMessage[TERM_SCREEN_SIZE_X * 2] = "";

// The lists: [B]rowse (the ROM files on the card) and [D]ownload (the
// catalog), a page at a time (navlist.c), keys taken one at a time.
typedef enum { LIST_NONE = 0, LIST_CARD, LIST_CATALOG } ListKind;
static ListKind listKind = LIST_NONE;
static navlist_t listNav;
static bool listDetails = false;
static SdRom *sdRoms = NULL;  // [B]: only while the list is open
static uint32_t sdRomsCount = 0;
static catalog_t catalog;  // [D]: the page index, while the list is open
static uint32_t catalogPageCached = UINT32_MAX;
static char catalogNames[NAVLIST_PAGE_LINES][CATALOG_SHOWN_BYTES];
static catalog_entry_t catalogEntry;  // the one on the details screen

#if defined(_DEBUG) && (_DEBUG != 0)
// Heap held on request by `swd.py app heap_hold`, to test running out of it.
typedef struct DevhooksHeldBlock {
  struct DevhooksHeldBlock *next;
} DevhooksHeldBlock;
static DevhooksHeldBlock *devhooksHeldHeap = NULL;

// App commands sent over SWD by tools/dev/swd.py (see emul.h).
static uint32_t emul_devhooksApp(uint16_t commandId, const uint16_t *payload,
                                 uint16_t payloadSize) {
  switch (commandId) {
    case DEVHOOKS_APP_HEAP_HOLD: {
      uint32_t holdKb = (payloadSize >= 2U) ? payload[0] : 0U;
      if (holdKb == 0U) {
        while (devhooksHeldHeap != NULL) {
          DevhooksHeldBlock *next = devhooksHeldHeap->next;
          free(devhooksHeldHeap);
          devhooksHeldHeap = next;
        }
        DPRINTF("devhooks: heap hold released\n");
        return 1;
      }
      DevhooksHeldBlock *block =
          malloc(sizeof(DevhooksHeldBlock) + (holdKb * BYTES_PER_KB));
      if (block != NULL) {
        block->next = devhooksHeldHeap;
        devhooksHeldHeap = block;
      }
      DPRINTF("devhooks: holding %lu KB more heap: %s\n", (unsigned long)holdKb,
              (block != NULL) ? "ok" : "refused");
      return (block != NULL) ? 1U : 0U;
    }
    case DEVHOOKS_APP_DOWNLOAD:
      return devdownload_start();
    case DEVHOOKS_APP_WIFI:
      netTestWifi((payloadSize >= 2U) && (payload[0] != 0U));
      return 1;
    case DEVHOOKS_APP_SD:
      if (payloadSize < 2U) {
        return 0;
      }
      sdcard_testSetRemoved(payload[0] == 0U);
      if (payload[0] == 0U) {
        sdcard_checkPresence();
      }
      return 1;
    case DEVHOOKS_APP_HEALTH:
      if (payloadSize < 2U || payload[0] == HEALTH_TEST_NONE ||
          payload[0] > HEALTH_TEST_STACK_OVERFLOW) {
        return 0;
      }
      health_requestTest((health_test_t)payload[0]);
      return 1;
    default:
      return 0;
  }
}
#endif

// Keep answering the ST for durationMs, so a command in flight is not left
// without its answer while a sentinel command waits to be seen.
static void emul_serviceFor(uint32_t durationMs) {
  absolute_time_t until = make_timeout_time_ms(durationMs);
  while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
    health_feed();
    chandler_loop();
  }
}

// Nothing of this app's runs past a reset or the jump to Booster: the bus
// engine and the command ring stop first (the jump itself masks and clears
// the interrupts, reset.h).
static void emul_quiesce(void) {
  romemul_stop();
  commemul_stop();
}

static void emul_quiesceAndReset(void) {
  emul_quiesce();
  reset_device();
}

static void emul_quiesceAndFactoryReset(void) {
  emul_quiesce();
  reset_deviceAndEraseFlash();
}

// After the reset command: true once the ST has rebooted, which shows as a
// read of the cartridge header (the first 256 bytes, TOS's cartridge checks;
// the menu loop never reads there). False after maxMs, when nothing on the ST
// was listening (the desktop, a ROM).
static bool emul_waitForStReboot(uint32_t maxMs) {
  absolute_time_t until = make_timeout_time_ms(maxMs);
  while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
    health_feed();
    chandler_loop();
    int32_t offset = romemul_lastReadOffset();
    if (offset >= 0 && offset < CARTRIDGE_HEADER_BYTES) {
      return true;
    }
  }
  return false;
}

// What every long wait runs, so the ST's commands are answered and SELECT is
// seen meanwhile.
static void __not_in_flash_func(emul_pollTick)(void) {
  health_feed();
  chandler_loop();
  term_loop();
  select_poll();
}

// ROM_TEMP's flash for romstore.c: one sector per call, interrupts off for
// that sector's erase or program only. Kept for SWD: the longest such stretch
// and the last whole write's duration.
volatile uint32_t romstoreMaxIrqOffUs = 0;
volatile uint32_t romstoreLastWriteUs = 0;
static uint32_t romstoreWriteStartUs = 0;
static uint32_t romstoreProgressMarks = 0;
static bool romstoreHeaderShown = false;

static void romFlashTime(uint32_t startUs) {
  uint32_t took = time_us_32() - startUs;
  if (took > romstoreMaxIrqOffUs) {
    romstoreMaxIrqOffUs = took;
  }
}

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds only, for the power-cut test: each sector's erase is repeated
// this many more times, so the erase phase lasts long enough for a hand on
// the power switch. Set over SWD before a launch; a reset clears it.
volatile uint32_t romstoreTestEraseRepeats = 0;
#endif

static void romFlashErase(uint32_t offset, size_t bytes) {
  uint32_t startUs = time_us_32();
  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(offset, bytes);
  restore_interrupts(ints);
  romFlashTime(startUs);
#if defined(_DEBUG) && (_DEBUG != 0)
  for (uint32_t i = 0; i < romstoreTestEraseRepeats; i++) {
    emul_pollTick();
    ints = save_and_disable_interrupts();
    flash_range_erase(offset, bytes);
    restore_interrupts(ints);
  }
#endif
}

static void romFlashProgram(uint32_t offset, const uint8_t *data,
                            size_t bytes) {
  uint32_t startUs = time_us_32();
  uint32_t ints = save_and_disable_interrupts();
  flash_range_program(offset, data, bytes);
  restore_interrupts(ints);
  romFlashTime(startUs);
}

static const uint8_t *romFlashRead(uint32_t offset) {
  return (const uint8_t *)(XIP_BASE + offset);
}

// Between sectors: the ST is answered, SELECT is seen, and a mark goes on the
// progress line for every 1/ROMSTORE_PROGRESS_MARKS of the write.
#define ROMSTORE_PROGRESS_MARKS 32
#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds only, set over SWD by symbol: each sector of a ROM write waits
// this long, answering the ST, so the write lasts long enough for a hand to
// pull the SD card in the middle of it. Any reset clears it.
volatile uint32_t romstoreTestSectorDelayMs = 0;
#endif

static void romFlashTick(uint32_t done, uint32_t total) {
  emul_pollTick();
#if defined(_DEBUG) && (_DEBUG != 0)
  if (romstoreTestSectorDelayMs != 0U) {
    absolute_time_t until = make_timeout_time_ms(romstoreTestSectorDelayMs);
    while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
      emul_pollTick();
    }
  }
#endif
  if (!romstoreHeaderShown) {
    // Only once the first sector is erased: a refused file shows no write.
    term_printString("\nWriting the ROM to flash:\n");
    romstoreHeaderShown = true;
  }
  uint32_t marks = done * ROMSTORE_PROGRESS_MARKS / total;
  if (marks > romstoreProgressMarks) {
    while (romstoreProgressMarks < marks) {
      term_printString("#");
      romstoreProgressMarks++;
    }
    display_refresh();
  }
}

static const romstore_flash_t romFlash = {romFlashErase, romFlashProgram,
                                          romFlashRead, romFlashTick};

static void romFlashBegin(void) {
  health_setPhase(HEALTH_PHASE_FLASH_WRITE);
  romstoreWriteStartUs = time_us_32();
  romstoreProgressMarks = 0;
  romstoreHeaderShown = false;
}

static void romFlashEnd(void) {
  romstoreLastWriteUs = time_us_32() - romstoreWriteStartUs;
  DPRINTF("ROM write: %lu us, interrupts off at most %lu us\n",
          (unsigned long)romstoreLastWriteUs,
          (unsigned long)romstoreMaxIrqOffUs);
}

// n with a comma every three digits, for the user's messages.
static void formatThousands(char *out, size_t outSize, uint32_t n) {
  char digits[NUMBER_TEXT_BYTES];
  int len = snprintf(digits, sizeof(digits), "%lu", (unsigned long)n);
  size_t pos = 0;
  for (int i = 0; i < len && pos + 1 < outSize; i++) {
    if (i > 0 && (len - i) % 3 == 0 && pos + 2 < outSize) {
      out[pos++] = ',';
    }
    out[pos++] = digits[i];
  }
  out[pos] = '\0';
}

// Why a launch failed, on the ST.
static void romstoreReport(romstore_result_t result,
                           const romstore_info_t *info) {
  term_printString("\n");
  if (result == ROMSTORE_TOO_LARGE) {
    char size[NUMBER_TEXT_BYTES];
    char limit[NUMBER_TEXT_BYTES];
    char line[TERM_SCREEN_SIZE_X * 2];
    formatThousands(size, sizeof(size), info->fileBytes);
    formatThousands(limit, sizeof(limit), ROMSTORE_MAX_BYTES);
    snprintf(line, sizeof(line), "ROM too large: %s bytes,\nthe limit is %s.",
             size, limit);
    term_printString(line);
  } else {
    term_printString(romstore_message(result));
  }
  term_printString("\n");
}

// The autorun's selection, saved after its ROM was written and read back.
static const char *autorunRomName = NULL;
static void autorunSelect(void) {
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED,
                      autorunRomName);
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                       ROM_MODE_DIRECT);
  settings_save(aconfig_getContext(), true);
}

// Tries to autorun a ROM specified in /roms/.autorun (or custom ROM folder)
static AutorunResult autorunIfRequested(const char *folder) {
  char autorunPath[MAX_PATH_SIZE];
  snprintf(autorunPath, sizeof(autorunPath), "%s/.autorun", folder);
  DPRINTF("Checking for autorun file: %s\n", autorunPath);

  FIL autorunFile;
  FRESULT res = f_open(&autorunFile, autorunPath, FA_READ);
  if (res != FR_OK) {
    DPRINTF("No autorun file found or cannot open it: %d\n", res);
    return AUTORUN_ERR_AUTORUN_NOT_FOUND;  // No autorun file or cannot open it
  }

  const size_t fileBufSize = MAX_PATH_SIZE;
  char *fileBuf = malloc(fileBufSize);
  if (fileBuf == NULL) {
    DPRINTF("Error allocating memory for autorun file buffer\n");
    f_close(&autorunFile);
    return AUTORUN_ERR_AUTORUN_EMPTY;
  }
  UINT bytesRead = 0;
  res = f_read(&autorunFile, fileBuf, fileBufSize - 1, &bytesRead);
  f_close(&autorunFile);
  if (res != FR_OK || bytesRead == 0) {
    DPRINTF("Error reading autorun file or file is empty: %d\n", res);
    free(fileBuf);
    return AUTORUN_ERR_AUTORUN_EMPTY;  // Autorun file empty or read error
  }
  fileBuf[bytesRead] = '\0';

  // Trim whitespace/newlines
  char *filenameStart = fileBuf;
  while (*filenameStart && isspace((unsigned char)*filenameStart)) {
    filenameStart++;
  }
  char *filenameEnd = filenameStart + strlen(filenameStart);
  while (filenameEnd > filenameStart &&
         isspace((unsigned char)*(filenameEnd - 1))) {
    filenameEnd--;
  }
  *filenameEnd = '\0';

  if (*filenameStart == '\0') {
    DPRINTF("Autorun file is empty after trimming whitespace\n");
    free(fileBuf);
    return AUTORUN_ERR_AUTORUN_EMPTY;  // Empty filename
  }

  DPRINTF("Autorun requested for file: %s\n", filenameStart);

  // Build full path to the ROM file
  char romPath[MAX_PATH_SIZE];
  snprintf(romPath, sizeof(romPath), "%s/%s", folder, filenameStart);

  // Ensure the target file exists and is not a directory
  FILINFO fno;
  res = f_stat(romPath, &fno);
  if (res != FR_OK || (fno.fattrib & AM_DIR)) {
    DPRINTF("Autorun file not found or is a directory: %s\n", romPath);
    free(fileBuf);
    return AUTORUN_ERR_ROM_NOT_FOUND;  // ROM file not found or is a directory
  }

  // The ROM into flash, read back; only then the settings that boot into it.
  autorunRomName = filenameStart;
  romstore_info_t info;
  term_setBusy(true);
  romFlashBegin();
  romstore_result_t stored = romstore_launch(romPath, FLASH_ROM_LOAD_OFFSET,
                                             &romFlash, &info, autorunSelect);
  romFlashEnd();
  term_setBusy(false);
  free(fileBuf);
  autorunRomName = NULL;
  if (stored != ROMSTORE_OK) {
    DPRINTF("Failed to store autorun ROM to flash: %s\n",
            romstore_message(stored));
    return AUTORUN_ERR_FLASH_STORE;  // Failed to store ROM in flash
  }

  // Blink the LED (if available) forever instead of resetting. The ST and
  // SELECT stay serviced: a short press restarts the RP, which boots into the
  // ROM just stored (MODE is ROM_MODE_DIRECT now), as v2.1.2 did.
  DPRINTF("Autorun successful. Blinking LED to indicate autorun mode.\n");
  bool ledOn = false;
  uint32_t toggledUs = time_us_32();
  while (1) {
    emul_pollTick();
    if (time_us_32() - toggledUs < AUTORUN_BLINK_MS * US_PER_MS) {
      continue;
    }
    toggledUs = time_us_32();
    ledOn = !ledOn;
#ifdef BLINK_H
    if (ledOn) {
      blink_on();
    } else {
      blink_off();
    }
#endif
  }
  return AUTORUN_OK;  // Not reached on success, keeps signature consistent
}

/**
 * @brief Checks whether a filename has one of the allowed extensions.
 *
 * Allowed extensions: "img", "rom", "stc", "bin" (case-insensitive)
 *
 * @param filename The filename to check.
 * @return 1 if the extension is allowed, 0 otherwise.
 */
static int hasValidExtension(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot || dot == filename) {
    return 0;  // No extension found.
  }
  dot++;  // Skip the dot.

  // Compare the extension case-insensitively.
  if (strcasecmp(dot, "img") == 0 || strcasecmp(dot, "rom") == 0 ||
      strcasecmp(dot, "stc") == 0 || strcasecmp(dot, "bin") == 0) {
    return 1;
  }
  return 0;
}

// [B]rowse sorts by the name shown, case-insensitively.
static int compareSdRoms(const void *first, const void *second) {
  const SdRom *romA = (const SdRom *)first;
  const SdRom *romB = (const SdRom *)second;
  return strcasecmp(romA->name, romB->name);
}

// The ROM's full name for EMULATED, from the name the browse list opens (the
// long name, or the card's short 8.3 one): the long name when a settings
// value holds it, the short one otherwise. Found by reading the folder:
// f_stat on a short name returns only the short name.
static void romFullName(const char *openName, char *out, size_t outSize) {
  snprintf(out, outSize, "%s", openName);
  DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, romsFolder) != FR_OK) {
    return;
  }
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
    if (strcmp(fno.fname, openName) == 0 ||
        strcmp(fno.altname, openName) == 0) {
      if (strlen(fno.fname) < outSize) {
        snprintf(out, outSize, "%s", fno.fname);
      }
      break;
    }
  }
  f_closedir(&dir);
}

// A name on one line: cut to width with "..." when longer.
static void termPrintCut(const char *text, int width) {
  if (width < 4 || (int)strlen(text) <= width) {
    term_printString(text);
    return;
  }
  char cut[TERM_SCREEN_SIZE_X + 1];
  snprintf(cut, sizeof(cut), "%.*s...", width - 3, text);
  term_printString(cut);
}

// The ROM files in folder, into sdRoms (allocated here, freed by
// listClose()), sorted by name. The list is empty when the folder cannot be
// read.
static FRESULT readRomsSdcard(const char *folder) {
  sdRomsCount = 0;
  if (sdRoms == NULL) {
    sdRoms = (SdRom *)malloc(MAX_ROMS * sizeof(SdRom));
    if (sdRoms == NULL) {
      return FR_NOT_ENOUGH_CORE;
    }
  }
  DIR dir;
  FILINFO fno;
  FRESULT res = f_opendir(&dir, folder);
  if (res != FR_OK) {
    DPRINTF("Error opening directory %s: %d\n", folder, res);
    return res;
  }
  for (;;) {
    // A large folder takes a while: keep answering the ST.
    emul_pollTick();
    res = f_readdir(&dir, &fno);
    if (res != FR_OK || fno.fname[0] == 0) {
      break;
    }
    if ((fno.fattrib & AM_DIR) || fno.fname[0] == '.' ||
        !hasValidExtension(fno.fname)) {
      continue;
    }
    if (sdRomsCount >= MAX_ROMS) {
      DPRINTF("Maximum ROM count reached (%d)\n", MAX_ROMS);
      break;
    }
    // The name to open: the long name when the field holds it whole, the
    // card's short 8.3 name otherwise, which opens the same file. The name
    // on screen is the long one, cut to the field.
    const char *openName =
        (strlen(fno.fname) < MAX_FILENAME_LENGTH || fno.altname[0] == '\0')
            ? fno.fname
            : fno.altname;
    SdRom *rom = &sdRoms[sdRomsCount++];
    snprintf(rom->open, sizeof(rom->open), "%s", openName);
    snprintf(rom->name, sizeof(rom->name), "%s", fno.fname);
  }
  f_closedir(&dir);
  qsort(sdRoms, sdRomsCount, sizeof(SdRom), compareSdRoms);
  DPRINTF("Found %lu ROMs on the SD card.\n", (unsigned long)sdRomsCount);
  return FR_OK;
}

static void showTitle() {
  term_printString(
      "\x1B"
      "E"
      "ROM Emulator - " RELEASE_VERSION "\n");
}

static void menu(void) {
  menuState.menuLevel = TERM_ROMS_MENU_MAIN;
  showTitle();
  // Why the RP restarted, when nobody asked for it, on the free row under
  // the title, for as long as this boot lasts.
  char bootLine[TERM_SCREEN_SIZE_X];
  if (health_getBootLine(bootLine, sizeof(bootLine))) {
    term_printString(bootLine);
  }
  term_printString("\n\n");
  term_printString("[B] Browse ROMs in microSD card\n");
  term_printString("[D] Download ROMs from the catalog\n");
  term_printString("[S] Settings\n\n");
  term_printString("[E] Exit to desktop (or hold SHIFT)\n");
  term_printString("[X] Return to booster menu\n\n");

  if (delayMode) {
    term_printString("[R] Disable ROM delay/ripper mode\n");
  } else {
    term_printString("[R] Enable ROM delay/ripper mode\n");
  }
  term_printString("\n");

  // Read ACONFIG_PARAM_ROM_SELECTED
  SettingsConfigEntry *romSelected =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED);
  if ((romSelected != NULL) && (strlen(romSelected->value) > 0)) {
    term_printString("[L] Launch ROM: ");
    termPrintCut(romSelected->value,
                 TERM_SCREEN_SIZE_X - (int)strlen("[L] Launch ROM: ") - 1);
    term_printString("\n");
  }
  term_printString("\n");

  term_printString("[M] Refresh this menu\n");

  term_printString("\n");

  // The network, then the last download
  if (!sdcard_isMounted()) {
    term_printString("SD card: none. Put one in.\n");
  }
  term_printString("Network: ");
  switch (netState) {
    case NET_CONNECTING:
      term_printString("connecting...\n");
      break;
    case NET_UP:
      term_printString(catalogRefresh == CATALOG_REFRESH_RUNNING
                           ? "connected, refreshing\n"
                           : "connected\n");
      break;
    default:
      term_printString(netReason);
      term_printString("\n");
      break;
  }
  if (downloadMessage[0] != '\0') {
    term_printString(downloadMessage);
    term_printString("\n");
  } else {
    term_printString("\n");
  }
  term_printString("Select an option: ");
}

// Command handlers
void cmdMenu(const char *arg) { menu(); }

void cmdHelp(const char *arg) {
  // term_printString("\x1B" "E" "Available commands:\n");
  term_printString("Available commands:\n");
  term_printString(" General:\n");
  term_printString("  clear   - Clear the terminal screen\n");
  term_printString("  exit    - Exit the terminal\n");
  term_printString("  help    - Show available commands\n");
}

void cmdClear(const char *arg) { term_clearScreen(); }

void cmdExit(const char *arg) {
  term_printString("Exiting terminal...\n");
  // Send continue to desktop command
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
}

// --- The network and the catalog refresh, behind the menu ------------------

// The menu shows the network and the last download on its status lines:
// redraw it when they change, unless a list or another screen is up.
static void menuStatusChanged(void) {
  if (listKind == LIST_NONE && menuState.menuLevel == TERM_ROMS_MENU_MAIN) {
    menu();
    display_refresh();
  }
}

static const char *catalogUrl(void) {
#if APP_DOWNLOAD_HTTPS == 1
  SettingsConfigEntry *entry = settings_find_entry(
      aconfig_getContext(), ACONFIG_PARAM_ROM_HTTPS_CATALOG);
#else
  SettingsConfigEntry *entry =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_HTTP_CATALOG);
#endif
  return (entry != NULL && entry->value[0] != '\0') ? entry->value : NULL;
}

// Why a download failed, in a few words.
static void describeDownload(download_err_t err, char *out, size_t size) {
  switch (err) {
    case DOWNLOAD_HTTPSTATUS_ERROR:
      snprintf(out, size, "the server answered %d", download_getHttpStatus());
      break;
    case DOWNLOAD_TIMEOUT_ERROR:
      snprintf(out, size, "the server stopped answering");
      break;
    case DOWNLOAD_TRANSFER_ERROR:
      snprintf(out, size, "the connection failed");
      break;
    case DOWNLOAD_CANNOTSTARTDOWNLOAD_ERROR:
      snprintf(out, size, "no connection to the server");
      break;
    case DOWNLOAD_FORCEDABORT_ERROR:
      snprintf(out, size, "it was stopped");
      break;
    case DOWNLOAD_TOOMANYREDIRECTS_ERROR:
      snprintf(out, size, "too many redirects");
      break;
    case DOWNLOAD_HTTPSNOTBUILT_ERROR:
      snprintf(out, size, "https:// is not in this build");
      break;
    case DOWNLOAD_URLTOOLONG_ERROR:
    case DOWNLOAD_CANNOTPARSEURL_ERROR:
    case DOWNLOAD_UNSUPPORTEDSCHEME_ERROR:
      snprintf(out, size, "the URL is not usable");
      break;
    case DOWNLOAD_CANNOTOPENFILE_ERROR:
    case DOWNLOAD_CANNOTCLOSEFILE_ERROR:
    case DOWNLOAD_CANNOTREADFILE_ERROR:
    case DOWNLOAD_CANNOTRENAMEFILE_ERROR:
      snprintf(out, size, "the SD card refused the file");
      break;
    default:
      snprintf(out, size, "error %d", (int)err);
      break;
  }
}

static void catalogRefreshStart(void) {
  if (downloadKind != DOWNLOAD_KIND_NONE || netState != NET_UP) {
    return;
  }
  if (!sdcard_isMounted()) {
    // Tried again when a card is mounted (cardPoll()).
    catalogRefresh = CATALOG_REFRESH_FAILED;
    snprintf(catalogReason, sizeof(catalogReason), "no SD card");
    return;
  }
  const char *url = catalogUrl();
  if (url == NULL) {
    catalogRefresh = CATALOG_REFRESH_FAILED;
    snprintf(catalogReason, sizeof(catalogReason), "no catalog URL is set");
    return;
  }
  DPRINTF("Catalog URL: %s\n", url);
  download_setFilepath(url);
  download_err_t err = download_start();
  if (err != DOWNLOAD_OK) {
    catalogRefresh = CATALOG_REFRESH_FAILED;
    describeDownload(err, catalogReason, sizeof(catalogReason));
    return;
  }
  downloadKind = DOWNLOAD_KIND_CATALOG;
  catalogRefresh = CATALOG_REFRESH_RUNNING;
}

static void netConnect(void);

#if defined(_DEBUG) && (_DEBUG != 0)
// The Wi-Fi debug hook: down as a lost network would be (the station leaves
// the AP), or a new connection.
static void netTestWifi(bool connect) {
  if (connect) {
    netAttempts = 0;
    netConnect();
  } else {
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    netState = NET_FAILED;
    netReason = "Wi-Fi went down.";
  }
  menuStatusChanged();
}
#endif

static void netConnect(void) {
  netAttempts++;
  wifi_sta_conn_process_status_t result = network_wifiStaConnectStart();
  if (result == NETWORK_WIFI_STA_CONN_OK) {
    netState = NET_CONNECTING;
    return;
  }
  netState = NET_FAILED;
  netReason = (result == NETWORK_WIFI_STA_CONN_ERR_NO_SSID)
                  ? "Wi-Fi is not configured."
                  : "Wi-Fi did not connect.";
}

// At boot, once the menu is up. Booster owns the Wi-Fi settings.
static void netStart(void) {
  netAttempts = 0;
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  if (wifiMode == NULL || (wifi_mode_t)atoi(wifiMode->value) == WIFI_MODE_AP) {
    netState = NET_OFF;
    netReason = "Wi-Fi is not configured.";
  } else if (network_wifiInit(WIFI_MODE_STA) != 0) {
    netState = NET_FAILED;
    netReason = "The Wi-Fi chip did not start.";
  } else {
    netConnect();
  }
  menuStatusChanged();
}

// One step of the connection, from the main loop: three attempts, as before.
static void netPoll(void) {
  if (netState != NET_CONNECTING) {
    return;
  }
  health_setPhase(HEALTH_PHASE_WIFI_CONNECT);
  wifi_sta_conn_process_status_t result = network_wifiStaConnectPoll();
  if (result == NETWORK_WIFI_STA_CONN_IN_PROGRESS) {
    return;
  }
  if (result == NETWORK_WIFI_STA_CONN_OK) {
    netState = NET_UP;
    catalogRefreshStart();
  } else if (result == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT &&
             netAttempts < NET_CONNECT_ATTEMPTS) {
    netConnect();
    if (netState == NET_CONNECTING) {
      return;
    }
  } else {
    netState = NET_FAILED;
    netReason = "Wi-Fi did not connect.";
  }
  menuStatusChanged();
}

// The download running, from the main loop: the catalog replaces the card's
// copy only when complete (download.c), a ROM is selected only when complete.
static void downloadsPoll(void) {
  if (downloadKind == DOWNLOAD_KIND_NONE) {
    return;
  }
  health_setPhase((downloadKind == DOWNLOAD_KIND_CATALOG)
                      ? HEALTH_PHASE_CATALOG_DOWNLOAD
                      : HEALTH_PHASE_ROM_DOWNLOAD);
  download_status_t status = download_getStatus();
  if (status == DOWNLOAD_STATUS_STARTED ||
      status == DOWNLOAD_STATUS_IN_PROGRESS) {
    download_poll();
    return;
  }
  if (status != DOWNLOAD_STATUS_COMPLETED && status != DOWNLOAD_STATUS_FAILED) {
    return;
  }
  download_err_t err = download_finish();
  if (err == DOWNLOAD_OK) {
    err = download_confirm();
  }
  download_setStatus(DOWNLOAD_STATUS_IDLE);
  DownloadKind kind = downloadKind;
  downloadKind = DOWNLOAD_KIND_NONE;
  char reason[TERM_SCREEN_SIZE_X];
  if (kind == DOWNLOAD_KIND_CATALOG) {
    if (err == DOWNLOAD_OK) {
      catalogRefresh = CATALOG_REFRESH_DONE;
      const download_url_components_t *origin = download_getUrlComponents();
      snprintf(catalogOrigin.protocol, sizeof(catalogOrigin.protocol), "%s",
               origin->protocol);
      snprintf(catalogOrigin.host, sizeof(catalogOrigin.host), "%s",
               origin->host);
      catalogOrigin.port = origin->port;
      catalogOrigin.known = true;
    } else {
      catalogRefresh = CATALOG_REFRESH_FAILED;
      describeDownload(err, catalogReason, sizeof(catalogReason));
      DPRINTF("Catalog refresh failed: %s\n", catalogReason);
    }
  } else if (err == DOWNLOAD_OK) {
    // The name the file was saved under, the one the download started for.
    const char *saved = download_getFilename();
    const char *name =
        (saved != NULL && saved[0] != '\0') ? saved : downloadRomName;
    // A whole 2xx can still be the wrong thing (an error page served as
    // 200): the file must be the size the catalog gives, to a KB.
    char path[MAX_PATH_SIZE];
    snprintf(path, sizeof(path), "%s/%s", romsFolder, name);
    FILINFO fno;
    if (downloadRomSizeKb > 0 && f_stat(path, &fno) == FR_OK) {
      uint32_t gotKb =
          (uint32_t)((fno.fsize + BYTES_PER_KB - 1U) / BYTES_PER_KB);
      if (gotKb + 1 < downloadRomSizeKb || gotKb > downloadRomSizeKb + 1) {
        f_unlink(path);
        snprintf(downloadMessage, sizeof(downloadMessage),
                 "Download failed: %lu KB, catalog %lu KB",
                 (unsigned long)gotKb, (unsigned long)downloadRomSizeKb);
        menuStatusChanged();
        return;
      }
    }
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED, name);
    settings_save(aconfig_getContext(), true);
    snprintf(downloadMessage, sizeof(downloadMessage), DOWNLOADED_PREFIX "%.*s",
             (int)(TERM_SCREEN_SIZE_X - sizeof(DOWNLOADED_PREFIX)), name);
  } else {
    // A write that failed because the card left reads as a transfer error:
    // ask the card, and say so.
    if (sdcard_checkPresence()) {
      describeDownload(err, reason, sizeof(reason));
    } else {
      snprintf(reason, sizeof(reason), "the SD card is gone");
    }
    snprintf(downloadMessage, sizeof(downloadMessage), "Download failed: %s",
             reason);
  }
  menuStatusChanged();
}

// --- The lists ---------------------------------------------------------------

static void listClose(void) {
  term_setKeyHandler(NULL);
  free(sdRoms);
  sdRoms = NULL;
  sdRomsCount = 0;
  catalog_close(&catalog);
  catalogPageCached = UINT32_MAX;
  listKind = LIST_NONE;
  listDetails = false;
}

static const char *listName(uint32_t index) {
  if (listKind == LIST_CARD) {
    return sdRoms[index].name;
  }
  uint32_t page = index / NAVLIST_PAGE_LINES;
  if (page != catalogPageCached) {
    uint32_t startUs = time_us_32();
    catalog_readPage(&catalog, page, catalogNames, NAVLIST_PAGE_LINES);
    DPRINTF("Catalog page %lu read in %lu us\n", (unsigned long)page,
            (unsigned long)(time_us_32() - startUs));
    catalogPageCached = page;
  }
  return catalogNames[index % NAVLIST_PAGE_LINES];
}

// Two lines at most above the catalog: why it may be old.
static void catalogNotice(void) {
  if (netState == NET_CONNECTING || catalogRefresh == CATALOG_REFRESH_RUNNING) {
    term_printString("Refreshing the catalog...\n");
  } else if (netState != NET_UP) {
    term_printString("Offline: the copy on the SD card.\n");
    term_printString(netReason);
    term_printString("\n");
  } else if (catalogRefresh == CATALOG_REFRESH_FAILED) {
    term_printString("Offline: the copy on the SD card.\n");
    term_printString("Server: ");
    term_printString(catalogReason);
    term_printString("\n");
  }
}

static void listDraw(void) {
  showTitle();
  if (listKind == LIST_CARD) {
    term_printString("ROMs on the SD card\n");
  } else {
    term_printString("ROM catalog\n");
    catalogNotice();
  }
  uint32_t first = navlist_first(&listNav);
  uint32_t shown = navlist_onPage(&listNav);
  for (uint32_t i = 0; i < shown; i++) {
    char line[TERM_SCREEN_SIZE_X + 1];
    snprintf(line, sizeof(line), "%c %.*s\n",
             (first + i == listNav.selected) ? '>' : ' ',
             TERM_SCREEN_SIZE_X - 3, listName(first + i));
    term_printString(line);
  }
  char footer[TERM_SCREEN_SIZE_X + 1];
  snprintf(footer, sizeof(footer), "\nPage %lu/%lu, %lu ROMs\n",
           (unsigned long)(navlist_page(&listNav) + 1),
           (unsigned long)navlist_pages(&listNav),
           (unsigned long)listNav.count);
  term_printString(footer);
  term_printString("UP/DOWN select, LEFT/RIGHT page\n");
  term_printString("RETURN details, ESC menu");
  display_refresh();
}

static bool catalogEntryAllowed(void) {
  switch (catalog_check(&catalogEntry)) {
    case CATALOG_ENTRY_TOO_LARGE: {
      char line[TERM_SCREEN_SIZE_X * 2];
      snprintf(line, sizeof(line),
               "\nToo large: %lu KB, the limit is 128 KB.\n",
               (unsigned long)catalogEntry.sizeKb);
      term_printString(line);
      return false;
    }
    case CATALOG_ENTRY_BAD_NAME:
      term_printString("\nRefused: its file name is not safe.\n");
      return false;
    case CATALOG_ENTRY_LONG_NAME:
      term_printString("\nRefused: its file name is too long.\n");
      return false;
    default:
      break;
  }
  if (netState != NET_UP) {
    term_printString("\nThe network is needed to download it:\n");
    term_printString(netReason);
    term_printString("\nFix Wi-Fi in Booster, then restart.\n");
    return false;
  }
  if (!catalogOrigin.known) {
    term_printString("\nThe catalog has not been refreshed\n");
    term_printString("from its server yet: try again soon.\n");
    return false;
  }
  if (downloadKind != DOWNLOAD_KIND_NONE) {
    term_printString("\nA download is running: wait for it.\n");
    return false;
  }
  return true;
}

static void listDetailsDraw(void) {
  showTitle();
  term_printString("\n");
  if (listKind == LIST_CARD) {
    char name[SETTINGS_MAX_VALUE_LENGTH];
    romFullName(sdRoms[listNav.selected].open, name, sizeof(name));
    term_printString("ROM: ");
    term_printString(name);
    term_printString("\n\nRETURN launch, ESC back");
    display_refresh();
    return;
  }
  if (catalog_readEntry(&catalog, listNav.selected, &catalogEntry) !=
      CATALOG_OK) {
    term_printString("The catalog could not be read.\n\nESC back");
    display_refresh();
    return;
  }
  char line[TERM_SCREEN_SIZE_X * 3];
  snprintf(line, sizeof(line), "Name: %s\nFile: %s\n", catalogEntry.name,
           catalog_fileName(&catalogEntry));
  term_printString(line);
  snprintf(line, sizeof(line), "Description: %s\nTags: %s\nSize: %lu KB\n",
           catalogEntry.description, catalogEntry.tags,
           (unsigned long)catalogEntry.sizeKb);
  term_printString(line);
  if (catalogEntryAllowed()) {
    term_printString("\nRETURN download, ESC back");
  } else {
    term_printString("\nESC back");
  }
  display_refresh();
}

static void listAct(void) {
  if (listKind == LIST_CARD) {
    char name[SETTINGS_MAX_VALUE_LENGTH];
    romFullName(sdRoms[listNav.selected].open, name, sizeof(name));
    // The pick is held in the settings in RAM and reaches flash with MODE,
    // after the launch has written and read back the ROM.
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED, name);
    listClose();
    showTitle();
    cmdLaunch(NULL);
    return;
  }
  if (!catalogEntryAllowed()) {
    return;
  }
  char encoded[CATALOG_URL_BYTES * 3];
  char url[DOWNLOAD_URL_SIZE];
  bool fits = catalog_urlEncodePath(catalogEntry.url, encoded, sizeof(encoded));
  int length =
      (catalogOrigin.port != 0)
          ? snprintf(url, sizeof(url), "%s://%s:%u/%s", catalogOrigin.protocol,
                     catalogOrigin.host, (unsigned)catalogOrigin.port, encoded)
          : snprintf(url, sizeof(url), "%s://%s/%s", catalogOrigin.protocol,
                     catalogOrigin.host, encoded);
  if (!fits || length < 0 || (size_t)length >= sizeof(url)) {
    term_printString("\nRefused: its URL is too long.\n");
    return;
  }
  DPRINTF("Downloading ROM: %s\n", url);
  download_setFilepath(url);
  download_err_t err = download_start();
  if (err != DOWNLOAD_OK) {
    char reason[TERM_SCREEN_SIZE_X];
    describeDownload(err, reason, sizeof(reason));
    term_printString("\nThe download did not start:\n");
    term_printString(reason);
    term_printString("\n");
    return;
  }
  downloadKind = DOWNLOAD_KIND_ROM;
  snprintf(downloadRomName, sizeof(downloadRomName), "%s",
           catalog_fileName(&catalogEntry));
  downloadRomSizeKb = catalogEntry.sizeKb;
  snprintf(downloadMessage, sizeof(downloadMessage), DOWNLOADING_PREFIX "%.*s",
           (int)(TERM_SCREEN_SIZE_X - sizeof(DOWNLOADING_PREFIX)),
           downloadRomName);
  listClose();
  menu();
  display_refresh();
}

static void listKey(char key) {
  if (listDetails) {
    if (key == '\r' || key == '\n') {
      listAct();
    } else if (key == TERM_KEY_ESC || key == 'm' || key == 'M') {
      listDetails = false;
      listDraw();
    }
    return;
  }
  switch (navlist_key(&listNav, key)) {
    case NAVLIST_MOVED:
    case NAVLIST_PAGE_TURNED:
      listDraw();
      break;
    case NAVLIST_CHOSEN:
      listDetails = true;
      listDetailsDraw();
      break;
    case NAVLIST_LEAVE:
      listClose();
      menu();
      display_refresh();
      break;
    default:
      break;
  }
}

// No card mounted: say so, and that one put in is found by itself.
static bool cardMissing(void) {
  if (sdcard_isMounted()) {
    return false;
  }
  term_printString("No SD card. Put one in: it is found\nby itself.\n");
  return true;
}

// The card going or coming, seen by sdcard_pollRemount() or by a failed
// operation's presence check. An open list read the old card (and the
// catalog's page index is the old card's), so it closes; a new card needs
// its own copy of the catalog.
static bool cardWasMounted = false;

static void cardPoll(void) {
  bool mounted = sdcard_isMounted();
  if (mounted == cardWasMounted) {
    return;
  }
  cardWasMounted = mounted;
  DPRINTF("SD card %s\n", mounted ? "mounted" : "gone");
  if (listKind != LIST_NONE) {
    listClose();
    menu();
    display_refresh();
  } else {
    menuStatusChanged();
  }
  if (mounted && catalogRefresh != CATALOG_REFRESH_RUNNING) {
    catalogRefreshStart();
  }
}

static void listOpen(ListKind kind, uint32_t count) {
  listKind = kind;
  listDetails = false;
  navlist_reset(&listNav, count);
  term_setKeyHandler(listKey);
  listDraw();
}

void cmdCard(const char *arg) {
  if (cardMissing()) {
    return;
  }
  FRESULT listed = readRomsSdcard(romsFolder);
  if (listed == FR_NOT_ENOUGH_CORE) {
    term_printString("Not enough memory for the ROM list.\n");
    return;
  }
  if (listed != FR_OK) {
    listClose();
    if (!sdcard_checkPresence()) {
      cardMissing();
      return;
    }
    term_printString("The folder '");
    term_printString(romsFolder);
    term_printString(
        "' could not be read\nfrom the SD card. Check FOLDER in\n"
        "[S]ettings.\n\n");
    return;
  }
  if (sdRomsCount == 0) {
    listClose();
    term_printString("No ROMs found in the SD card.\n");
    term_printString("Download ROMs from internet,\n");
    term_printString("or copy them to folder '");
    term_printString(romsFolder);
    term_printString("'\n\n");
    return;
  }
  listOpen(LIST_CARD, sdRomsCount);
}

void cmdNetwork(const char *arg) {
  if (cardMissing()) {
    return;
  }
  // A refresh that failed is tried again here, not in a loop.
  if (netState == NET_UP && catalogRefresh == CATALOG_REFRESH_FAILED) {
    catalogRefreshStart();
  }
  char path[MAX_PATH_SIZE];
  snprintf(path, sizeof(path), "%s/roms.csv", romsFolder);
  uint32_t openUs = time_us_32();
  catalog_result_t opened = catalog_open(&catalog, path);
  DPRINTF("Catalog indexed in %lu us\n",
          (unsigned long)(time_us_32() - openUs));
  if (opened != CATALOG_OK || catalog.count == 0) {
    catalog_close(&catalog);
    if (opened == CATALOG_READ_ERROR && !sdcard_checkPresence()) {
      cardMissing();
      return;
    }
    if (netState == NET_CONNECTING ||
        catalogRefresh == CATALOG_REFRESH_RUNNING) {
      term_printString("The catalog is still downloading:\n");
      term_printString("try [D] again in a moment.\n");
    } else if (opened == CATALOG_NO_MEMORY) {
      term_printString("Not enough memory for the catalog.\n");
    } else {
      term_printString("No catalog on the SD card");
      if (netState != NET_UP) {
        term_printString(", and no\nnetwork: ");
        term_printString(netReason);
        term_printString("\nSet up Wi-Fi in Booster.\n");
      } else {
        term_printString(":\n");
        term_printString(catalogReason[0] ? catalogReason : "empty");
        term_printString("\n");
      }
    }
    return;
  }
  catalogPageCached = UINT32_MAX;
  listOpen(LIST_CATALOG, catalog.count);
}

// The selection, saved only once the ROM is in ROM_TEMP and read back.
static void cmdLaunchSelect(void) {
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                       delayMode ? ROM_MODE_DELAY : ROM_MODE_DIRECT);
  settings_save(aconfig_getContext(), true);
}

void cmdLaunch(const char *arg) {
  SettingsConfigEntry *romFile =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED);
  if (romFile == NULL || romFile->value[0] == '\0') {
    // The menu hides [L] then, but the key still arrives.
    romstoreReport(ROMSTORE_NOT_SELECTED, NULL);
    display_refresh();
    return;
  }
  if (cardMissing()) {
    return;
  }
  char filename[MAX_PATH_SIZE];
  snprintf(filename, sizeof(filename), "%s/%s", romsFolder, romFile->value);
  DPRINTF("Loading ROM file into FLASH: %s\n", filename);
  romstore_info_t info;
  romFlashBegin();
  romstore_result_t result = romstore_launch(filename, FLASH_ROM_LOAD_OFFSET,
                                             &romFlash, &info, cmdLaunchSelect);
  romFlashEnd();
  if (result != ROMSTORE_OK) {
    DPRINTF("Launch failed: %s\n", romstore_message(result));
    romstoreReport(result, &info);
    if ((result == ROMSTORE_READ_ERROR || result == ROMSTORE_NOT_FOUND) &&
        !sdcard_checkPresence()) {
      term_printString("The SD card is gone.\n");
    }
    // The message stays until [M]: a status redraw (the card, the network)
    // would wipe it before it was read.
    menuState.menuLevel = TERM_ROMS_MENU_LAUNCH;
    term_printString("Press [M] for the menu.\n");
    display_refresh();
    return;
  }

  menuState.menuLevel = TERM_ROMS_MENU_LAUNCH;
  term_printString("\n\nThe ROM will boot shortly...\n\n");
  if (delayMode) {
    term_printString(
        "ROM delay/ripper mode enabled. You must press SELECT to activate the "
        "ROM.\n");
  }
  term_printString("To return to this menu, press SELECT\n");
  term_printString("If ROM doesn't boot, reset the computer\n");
  keepActive = false;  // Exit the active loop
}

// A line that is not a command: the menu again (the lists take their keys
// one at a time, through listKey()).
void cmdUnknown(const char *arg) {
  (void)arg;
  if (menuState.menuLevel == TERM_ROMS_MENU_MAIN) {
    menu();
  } else {
    term_printString("Unknown command. Type 'help' for a list of commands.\n");
  }
}

void cmdBooster(const char *arg) {
  menuState.menuLevel = TERM_ROMS_MENU_BOOSTER;
  term_printString("Launching Booster app...\n");
  term_printString("The computer will boot shortly...\n\n");
  term_printString("If it doesn't boot, power it on and off.\n");
  resetDeviceAtBoot = false;  // Jump to the booster app
  keepActive = false;         // Exit the active loop
}

#if defined(_DEBUG) && (_DEBUG != 0)
static void cmdFirmware(const char *arg) {
  if (!chandler_stPresent()) {
    // The user firmware relies on what the ST publishes at boot (the machine
    // type, for a Mega STE's cache), and this RP has not heard it yet.
    term_printString("\nReset the Atari ST first.\n");
    return;
  }
  term_printString("Launching user firmware on the Atari ST...\n");
  // The ST's check_commands polls the sentinel and jumps to USERFW on
  // CMD_START. The release cartridge's userfw.s returns to TOS.
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_START);
}
#endif

void cmdDelay(const char *arg) {
  // Change the ROM mode
  delayMode = !delayMode;
  menuState.menuLevel = TERM_ROMS_MENU_MAIN;
  menu();
}

// This section contains the functions that are called from the main loop

static bool getKeepActive() { return keepActive; }

static bool getResetDevice() { return resetDeviceAtBoot; }

void failure(const char *message) {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString(message);

  display_refresh();
}

static void init(const char *folder) {
  // Store the ROMs folder, if not NULL or empty
  if (folder != NULL && strlen(folder) > 0) {
    strncpy(romsFolder, folder, MAX_PATH_SIZE - 1);
  }

  // Set the command table
  term_setCommands(commands, numCommands);

  // Clear the screen
  term_clearScreen();

  // Display the menu
  menu();

  display_refresh();
}

// ROM mode: SELECT, polled on core 0. A short press either starts the ROM
// (in the Delay wait) or ends ROM mode; a 10 s press is the factory reset.
static volatile bool selectPressed = false;
static void romModeSelectPressed(void) { selectPressed = true; }

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds only, set over SWD by symbol: the ROM-mode wait stops feeding
// the watchdog, as a hang there would, so its reboot can be checked.
volatile uint32_t romModeTestHang = 0;
#endif

static void __not_in_flash_func(romModeWaitForSelect)(void) {
  selectPressed = false;
  while (!selectPressed) {
#if defined(_DEBUG) && (_DEBUG != 0)
    while (romModeTestHang != 0) {
      tight_loop_contents();
    }
#endif
    health_feed();
    select_poll();
    sleep_ms(1);
  }
}

// ROM mode's boot race, read over SWD (release ELFs keep their symbols):
//   romModeLiveUs            the timer when the engine started; a reset
//                            zeroes the timer, so it is the time from reset
//   romModeAccessBeforeLive  1 when the ST had already read the cartridge
//                            then: the !ROM4 falling-edge latch was set
//   romModeFirstAccessUs     the ST's first cartridge access after that, 0
//                            while there has been none
volatile uint32_t romModeLiveUs = 0;
volatile uint32_t romModeAccessBeforeLive = 0;
volatile uint32_t romModeFirstAccessUs = 0;

// IO_BANK0 keeps eight GPIOs' four event bits in each interrupt register.
#define GPIOS_PER_IRQ_REG 8
#define ROM4_EDGE_FALL_BITS \
  (GPIO_IRQ_EDGE_FALL << (4 * (ROM4_GPIO % GPIOS_PER_IRQ_REG)))

// One-shot: records the first access and switches itself off, with register
// writes only (no flash code in an interrupt).
static void __not_in_flash_func(romModeFirstAccessIrq)(void) {
  if ((gpio_get_irq_event_mask(ROM4_GPIO) & GPIO_IRQ_EDGE_FALL) == 0U) {
    return;
  }
  gpio_acknowledge_irq(ROM4_GPIO, GPIO_IRQ_EDGE_FALL);
  romModeFirstAccessUs = time_us_32();
  hw_clear_bits(
      &io_bank0_hw->proc0_irq_ctrl.inte[ROM4_GPIO / GPIOS_PER_IRQ_REG],
      ROM4_EDGE_FALL_BITS);
}

// Called the moment the engine serves the bus: takes the time, reads the
// latch of edges since reset, then arms the one-shot for the next access.
static void romModeMarkLive(void) {
  romModeLiveUs = time_us_32();
  romModeAccessBeforeLive = (io_bank0_hw->intr[ROM4_GPIO / GPIOS_PER_IRQ_REG] &
                             ROM4_EDGE_FALL_BITS) != 0U;
  gpio_acknowledge_irq(ROM4_GPIO, GPIO_IRQ_EDGE_FALL);
  gpio_add_raw_irq_handler(ROM4_GPIO, romModeFirstAccessIrq);
  gpio_set_irq_enabled(ROM4_GPIO, GPIO_IRQ_EDGE_FALL, true);
  irq_set_enabled(IO_IRQ_BANK0, true);
}

static void romMode(int appModeValue) {
  select_configure();
  select_setResetCallback(romModeSelectPressed);
  select_setLongResetCallback(emul_quiesceAndFactoryReset);

  if (appModeValue == ROM_MODE_DELAY) {
    // Delay/Ripper mode: the ST boots without the cartridge, as the old
    // ripper cartridges worked, until SELECT is pressed.
    DPRINTF("Delay mode: waiting for SELECT to start the ROM\n");
    health_setPhase(HEALTH_PHASE_DELAY_WAIT);
    romModeWaitForSelect();
  }

  // Copy the ROM in the flash to RAM, both banks
  DPRINTF("Copy the ROM firmware to RAM: 0x%X, length: %u bytes\n",
          (unsigned int)&_rom_temp_start, ROM_SIZE_BYTES * ROM_BANKS);
  COPY_FIRMWARE_TO_RAM((uint16_t *)&_rom_temp_start,
                       ROM_SIZE_WORDS * ROM_BANKS);
  if (romemul_initTwoBanks(false) < 0) {
    panic("romemul_initTwoBanks failed: PIO/DMA claim returned <0");
  }
  romModeMarkLive();
#if defined(_DEBUG) && (_DEBUG != 0)
  // Debug builds only: capture the ST's ROM3 reads, driving nothing and
  // answering nothing (the window is the ROM's), so `swd.py ring` reads what
  // a test cartridge sends, such as the self-check's verdict.
  if (commemul_init() < 0) {
    DPRINTF("ROM3 capture not started\n");
  }
#endif
  DPRINTF("ROM mode live at %lu us; the ST read the cartridge before: %s\n",
          (unsigned long)romModeLiveUs, romModeAccessBeforeLive ? "yes" : "no");

#ifdef BLINK_H
  blink_on();
#endif

  DPRINTF("ROM emulation mode started. Waiting for SELECT button\n");
  health_setPhase(HEALTH_PHASE_ROM_MODE);
  romModeWaitForSelect();
  DPRINTF("SELECT button pressed: back to the setup menu\n");

  // Engine off, then the save, then the reset.
  emul_quiesce();

  // Set the ROM emulation mode to 255 (setup menu)
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                       ROM_MODE_SETUP);
  settings_save(aconfig_getContext(), true);

#ifdef BLINK_H
  blink_off();
#endif

  // Now reset the device
  reset_device();
}

void emul_start() {
  // 0. Why the RP started, then the watchdog, in both modes (D-14): from
  // here a hang reboots the RP and says where it was.
  health_init();
  health_watchdogStart();

  // 1. ROM mode or setup mode, from the settings
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_MODE);
  int appModeValue = ROM_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF("MODE not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start ROM emulation in mode: %i\n", appModeValue);
  }
  // Crashing again and again: setup mode for this boot, with no autorun, and
  // the reason on the menu. The settings are not changed.
  bool crashLoop = health_isCrashLoop();
  if (crashLoop && appModeValue != ROM_MODE_SETUP) {
    DPRINTF("Crash loop: setup mode instead of ROM mode %i\n", appModeValue);
    appModeValue = ROM_MODE_SETUP;
  }

  // 2. ROM mode: the ST owns the whole window, no command channel.
  if ((appModeValue == ROM_MODE_DIRECT) || (appModeValue == ROM_MODE_DELAY)) {
    romMode(appModeValue);
  }

  // 3. Setup mode. The cartridge image the ST runs is copied into the window.
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
#if defined(_DEBUG) && (_DEBUG != 0)
  // The ST must see exactly the generated image.
  if (memcmp((const void *)&__rom_in_ram_start__, target_firmware,
             (size_t)target_firmware_length * sizeof(uint16_t)) != 0) {
    DPRINTF("ERROR: cartridge image in RAM does not match target_firmware\n");
  } else {
    DPRINTF("Cartridge image in RAM verified (%u words)\n",
            (unsigned)target_firmware_length);
  }
#endif

  // ROM4 reads are served by chained DMAs feeding the PIO, no CPU involved.
  // Without this engine the cartridge image is unreadable from the m68k, so a
  // failure here is fatal.
  if (init_romemul(false) < 0) {
    panic("init_romemul failed: PIO/DMA claim or program load returned <0");
  }

  // The ROM3 command capture (PIO + DMA ring), and the command handler that
  // polls the ring, parses the protocol and dispatches each command.
  if (commemul_init() < 0) {
    panic("commemul_init failed: PIO/DMA claim or program load returned <0");
  }
  chandler_init();
  chandler_addCB(term_command_cb);

  // After this point, the remote computer can execute the code

  // 4. The terminal, on the ST's screen
  display_setupU8g2();

  // Configure the SELECT button before anything slow (the SD card, the
  // network), so a press is seen from here on; its edge interrupt catches one
  // made while a wait cannot poll. A short press restarts the RP. A press held
  // for SELECT_LONG_RESET is a factory reset.
  select_configure();
  select_setResetCallback(emul_quiesceAndReset);
  select_setLongResetCallback(emul_quiesceAndFactoryReset);

  // 5. The SD card. Static, not on the stack: FatFs keeps a pointer to it
  // while the card is mounted.
  static FATFS fsys;
  SettingsConfigEntry *folder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_FOLDER);
  char *folderName = "/roms";
  if (folder == NULL) {
    DPRINTF("FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("FOLDER: %s\n", folder->value);
    folderName = folder->value;
  }
  health_setPhase(HEALTH_PHASE_SD_CARD);
  sdcard_status_t sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  // A card an RP restart left inside a write answers "not ready": freed and
  // mounted once more.
  sdcardErr = sdcard_recoverAtBoot(sdcardErr);
  // A failing card can take a few seconds to give up.
  health_feed();
  cardWasMounted = (sdcardErr == SDCARD_INIT_OK);
  if (sdcardErr != SDCARD_INIT_OK) {
    // No card, or none that mounts: the menu says so, and the main loop
    // mounts one as soon as it is put in (sdcard_pollRemount()).
    DPRINTF("Error initializing the SD card: %i\n", sdcardErr);
  } else {
    DPRINTF("SD card found & initialized\n");
    // The configured folder: romsFolder is set later, by init().
    AutorunResult autorunResult = crashLoop ? AUTORUN_ERR_AUTORUN_NOT_FOUND
                                            : autorunIfRequested(folderName);
    if (autorunResult != AUTORUN_OK) {
      DPRINTF("Autorun error: %i. Continue.\n", autorunResult);
    }
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // 6. The menu first: the network and the catalog refresh come up behind
  // it, from the main loop (netPoll(), downloadsPoll()), so the menu never
  // waits for them.
  init(folderName);
  netStart();

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. The main loop, until a ROM is launched or Booster is chosen
#if defined(_DEBUG) && (_DEBUG != 0)
  // Debug builds only: serve the SWD mailbox of tools/dev/swd.py
  devhooks_setAppHandler(emul_devhooksApp);
#endif

  DPRINTF("Start the app loop here\n");
  absolute_time_t nextNetworkPoll = get_absolute_time();
  while (getKeepActive()) {
    health_feed();
    health_setPhase(HEALTH_PHASE_MAIN_LOOP);
    devhooks_poll();
    devdownload_poll();
    select_poll();
    // Drain the ROM3 command ring and dispatch to the registered callbacks on
    // every pass: the ST spins on its answer, so the loop never waits.
    chandler_loop();
    if (chandler_consumeStBoot()) {
      // A new ST session: nothing typed before the reset carries over, and
      // the ST gets a freshly drawn menu.
      term_clearInputBuffer();
      listClose();
      menu();
      display_refresh();
    }
#if PICO_CYW43_ARCH_POLL
    // Wi-Fi every 10 ms, which is plenty for lwIP's timers and leaves the
    // loop to the command ring.
    if (absolute_time_diff_us(nextNetworkPoll, get_absolute_time()) >= 0) {
      network_safePoll();
      nextNetworkPoll = make_timeout_time_ms(NETWORK_POLL_MS);
    }
#endif

    // Run the terminal foreground (consume the published command, render
    // output, etc.).
    term_loop();

    // The card going or coming, the network coming up, the download running
    sdcard_pollRemount();
    cardPoll();
    netPoll();
    downloadsPoll();
    // A hang past here is the loop's, not the connect's or the download's.
    health_setPhase(HEALTH_PHASE_MAIN_LOOP);

    // Heap sampling, the debug summary and the debug test hooks
    health_tick();
  }
  listClose();

  // 10. Reset the computer, then this device or Booster. Hold the command
  // long enough for the ST's menu loop to see it, and keep answering: a
  // keystroke in flight would otherwise keep the ST in its send, retrying,
  // until the hold was over.
  // The ST resets when its menu loop reads the command, after its own
  // PRE_RESET_WAIT (main.s: about 2.4 s at 8 MHz). A launch restarts the RP
  // within that wait, so the ST's TOS finds the new ROM.
  emul_serviceFor(SLEEP_LOOP_MS);
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  if (getResetDevice()) {
    emul_serviceFor(SENTINEL_HOLD_MS);
    emul_quiesceAndReset();
  } else {
    // Booster: keep the command until the ST has rebooted (its TOS has read
    // this cartridge's header), then put the no-op back, so the ST cannot
    // reset again on this menu, and only then stop the engines and hand the
    // window over.
    bool stRebooted = emul_waitForStReboot(BOOSTER_HANDOVER_MAX_MS);
    SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_NOP);
    DPRINTF("The ST %s\n", stRebooted
                               ? "rebooted: its cartridge header was read"
                               : "did not reboot (nothing listening)");
    // Before jumping to the booster app, clean the settings: no ROM selected,
    // setup mode
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED, "");
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                         ROM_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Jump to the booster app, which does not feed the watchdog
    DPRINTF("Jumping to the booster app...\n");
    emul_quiesce();
    health_prepareJump();
    reset_jump_to_booster();
  }
}
