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
#if APP_DOWNLOAD_HTTPS
#include "httpc/httpc.h"
#include "mbedtls/memory_buffer_alloc.h"
#endif
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
#include "ui.h"

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

// The menu as menu() drew it, only its prompt's row changed since: its status
// is redrawn in place. Anything printed below the prompt ends that.
static bool menuIntact = false;

// The terminal's own commands print below the prompt.
static void cmdSettings(const char *arg);
static void cmdPrint(const char *arg) {
  menuIntact = false;
  term_cmdPrint(arg);
}
static void cmdSave(const char *arg) {
  menuIntact = false;
  term_cmdSave(arg);
}
static void cmdErase(const char *arg) {
  menuIntact = false;
  term_cmdErase(arg);
}
static void cmdGet(const char *arg) {
  menuIntact = false;
  term_cmdGet(arg);
}
static void cmdPutInt(const char *arg) {
  menuIntact = false;
  term_cmdPutInt(arg);
}
static void cmdPutBool(const char *arg) {
  menuIntact = false;
  term_cmdPutBool(arg);
}
static void cmdPutString(const char *arg) {
  menuIntact = false;
  term_cmdPutString(arg);
}

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
    {"s", cmdSettings},
    {"settings", cmdSettings},
    {"print", cmdPrint},
    {"save", cmdSave},
    {"erase", cmdErase},
    {"get", cmdGet},
    {"put_int", cmdPutInt},
    {"put_bool", cmdPutBool},
    {"put_str", cmdPutString},
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
// Joining again, as md-devops' link supervisor does: after a lost link (a
// grace period first, since an ordinary reconnect can finish by itself) and
// after a join that did not work, one more try each time, later and later,
// never while a download runs (it fails first). Not for what a try cannot
// fix (no Wi-Fi settings, a chip that did not start).
#define NET_LINK_DOWN_GRACE_MS 5000U
#define NET_REJOIN_BACKOFF_MIN_MS 5000U
#define NET_REJOIN_BACKOFF_MAX_MS 60000U
static bool netRetry = false;
static absolute_time_t netNextTryAt;
static uint32_t netBackoffMs = NET_REJOIN_BACKOFF_MIN_MS;
// Tries made since boot, and links lost: for SWD.
uint32_t netRejoins = 0;
uint32_t netLinkLosses = 0;

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

// Between sectors: the ST is answered, SELECT is seen, and the launch
// screen's bar moves on every 1/ROMSTORE_PROGRESS_MARKS of the write.
#define ROMSTORE_PROGRESS_MARKS 64
#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds only, set over SWD by symbol: each sector of a ROM write waits
// this long, answering the ST, so the write lasts long enough for a hand to
// pull the SD card in the middle of it. Any reset clears it.
volatile uint32_t romstoreTestSectorDelayMs = 0;
#endif

// The launch screen: a box with the ROM, the mode and the write's progress
#define LAUNCH_ROW_BOX 2
#define LAUNCH_ROW_ROM 3
#define LAUNCH_ROW_MODE 4
#define LAUNCH_ROW_BAR 6
#define LAUNCH_ROW_BOX_END 7
#define LAUNCH_ROW_NOTICE 9
#define LAUNCH_ROW_TEXT 9
#define LAUNCH_COL 2
#define LAUNCH_COL_VALUE 12
#define LAUNCH_VALUE_WIDTH (TERM_SCREEN_SIZE_X - LAUNCH_COL_VALUE - 2)
#define LAUNCH_BAR_COLS (TERM_SCREEN_SIZE_X - (2 * LAUNCH_COL))
#define PERCENT 100U

static bool launchScreenShown = false;

static void showTitle(void);

static void launchScreen(const char *heading, const char *name,
                         const char *mode) {
  showTitle();
  term_printAt(LAUNCH_ROW_BOX, LAUNCH_COL, heading);
  term_printAt(LAUNCH_ROW_ROM, LAUNCH_COL, "ROM");
  ui_printField(LAUNCH_ROW_ROM, LAUNCH_COL_VALUE, LAUNCH_VALUE_WIDTH, name);
  term_printAt(LAUNCH_ROW_MODE, LAUNCH_COL, "Mode");
  ui_printField(LAUNCH_ROW_MODE, LAUNCH_COL_VALUE, LAUNCH_VALUE_WIDTH, mode);
  ui_parkCursor();
  ui_group(LAUNCH_ROW_BOX, LAUNCH_ROW_BOX_END, LAUNCH_COL,
           (uint8_t)strlen(heading), UI_GLYPH_CARTRIDGE);
  ui_bar(LAUNCH_ROW_BAR, LAUNCH_COL, LAUNCH_BAR_COLS, 0, 1,
         "Reading the ROM from the SD card");
  launchScreenShown = true;
  display_refresh();
}

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
  uint32_t marks = done * ROMSTORE_PROGRESS_MARKS / total;
  if (launchScreenShown && marks > romstoreProgressMarks) {
    romstoreProgressMarks = marks;
    char text[TERM_SCREEN_SIZE_X];
    snprintf(text, sizeof(text), "Writing to flash   %lu%%",
             (unsigned long)(done * PERCENT / total));
    ui_bar(LAUNCH_ROW_BAR, LAUNCH_COL, LAUNCH_BAR_COLS, done, total, text);
    display_refresh();
  }
}

static const romstore_flash_t romFlash = {romFlashErase, romFlashProgram,
                                          romFlashRead, romFlashTick};

static void romFlashBegin(void) {
  health_setPhase(HEALTH_PHASE_FLASH_WRITE);
  romstoreWriteStartUs = time_us_32();
  romstoreProgressMarks = 0;
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

// Why a launch failed, in words for the ST.
static void romstoreDescribe(romstore_result_t result,
                             const romstore_info_t *info, char *out,
                             size_t outSize) {
  if (result == ROMSTORE_TOO_LARGE && info != NULL) {
    char size[NUMBER_TEXT_BYTES];
    char limit[NUMBER_TEXT_BYTES];
    formatThousands(size, sizeof(size), info->fileBytes);
    formatThousands(limit, sizeof(limit), ROMSTORE_MAX_BYTES);
    snprintf(out, outSize, "ROM too large: %s bytes, the limit is %s.", size,
             limit);
  } else {
    snprintf(out, outSize, "%s", romstore_message(result));
  }
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
  launchScreen("Autorun", filenameStart, "direct");
  romFlashBegin();
  romstore_result_t stored = romstore_launch(romPath, FLASH_ROM_LOAD_OFFSET,
                                             &romFlash, &info, autorunSelect);
  romFlashEnd();
  launchScreenShown = false;
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

// The title bar's icons: the SD card's, and Wi-Fi's (joining shows the
// reload arrows)
static uint8_t titleSdGlyph(void) {
  return sdcard_isMounted() ? UI_GLYPH_DRIVE : UI_GLYPH_NONE;
}

static uint8_t titleWifiGlyph(void) {
  if (netState == NET_UP) {
    return UI_GLYPH_WIFI;
  }
  return (netState == NET_CONNECTING) ? UI_GLYPH_RELOAD : UI_GLYPH_NONE;
}

// The strip under every screen without key hints: the version is in the
// title bar.
#define PRODUCT_STRIP \
  "SidecarTridge Multi-device   -   (C)2023-2026 GOODDATA LABS SL"

// Clears the screen and draws the title bar and the product strip (a screen
// with key hints draws its own); the cursor is left on row 1. The menu is no
// longer on the screen until menu() has drawn it again.
static void showTitle(void) {
  menuIntact = false;
  term_printString(
      "\x1B"
      "E"
      " ROM Emulator " RELEASE_VERSION "\n");
  ui_titleBar(titleSdGlyph(), titleWifiGlyph());
  ui_strip(PRODUCT_STRIP, true);
}

static const char *catalogUrl(void);
static void listClose(void);

// --- Notices -----------------------------------------------------------------
// A command's outcome or a refusal, set apart the same way on every screen:
// two inverted rows with the warning icon, in the small font.
#define NOTICE_ROWS 2
#define NOTICE_TEXT_BYTES (NOTICE_ROWS * TERM_SCREEN_SIZE_X)

// text on the rows from row (ui_notice()); blank rows for an empty text.
static void noticeDraw(uint8_t row, const char *text) {
  for (uint8_t line = 0; line < NOTICE_ROWS; line++) {
    ui_printField((uint8_t)(row + line), 0, TERM_SCREEN_SIZE_X, "");
  }
  if (text != NULL && text[0] != '\0') {
    ui_notice(row, NOTICE_ROWS, text);
  }
}

// --- The main menu -----------------------------------------------------------
// Its rows, in one table: a row added moves everything below it. Row 1 is
// left blank under the title bar. A group box takes its label's row (the top
// edge), its rows, and a row for the bottom edge.
#define MENU_ROW_ROM 2
#define MENU_ROW_LAUNCH 3
#define MENU_ROW_RIPPER 4
#define MENU_ROW_ROM_END 5
#define MENU_ROW_CARD 6
#define MENU_ROW_BROWSE 7
#define MENU_ROW_CARD_STATE 8
#define MENU_ROW_CARD_END 9
#define MENU_ROW_CATALOG 10
#define MENU_ROW_DOWNLOAD 11
#define MENU_ROW_WIFI 12
#define MENU_ROW_TRANSFER 13
#define MENU_ROW_CATALOG_END 14
#define MENU_ROW_DEVICE 15
#define MENU_ROW_SETTINGS 16
#define MENU_ROW_EXIT 17
#define MENU_ROW_BOOSTER 18
#define MENU_ROW_DEVICE_END 19
// Two rows for a notice, inverted, above the prompt; the last row stays blank
// above the strip.
#define MENU_ROW_NOTICE 20
#define MENU_ROW_PROMPT 22
// Inside a box: the label, the keys and the values' column (to column 37,
// left of the box's right edge)
#define MENU_COL_LABEL 2
#define MENU_COL_KEY 2
#define MENU_COL_VALUE 14
#define MENU_VALUE_WIDTH 24
#define MENU_ROW_WIDTH (TERM_SCREEN_SIZE_X - (2 * MENU_COL_KEY))
#define MENU_COL_PROMPT 1
#define MENU_PROMPT "Select an option: "
// The boxes' labels
#define MENU_LABEL_ROM "ROM"
#define MENU_LABEL_CARD "SD card"
#define MENU_LABEL_CATALOG "Catalog"
#define MENU_LABEL_DEVICE "Device"
#define LABEL_COLS(label) ((uint8_t)(sizeof(label) - 1))

// The last command's notice, empty for none: the rows then say why the RP
// last restarted, if that was not asked for.
static char menuNoticeText[NOTICE_TEXT_BYTES] = "";

// The menu's notice goes after NOTICE_SHOWN_MS: when it was drawn, 0 for
// none on the screen.
#define NOTICE_SHOWN_MS 5000U
static uint32_t menuNoticeShownUs = 0;

static void menuDrawNotice(void) {
  char bootLine[TERM_SCREEN_SIZE_X];
  const char *text = menuNoticeText;
  if (text[0] == '\0' && health_getBootLine(bootLine, sizeof(bootLine))) {
    text = bootLine;
  }
  noticeDraw(MENU_ROW_NOTICE, text);
  menuNoticeShownUs = (text[0] != '\0') ? (time_us_32() | 1U) : 0;
}

// From the main loop: the notice off the menu once its time is up.
static void menuNoticePoll(void) {
  if (menuNoticeShownUs == 0 ||
      time_us_32() - menuNoticeShownUs < NOTICE_SHOWN_MS * US_PER_MS) {
    return;
  }
  menuNoticeShownUs = 0;
  menuNoticeText[0] = '\0';
  if (menuIntact && listKind == LIST_NONE &&
      menuState.menuLevel == TERM_ROMS_MENU_MAIN) {
    noticeDraw(MENU_ROW_NOTICE, "");
    display_refresh();
  }
}

// The prompt, cleared of what was typed after it, with the cursor on it.
static void menuPrompt(void) {
  ui_printField(MENU_ROW_PROMPT, 0, TERM_SCREEN_SIZE_X, "");
  term_printAt(MENU_ROW_PROMPT, MENU_COL_PROMPT, MENU_PROMPT);
  ui_cursorAt(MENU_ROW_PROMPT,
              (uint8_t)(MENU_COL_PROMPT + sizeof(MENU_PROMPT) - 1));
  term_markMenuPromptCursor();
}

// The catalog box's last row: a ROM download's progress while it runs, then
// how it ended.
static void menuDrawTransfer(void) {
  if (downloadKind == DOWNLOAD_KIND_ROM) {
    uint32_t done = download_getBytesWritten();
    char text[TERM_SCREEN_SIZE_X * 2];
    snprintf(text, sizeof(text), "%s   %lu of %lu KB", downloadRomName,
             (unsigned long)(done / BYTES_PER_KB),
             (unsigned long)downloadRomSizeKb);
    ui_bar(MENU_ROW_TRANSFER, MENU_COL_KEY, MENU_ROW_WIDTH, done,
           downloadRomSizeKb * BYTES_PER_KB, text);
    return;
  }
  ui_printField(MENU_ROW_TRANSFER, MENU_COL_KEY, MENU_ROW_WIDTH,
                downloadMessage);
}

// The values that follow the state: the selection, the card, the network,
// the last download, and the icons. Drawn in place.
static void menuDrawStatus(void) {
  SettingsConfigEntry *romSelected =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED);
  bool picked = (romSelected != NULL) && (romSelected->value[0] != '\0');
  ui_printField(MENU_ROW_LAUNCH, MENU_COL_VALUE, MENU_VALUE_WIDTH,
                picked ? romSelected->value : "pick one: [B] or [D]");
  ui_printField(MENU_ROW_RIPPER, MENU_COL_VALUE, MENU_VALUE_WIDTH,
                delayMode ? "on: waits for SELECT" : "off");
  ui_printField(MENU_ROW_BROWSE, MENU_COL_VALUE, MENU_VALUE_WIDTH, romsFolder);
  ui_printField(MENU_ROW_CARD_STATE, MENU_COL_VALUE, MENU_VALUE_WIDTH,
                sdcard_isMounted() ? "mounted" : "none: put one in");
  const char *wifi = netReason;
  if (netState == NET_CONNECTING) {
    wifi = "joining...";
  } else if (netState == NET_UP) {
    wifi = (catalogRefresh == CATALOG_REFRESH_RUNNING) ? "connected, refreshing"
                                                       : "connected";
  }
  ui_printField(MENU_ROW_WIFI, MENU_COL_VALUE, MENU_VALUE_WIDTH, wifi);
  menuDrawTransfer();
  ui_titleIcons(titleSdGlyph(), titleWifiGlyph());
  ui_groupIcon(MENU_ROW_CARD,
               sdcard_isMounted() ? UI_GLYPH_DRIVE : UI_GLYPH_NONE);
  ui_groupIcon(MENU_ROW_CATALOG, titleWifiGlyph());
}

static void menu(void) {
  menuState.menuLevel = TERM_ROMS_MENU_MAIN;
  term_setCommandLevel(TERM_COMMAND_LEVEL_SINGLE_KEY);
  menuNoticeText[0] = '\0';
  showTitle();
  // The words: printed in place, so the frames drawn after them stay whole
  term_printAt(MENU_ROW_ROM, MENU_COL_LABEL, MENU_LABEL_ROM);
  term_printAt(MENU_ROW_LAUNCH, MENU_COL_KEY, "[L]aunch");
  term_printAt(MENU_ROW_RIPPER, MENU_COL_KEY, "[R]ipper");
  term_printAt(MENU_ROW_CARD, MENU_COL_LABEL, MENU_LABEL_CARD);
  term_printAt(MENU_ROW_BROWSE, MENU_COL_KEY, "[B]rowse");
  term_printAt(MENU_ROW_CARD_STATE, MENU_COL_KEY, "Card:");
  term_printAt(MENU_ROW_CATALOG, MENU_COL_LABEL, MENU_LABEL_CATALOG);
  term_printAt(MENU_ROW_DOWNLOAD, MENU_COL_KEY, "[D]ownload");
  term_printAt(MENU_ROW_WIFI, MENU_COL_KEY, "Wi-Fi:");
  term_printAt(MENU_ROW_DEVICE, MENU_COL_LABEL, MENU_LABEL_DEVICE);
  term_printAt(MENU_ROW_SETTINGS, MENU_COL_KEY, "[S]ettings");
  term_printAt(MENU_ROW_EXIT, MENU_COL_KEY,
               "[E]xit to desktop (or hold SHIFT)");
  term_printAt(MENU_ROW_BOOSTER, MENU_COL_KEY,
               "[X] Back to Booster   [M] Refresh");
  const char *url = catalogUrl();
  const char *host = (url != NULL) ? strstr(url, "://") : NULL;
  host = (host != NULL) ? host + 3 : "not set";
  char hostName[MENU_VALUE_WIDTH + 1];
  snprintf(hostName, sizeof(hostName), "%.*s", (int)strcspn(host, "/:"), host);
  ui_printField(MENU_ROW_DOWNLOAD, MENU_COL_VALUE, MENU_VALUE_WIDTH, hostName);
  menuDrawStatus();
  menuPrompt();
  // Then the frames and the strip
  ui_group(MENU_ROW_ROM, MENU_ROW_ROM_END, MENU_COL_LABEL,
           LABEL_COLS(MENU_LABEL_ROM), UI_GLYPH_CARTRIDGE);
  ui_group(MENU_ROW_CARD, MENU_ROW_CARD_END, MENU_COL_LABEL,
           LABEL_COLS(MENU_LABEL_CARD),
           sdcard_isMounted() ? UI_GLYPH_DRIVE : UI_GLYPH_NONE);
  ui_group(MENU_ROW_CATALOG, MENU_ROW_CATALOG_END, MENU_COL_LABEL,
           LABEL_COLS(MENU_LABEL_CATALOG), titleWifiGlyph());
  ui_group(MENU_ROW_DEVICE, MENU_ROW_DEVICE_END, MENU_COL_LABEL,
           LABEL_COLS(MENU_LABEL_DEVICE), UI_GLYPH_COG);
  menuDrawNotice();
  menuIntact = true;
}

// A command's outcome, on the menu's notice rows; the menu is drawn again if
// something else is on the screen. The cursor goes back to the prompt.
static void menuNotice(const char *text) {
  if (!menuIntact || listKind != LIST_NONE ||
      menuState.menuLevel != TERM_ROMS_MENU_MAIN) {
    listClose();
    menu();
  }
  snprintf(menuNoticeText, sizeof(menuNoticeText), "%s", text);
  menuDrawNotice();
  menuPrompt();
  display_refresh();
}

// Command handlers
// A screen of a few lines under the title bar, when the menu gives way to
// something (Booster, the desktop) or shows the keys. Lines fit in
// MESSAGE_WIDTH.
#define MESSAGE_ROW 2
#define MESSAGE_COL 2
#define MESSAGE_WIDTH (TERM_SCREEN_SIZE_X - (2 * MESSAGE_COL))

static void messageScreen(const char *const lines[], size_t count) {
  showTitle();
  for (size_t i = 0; i < count; i++) {
    ui_printField((uint8_t)(MESSAGE_ROW + i), MESSAGE_COL, MESSAGE_WIDTH,
                  lines[i]);
  }
  ui_parkCursor();
  display_refresh();
}

void cmdMenu(const char *arg) {
  menu();
  display_refresh();
}

// The settings take typed lines (put_str KEY VALUE, save, ...) until 'm'.
static void cmdSettings(const char *arg) {
  menuIntact = false;
  menuState.menuLevel = TERM_ROMS_MENU_SETTINGS;
  term_setCommandLevel(TERM_COMMAND_LEVEL_COMMAND_INPUT);
  term_cmdSettings(arg);
  term_printString("> ");
}

void cmdHelp(const char *arg) {
  static const char *const lines[] = {
      "Available commands:",
      "",
      "B  Browse the ROMs on the SD card",
      "D  Download ROMs from the catalog",
      "L  Launch the selected ROM",
      "R  Delay/ripper mode on or off",
      "S  Settings (typed commands)",
      "E  Exit to the desktop",
      "X  Back to Booster",
      "M  The menu",
      "",
      "Press M for the menu.",
  };
  messageScreen(lines, sizeof(lines) / sizeof(lines[0]));
}
void cmdClear(const char *arg) {
  menuIntact = false;
  term_clearScreen();
}

void cmdExit(const char *arg) {
  static const char *const lines[] = {"Booting to the desktop..."};
  messageScreen(lines, sizeof(lines) / sizeof(lines[0]));
  // Send continue to desktop command
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_CONTINUE);
}
// --- The network and the catalog refresh, behind the menu ------------------

// The menu shows the network and the last download on its status lines:
// redraw it when they change, unless a list or another screen is up.
static void menuStatusChanged(void) {
  if (listKind != LIST_NONE) {
    ui_titleIcons(titleSdGlyph(), titleWifiGlyph());
    display_refresh();
  } else if (menuIntact && menuState.menuLevel == TERM_ROMS_MENU_MAIN) {
    menuDrawStatus();
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
// the AP, and netPoll() notices as it would a real loss), or the next try at
// once instead of after the backoff.
static void netTestWifi(bool connect) {
  if (connect) {
    netBackoffMs = NET_REJOIN_BACKOFF_MIN_MS;
    netNextTryAt = get_absolute_time();
  } else {
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
  }
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
  netRetry = (result != NETWORK_WIFI_STA_CONN_ERR_NO_SSID);
  netReason =
      netRetry ? "Did not connect; trying again." : "Wi-Fi is not configured.";
}

// The next try after a failure, later each time up to the maximum.
static void netScheduleRetry(void) {
  netRetry = true;
  netNextTryAt = make_timeout_time_ms(netBackoffMs);
  netBackoffMs *= 2U;
  if (netBackoffMs > NET_REJOIN_BACKOFF_MAX_MS) {
    netBackoffMs = NET_REJOIN_BACKOFF_MAX_MS;
  }
}

#if APP_DOWNLOAD_HTTPS
// mbedTLS allocates in the window's ROM3 bank, which setup mode doesn't serve
// (the template's program serves ROM4 only); ROM mode copies the ROM over it
// and never uses TLS. A TLS session's buffers (about 40 KB) stay off the heap.
// lwIP points mbedTLS at its own 4 KB heap when it creates its TLS config, so
// the arena goes in once that config exists, while nothing is allocated yet.
static void tlsArenaStart(void) {
  if (httpc_shared_tls_config() == NULL) {
    DPRINTF("No TLS config: https downloads will fail\n");
    return;
  }
  mbedtls_memory_buffer_alloc_init(
      (unsigned char *)&__rom_in_ram_start__ + ROM_SIZE_BYTES, ROM_SIZE_BYTES);
}
#endif

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
    netRetry = false;
    netReason = "The Wi-Fi chip did not start.";
  } else {
#if APP_DOWNLOAD_HTTPS
    tlsArenaStart();
#endif
    netConnect();
    if (netState == NET_FAILED && netRetry) {
      netScheduleRetry();
    }
  }
  menuStatusChanged();
}

// Connected: is the link still there? The link status catches an ordinary
// disconnect, the gateway probe a silent one.
static void netWatchLink(void) {
  if (network_isLinkHealthy() && !network_pollGatewayProbe()) {
    return;
  }
  DPRINTF("Wi-Fi: the link is down; joining again in %u ms\n",
          (unsigned)NET_LINK_DOWN_GRACE_MS);
  netLinkLosses++;
  netState = NET_FAILED;
  netReason = "Wi-Fi lost; joining again.";
  netRetry = true;
  netBackoffMs = NET_REJOIN_BACKOFF_MIN_MS;
  netNextTryAt = make_timeout_time_ms(NET_LINK_DOWN_GRACE_MS);
  menuStatusChanged();
}

// Not connected: the next try, when it is due and no download runs.
static void netRetryIfDue(void) {
  if (!netRetry || downloadKind != DOWNLOAD_KIND_NONE ||
      absolute_time_diff_us(get_absolute_time(), netNextTryAt) > 0) {
    return;
  }
  netRejoins++;
  DPRINTF("Wi-Fi: try %lu\n", (unsigned long)netRejoins);
  // One attempt per try: the backoff, not the attempts, spaces them.
  netAttempts = NET_CONNECT_ATTEMPTS - 1;
  health_feed();
  netConnect();
  health_feed();
  if (netState == NET_FAILED && netRetry) {
    netScheduleRetry();
  }
  menuStatusChanged();
}

// From the main loop: the connection under way (three attempts at boot, one
// per later try), the link while connected, the next try while not.
static void netPoll(void) {
  if (netState == NET_UP) {
    netWatchLink();
    return;
  }
  if (netState == NET_FAILED) {
    netRetryIfDue();
    return;
  }
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
    netBackoffMs = NET_REJOIN_BACKOFF_MIN_MS;
    network_resetGatewayProbe();
    catalogRefreshStart();
  } else if (result == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT &&
             netAttempts < NET_CONNECT_ATTEMPTS) {
    netConnect();
    if (netState == NET_CONNECTING) {
      return;
    }
  } else {
    netState = NET_FAILED;
    netReason = "Did not connect; trying again.";
    netScheduleRetry();
  }
  menuStatusChanged();
}

static void downloadProgress(void);
static void downloadRomEnded(void);

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
    downloadProgress();
    return;
  }
  if (status != DOWNLOAD_STATUS_COMPLETED && status != DOWNLOAD_STATUS_FAILED) {
    return;
  }
  download_err_t err = download_finish();
#if APP_DOWNLOAD_HTTPS && defined(_DEBUG) && (_DEBUG != 0)
  size_t tlsPeakBytes = 0;
  size_t tlsPeakBlocks = 0;
  mbedtls_memory_buffer_alloc_max_get(&tlsPeakBytes, &tlsPeakBlocks);
  DPRINTF("TLS arena: peak %u bytes in %u blocks\n", (unsigned)tlsPeakBytes,
          (unsigned)tlsPeakBlocks);
  mbedtls_memory_buffer_alloc_max_reset();
#endif
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
        downloadRomEnded();
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
  if (kind == DOWNLOAD_KIND_ROM) {
    downloadRomEnded();
  } else {
    menuStatusChanged();
  }
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

// Why the catalog may be old, from row: the rows it took, with a blank one.
static uint8_t catalogNotice(uint8_t row) {
  char text[NOTICE_TEXT_BYTES];
  if (netState == NET_CONNECTING || catalogRefresh == CATALOG_REFRESH_RUNNING) {
    snprintf(text, sizeof(text), "Refreshing the catalog...");
  } else if (netState != NET_UP) {
    snprintf(text, sizeof(text), "Offline: the copy on the SD card. %s",
             netReason);
  } else if (catalogRefresh == CATALOG_REFRESH_FAILED) {
    snprintf(text, sizeof(text), "Offline: the copy on the SD card. Server: %s",
             catalogReason);
  } else {
    return 0;
  }
  noticeDraw(row, text);
  return NOTICE_ROWS + 1;
}
// The lists' rows
#define LIST_ROW_HEADER 2
#define LIST_ROW_RULE 3
#define LIST_ROW_FIRST 4
#define LIST_NAME_WIDTH (TERM_SCREEN_SIZE_X - 3)

// The page's first entry's row: after the catalog's notice, if any
uint8_t listFirstRow = LIST_ROW_FIRST;

// An entry's row, inverted when it is the selection (the tools read the
// selection and the first row over SWD: listNav, listFirstRow).
static void listDrawEntry(uint32_t index) {
  uint8_t row = (uint8_t)(listFirstRow + (index - navlist_first(&listNav)));
  bool selected = (index == listNav.selected);
  char line[TERM_SCREEN_SIZE_X + 1];
  snprintf(line, sizeof(line), "  %.*s", LIST_NAME_WIDTH, listName(index));
  ui_printField(row, 0, TERM_SCREEN_SIZE_X, line);
  if (selected) {
    ui_invertRows(row, 1);
  }
}

static void listDraw(void) {
  showTitle();
  char page[TERM_SCREEN_SIZE_X];
  snprintf(page, sizeof(page), "Page %lu/%lu, %lu ROMs",
           (unsigned long)(navlist_page(&listNav) + 1),
           (unsigned long)navlist_pages(&listNav),
           (unsigned long)listNav.count);
  const char *name =
      (listKind == LIST_CARD) ? "ROMs on the SD card" : "ROM catalog";
  char head[TERM_SCREEN_SIZE_X + 1];
  snprintf(head, sizeof(head), " %-*s%s",
           (int)(TERM_SCREEN_SIZE_X - 2 - strlen(page)), name, page);
  term_printAt(LIST_ROW_HEADER, 0, head);
  listFirstRow = LIST_ROW_FIRST;
  if (listKind == LIST_CATALOG) {
    listFirstRow += catalogNotice(LIST_ROW_FIRST);
  }
  uint32_t first = navlist_first(&listNav);
  uint32_t shown = navlist_onPage(&listNav);
  for (uint32_t i = 0; i < shown; i++) {
    listDrawEntry(first + i);
  }
  ui_parkCursor();
  ui_rule(LIST_ROW_RULE);
  ui_strip("UP/DOWN select    LEFT/RIGHT page    RETURN details    ESC menu",
           false);
  display_refresh();
}
static bool catalogEntryAllowed(char *why, size_t whySize) {
  switch (catalog_check(&catalogEntry)) {
    case CATALOG_ENTRY_TOO_LARGE:
      snprintf(why, whySize, "Too large: %lu KB, the limit is 128 KB.",
               (unsigned long)catalogEntry.sizeKb);
      return false;
    case CATALOG_ENTRY_BAD_NAME:
      snprintf(why, whySize, "Refused: its file name is not safe.");
      return false;
    case CATALOG_ENTRY_LONG_NAME:
      snprintf(why, whySize, "Refused: its file name is too long.");
      return false;
    default:
      break;
  }
  if (netState != NET_UP) {
    snprintf(why, whySize, "The network is needed to download it: %s",
             netReason);
    return false;
  }
  if (!catalogOrigin.known) {
    snprintf(why, whySize,
             "The catalog has not been refreshed from its server yet: try "
             "again soon.");
    return false;
  }
  if (downloadKind != DOWNLOAD_KIND_NONE) {
    snprintf(why, whySize, "A download is running: wait for it.");
    return false;
  }
  why[0] = '\0';
  return true;
}
// The details' rows: a box with the ROM's name on its top edge, its fields,
// and a refusal under it
#define DETAILS_ROW_BOX 4
#define DETAILS_COL_LABEL 2
#define DETAILS_COL_VALUE 12
#define DETAILS_VALUE_WIDTH (TERM_SCREEN_SIZE_X - DETAILS_COL_VALUE - 2)
#define DETAILS_NAME_WIDTH (TERM_SCREEN_SIZE_X - 10)
#define DETAILS_ABOUT_ROWS 6

// Where the details' refusal goes, under the box
static uint8_t detailsNoticeRow = DETAILS_ROW_BOX + 2;

// A refusal on the details screen, after it was drawn
static void detailsNotice(const char *text) {
  noticeDraw(detailsNoticeRow, text);
  ui_parkCursor();
  display_refresh();
}

static void detailsField(uint8_t row, const char *label, const char *value) {
  term_printAt(row, DETAILS_COL_LABEL, label);
  ui_printField(row, DETAILS_COL_VALUE, DETAILS_VALUE_WIDTH, value);
}

// value split at spaces over rows from row, at most maxRows: the rows taken
static uint8_t detailsWrapped(uint8_t row, const char *label, const char *value,
                              uint8_t maxRows) {
  term_printAt(row, DETAILS_COL_LABEL, label);
  uint8_t used = 0;
  while (*value != '\0' && used < maxRows) {
    int len = (int)strlen(value);
    int take = len;
    if (len > DETAILS_VALUE_WIDTH) {
      take = DETAILS_VALUE_WIDTH;
      while (take > 0 && value[take] != ' ') {
        take--;
      }
      if (take == 0) {
        take = DETAILS_VALUE_WIDTH;
      }
    }
    char part[TERM_SCREEN_SIZE_X + 1];
    snprintf(part, sizeof(part), "%.*s", take, value);
    term_printAt((uint8_t)(row + used), DETAILS_COL_VALUE, part);
    value += take;
    while (*value == ' ') {
      value++;
    }
    used++;
  }
  return (used > 0) ? used : 1;
}

static void detailsBox(uint8_t bottomRow, const char *name) {
  char label[DETAILS_NAME_WIDTH + 1];
  snprintf(label, sizeof(label), "%s", name);
  term_printAt(DETAILS_ROW_BOX, DETAILS_COL_LABEL, label);
  ui_parkCursor();
  ui_rule(LIST_ROW_RULE);
  ui_group(DETAILS_ROW_BOX, bottomRow, DETAILS_COL_LABEL,
           (uint8_t)strlen(label), UI_GLYPH_CARTRIDGE);
}

static void listDetailsDraw(void) {
  showTitle();
  term_printAt(
      LIST_ROW_HEADER, 0,
      (listKind == LIST_CARD) ? " ROMs on the SD card" : " ROM catalog");
  if (listKind == LIST_CARD) {
    char name[SETTINGS_MAX_VALUE_LENGTH];
    romFullName(sdRoms[listNav.selected].open, name, sizeof(name));
    detailsField(DETAILS_ROW_BOX + 1, "File", name);
    detailsField(DETAILS_ROW_BOX + 2, "Folder", romsFolder);
    detailsBox(DETAILS_ROW_BOX + 3, name);
    ui_strip("RETURN select    ESC back", false);
    display_refresh();
    return;
  }
  if (catalog_readEntry(&catalog, listNav.selected, &catalogEntry) !=
      CATALOG_OK) {
    noticeDraw(DETAILS_ROW_BOX, "The catalog could not be read.");
    ui_parkCursor();
    ui_rule(LIST_ROW_RULE);
    ui_strip("ESC back", false);
    display_refresh();
    return;
  }
  char size[NUMBER_TEXT_BYTES];
  snprintf(size, sizeof(size), "%lu KB", (unsigned long)catalogEntry.sizeKb);
  detailsField(DETAILS_ROW_BOX + 1, "File", catalog_fileName(&catalogEntry));
  detailsField(DETAILS_ROW_BOX + 2, "Size", size);
  detailsField(DETAILS_ROW_BOX + 3, "Tags", catalogEntry.tags);
  uint8_t rows = detailsWrapped(DETAILS_ROW_BOX + 4, "About",
                                catalogEntry.description, DETAILS_ABOUT_ROWS);
  uint8_t bottom = (uint8_t)(DETAILS_ROW_BOX + 4 + rows);
  char why[NOTICE_TEXT_BYTES];
  bool allowed = catalogEntryAllowed(why, sizeof(why));
  detailsNoticeRow = (uint8_t)(bottom + 2);
  noticeDraw(detailsNoticeRow, why);
  detailsBox(bottom, catalogEntry.name);
  ui_strip(allowed ? "RETURN download    ESC back" : "ESC back", false);
  display_refresh();
}
// --- The download screen -----------------------------------------------------
// A ROM download in front, with its progress; ESC leaves it running behind
// the menu, whose catalog box shows its progress too.
#define DOWNLOAD_ROW_BOX 2
#define DOWNLOAD_ROW_ROM 3
#define DOWNLOAD_ROW_FROM 4
#define DOWNLOAD_ROW_BAR 6
#define DOWNLOAD_ROW_BOX_END 7
#define DOWNLOAD_COL 2
#define DOWNLOAD_LABEL "Download"
#define DOWNLOAD_COL_VALUE 12
#define DOWNLOAD_VALUE_WIDTH (TERM_SCREEN_SIZE_X - DOWNLOAD_COL_VALUE - 2)
#define DOWNLOAD_BAR_COLS (TERM_SCREEN_SIZE_X - (2 * DOWNLOAD_COL))
// How often the progress is drawn again
#define DOWNLOAD_PROGRESS_MS 250

static bool downloadScreenShown = false;
static uint32_t downloadProgressUs = 0;

static void downloadDrawBar(uint8_t row, uint8_t col, uint8_t cols) {
  uint32_t done = download_getBytesWritten();
  char text[TERM_SCREEN_SIZE_X * 2];
  snprintf(text, sizeof(text), "%lu of %lu KB",
           (unsigned long)(done / BYTES_PER_KB),
           (unsigned long)downloadRomSizeKb);
  ui_bar(row, col, cols, done, downloadRomSizeKb * BYTES_PER_KB, text);
}

static void downloadScreenKey(char key) {
  if (key == TERM_KEY_ESC || key == 'm' || key == 'M') {
    downloadScreenShown = false;
    term_setKeyHandler(NULL);
    menu();
    display_refresh();
  }
}

static void downloadScreen(void) {
  menuIntact = false;
  menuState.menuLevel = TERM_ROMS_MENU_BROWSE_NETWORK;
  showTitle();
  term_printAt(DOWNLOAD_ROW_BOX, DOWNLOAD_COL, DOWNLOAD_LABEL);
  term_printAt(DOWNLOAD_ROW_ROM, DOWNLOAD_COL, "ROM");
  ui_printField(DOWNLOAD_ROW_ROM, DOWNLOAD_COL_VALUE, DOWNLOAD_VALUE_WIDTH,
                downloadRomName);
  term_printAt(DOWNLOAD_ROW_FROM, DOWNLOAD_COL, "From");
  ui_printField(DOWNLOAD_ROW_FROM, DOWNLOAD_COL_VALUE, DOWNLOAD_VALUE_WIDTH,
                catalogOrigin.host);
  ui_parkCursor();
  ui_group(DOWNLOAD_ROW_BOX, DOWNLOAD_ROW_BOX_END, DOWNLOAD_COL,
           LABEL_COLS(DOWNLOAD_LABEL), UI_GLYPH_WIFI);
  downloadDrawBar(DOWNLOAD_ROW_BAR, DOWNLOAD_COL, DOWNLOAD_BAR_COLS);
  ui_strip("ESC menu: the download goes on", false);
  downloadScreenShown = true;
  downloadProgressUs = time_us_32();
  term_setKeyHandler(downloadScreenKey);
  display_refresh();
}

// A ROM download's end: from its screen, the menu with how it went
static void downloadRomEnded(void) {
  if (!downloadScreenShown) {
    menuStatusChanged();
    return;
  }
  downloadScreenShown = false;
  term_setKeyHandler(NULL);
  menuIntact = false;
  if (strncmp(downloadMessage, DOWNLOADED_PREFIX,
              sizeof(DOWNLOADED_PREFIX) - 1) == 0) {
    char text[NOTICE_TEXT_BYTES];
    snprintf(text, sizeof(text), "Downloaded and selected: %s",
             downloadMessage + sizeof(DOWNLOADED_PREFIX) - 1);
    menuNotice(text);
  } else {
    menuNotice(downloadMessage);
  }
}

// The progress, every DOWNLOAD_PROGRESS_MS, where it is shown
static void downloadProgress(void) {
  if (downloadKind != DOWNLOAD_KIND_ROM ||
      time_us_32() - downloadProgressUs < DOWNLOAD_PROGRESS_MS * US_PER_MS) {
    return;
  }
  downloadProgressUs = time_us_32();
  if (downloadScreenShown) {
    downloadDrawBar(DOWNLOAD_ROW_BAR, DOWNLOAD_COL, DOWNLOAD_BAR_COLS);
    display_refresh();
  } else if (menuIntact && listKind == LIST_NONE &&
             menuState.menuLevel == TERM_ROMS_MENU_MAIN) {
    menuDrawTransfer();
    display_refresh();
  }
}

static void listAct(void) {
  if (listKind == LIST_CARD) {
    char name[SETTINGS_MAX_VALUE_LENGTH];
    romFullName(sdRoms[listNav.selected].open, name, sizeof(name));
    // The pick is held in the settings in RAM and reaches flash with MODE,
    // after the launch has written and read back the ROM.
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED, name);
    listClose();
    char text[NOTICE_TEXT_BYTES];
    snprintf(text, sizeof(text), "Selected: %s", name);
    menuNotice(text);
    return;
  }
  char why[NOTICE_TEXT_BYTES];
  if (!catalogEntryAllowed(why, sizeof(why))) {
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
    detailsNotice("Refused: its URL is too long.");
    return;
  }
  DPRINTF("Downloading ROM: %s\n", url);
  download_setFilepath(url);
  download_err_t err = download_start();
  if (err != DOWNLOAD_OK) {
    char reason[TERM_SCREEN_SIZE_X];
    describeDownload(err, reason, sizeof(reason));
    char text[NOTICE_TEXT_BYTES];
    snprintf(text, sizeof(text), "The download did not start: %s", reason);
    detailsNotice(text);
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
  downloadScreen();
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
  uint32_t before = listNav.selected;
  switch (navlist_key(&listNav, key)) {
    case NAVLIST_MOVED:
      // Two rows, not the page
      listDrawEntry(before);
      listDrawEntry(listNav.selected);
      display_refresh();
      break;
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
  menuNotice("No SD card. Put one in: it is found by itself.");
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
    menuNotice("Not enough memory for the ROM list.");
    return;
  }
  if (listed != FR_OK) {
    listClose();
    if (!sdcard_checkPresence()) {
      cardMissing();
      return;
    }
    char text[NOTICE_TEXT_BYTES];
    snprintf(text, sizeof(text),
             "The folder %s could not be read: check FOLDER in [S]ettings.",
             romsFolder);
    menuNotice(text);
    return;
  }
  if (sdRomsCount == 0) {
    listClose();
    char text[NOTICE_TEXT_BYTES];
    snprintf(text, sizeof(text),
             "No ROMs in %s: [D]ownload some, or copy them there.", romsFolder);
    menuNotice(text);
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
    char text[NOTICE_TEXT_BYTES];
    if (netState == NET_CONNECTING ||
        catalogRefresh == CATALOG_REFRESH_RUNNING) {
      snprintf(text, sizeof(text),
               "The catalog is still downloading: try [D] again in a moment.");
    } else if (opened == CATALOG_NO_MEMORY) {
      snprintf(text, sizeof(text), "Not enough memory for the catalog.");
    } else if (netState != NET_UP) {
      snprintf(text, sizeof(text),
               "No catalog on the SD card, and no "
               "network: %s",
               netReason);
    } else {
      snprintf(text, sizeof(text), "No catalog on the SD card: %s",
               catalogReason[0] ? catalogReason : "empty");
    }
    menuNotice(text);
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
    menuNotice(romstore_message(ROMSTORE_NOT_SELECTED));
    return;
  }
  if (cardMissing()) {
    return;
  }
  // The launch's screen stays until [M]: a status redraw (the card, the
  // network) would wipe a failure before it was read.
  menuIntact = false;
  menuState.menuLevel = TERM_ROMS_MENU_LAUNCH;
  char filename[MAX_PATH_SIZE];
  snprintf(filename, sizeof(filename), "%s/%s", romsFolder, romFile->value);
  DPRINTF("Loading ROM file into FLASH: %s\n", filename);
  launchScreen("Launch", romFile->value,
               delayMode ? "delay: waits for SELECT" : "direct");
  romstore_info_t info;
  romFlashBegin();
  romstore_result_t result = romstore_launch(filename, FLASH_ROM_LOAD_OFFSET,
                                             &romFlash, &info, cmdLaunchSelect);
  romFlashEnd();
  launchScreenShown = false;
  if (result != ROMSTORE_OK) {
    DPRINTF("Launch failed: %s\n", romstore_message(result));
    char why[NOTICE_TEXT_BYTES];
    if ((result == ROMSTORE_READ_ERROR || result == ROMSTORE_NOT_FOUND) &&
        !sdcard_checkPresence()) {
      snprintf(why, sizeof(why),
               "The ROM could not be read: the SD card is "
               "gone.");
    } else {
      romstoreDescribe(result, &info, why, sizeof(why));
    }
    ui_bar(LAUNCH_ROW_BAR, LAUNCH_COL, LAUNCH_BAR_COLS, 0, 1, "Not written");
    noticeDraw(LAUNCH_ROW_NOTICE, why);
    term_printAt(LAUNCH_ROW_NOTICE + NOTICE_ROWS + 1, LAUNCH_COL,
                 "Press M for the menu.");
    display_refresh();
    return;
  }
  ui_bar(LAUNCH_ROW_BAR, LAUNCH_COL, LAUNCH_BAR_COLS, 1, 1,
         "Written to flash and read back");
  uint8_t row = LAUNCH_ROW_TEXT;
  term_printAt(row++, LAUNCH_COL, "The ROM will boot shortly.");
  if (delayMode) {
    term_printAt(row++, LAUNCH_COL, "Delay mode: SELECT starts the ROM.");
  }
  row++;
  term_printAt(row++, LAUNCH_COL, "SELECT brings this menu back.");
  term_printAt(row, LAUNCH_COL, "If it doesn't boot, reset the ST.");
  display_refresh();
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
  static const char *const lines[] = {
      "Launching Booster...",
      "",
      "The computer will boot shortly.",
      "If it doesn't, turn it off and on.",
  };
  menuState.menuLevel = TERM_ROMS_MENU_BOOSTER;
  messageScreen(lines, sizeof(lines) / sizeof(lines[0]));
  resetDeviceAtBoot = false;  // Jump to the booster app
  keepActive = false;         // Exit the active loop
}
#if defined(_DEBUG) && (_DEBUG != 0)
static void cmdFirmware(const char *arg) {
  menuIntact = false;
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
    menuNoticePoll();
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
