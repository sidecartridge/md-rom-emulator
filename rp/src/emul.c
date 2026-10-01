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
#include "memfunc.h"
#include "network.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
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

// The largest file ROM_TEMP takes: two 64 KB banks, plus the 4 zero bytes a
// STEEM cartridge image starts with.
#define ROM_FILE_MAX_BYTES (ROM_SIZE_BYTES * ROM_BANKS)
#define ROM_FILE_STEEM_HEADER_BYTES 4

// Command handlers
static void cmdMenu(const char *arg);
static void cmdNext(const char *arg);
static void cmdPrev(const char *arg);
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
#endif

// Command table
static const Command commands[] = {
    {"m", cmdMenu},
    {"n", cmdNext},
    {"p", cmdPrev},
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

// Global array to store ROM info.
static ROM *roms = NULL;
static int romsCount = 0;

// ROMs folder. Initialize with the default value.
static char romsFolder[MAX_PATH_SIZE] = "/roms";

// Pagination info
static int currentRomPage = 0;
static int maxRomPages = 0;
static int downloadRomSelected = -1;

// Menu status
static MenuState menuState = {0, 0};

// Keep active loop or exit
static bool keepActive = true;

// Should we reset the device, or jump to the booster app?
// By default, we reset the device.
static bool resetDeviceAtBoot = true;

// Do we have network or not?
static bool hasNetwork = false;
static bool wifiConnected = false;
static bool catalogAvailable = false;

// Delay/ripper mode?
static bool delayMode = false;
// A ROM download from the catalog is running: romDownloadPoll() finishes it.
static bool romDownloadActive = false;

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
      uint32_t kb = (payloadSize >= 2u) ? payload[0] : 0u;
      if (kb == 0u) {
        while (devhooksHeldHeap != NULL) {
          DevhooksHeldBlock *next = devhooksHeldHeap->next;
          free(devhooksHeldHeap);
          devhooksHeldHeap = next;
        }
        DPRINTF("devhooks: heap hold released\n");
        return 1;
      }
      DevhooksHeldBlock *block = malloc(sizeof(DevhooksHeldBlock) + kb * 1024u);
      if (block != NULL) {
        block->next = devhooksHeldHeap;
        devhooksHeldHeap = block;
      }
      DPRINTF("devhooks: holding %lu KB more heap: %s\n", (unsigned long)kb,
              (block != NULL) ? "ok" : "refused");
      return (block != NULL) ? 1u : 0u;
    }
    case DEVHOOKS_APP_DOWNLOAD:
      return devdownload_start();
    default:
      return 0;
  }
}
#endif

// Keep answering the ST for ms milliseconds, so a command in flight is not
// left without its answer while a sentinel command waits to be seen.
static void emul_serviceFor(uint32_t ms) {
  absolute_time_t until = make_timeout_time_ms(ms);
  while (absolute_time_diff_us(get_absolute_time(), until) > 0) {
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
    chandler_loop();
    int32_t offset = romemul_lastReadOffset();
    if (offset >= 0 && offset < 0x100) {
      return true;
    }
  }
  return false;
}

// What every long wait runs, so the ST's commands are answered and SELECT is
// seen meanwhile.
static void __not_in_flash_func(emul_pollTick)(void) {
  chandler_loop();
  term_loop();
  select_poll();
}

static FRESULT storeFileToFlash(const char *filename, uint32_t flashAddress) {
  FIL file;
  FRESULT res;
  UINT bytesRead;
  FSIZE_t size;

  uint8_t *buffer = (uint8_t *)malloc(FLASH_SECTOR_SIZE);
  if (buffer == NULL) {
    DPRINTF("Error allocating memory for buffer\n");
    return FR_NOT_ENOUGH_CORE;
  }

  // Open the file (read-only, binary mode)
  res = f_open(&file, filename, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Error opening file %s: %d\n", filename, res);
    free(buffer);
    return res;
  }

  // Get file size (use FSIZE_t for portability)
  size = f_size(&file);
  DPRINTF("File size: %u bytes\n", (unsigned int)size);

  // ROM_TEMP holds two banks, and Booster's flash starts right after it: a
  // larger file is refused before anything is erased.
  if (size > ROM_FILE_MAX_BYTES + ROM_FILE_STEEM_HEADER_BYTES) {
    DPRINTF("File too large: %u bytes, the limit is %u\n", (unsigned int)size,
            (unsigned int)ROM_FILE_MAX_BYTES);
    f_close(&file);
    free(buffer);
    return FR_INVALID_PARAMETER;
  }

  // If the file size is a multiple of FLASH_SECTOR_SIZE plus 4 bytes, check for
  // 4-byte padding.
  if (size > 4 && ((size - 4) % FLASH_SECTOR_SIZE == 0)) {
    // Read the first 4 bytes
    res = f_read(&file, buffer, 4, &bytesRead);
    if (res != FR_OK || bytesRead != 4) {
      DPRINTF("Error reading header of file: %d (bytes read: %u)\n", res,
              bytesRead);
      f_close(&file);
      free(buffer);
      return res;
    }

    // Check if the first 4 bytes are 0x00000000
    if (buffer[0] == 0x00 && buffer[1] == 0x00 && buffer[2] == 0x00 &&
        buffer[3] == 0x00) {
      DPRINTF("Skipping first 4 bytes. Looks like a STEEM cartridge image.\n");
    } else {
      // Rollback the file pointer by 4 bytes.
      res = f_lseek(&file, f_tell(&file) - 4);
      if (res != FR_OK) {
        DPRINTF("Error seeking back in file: %d\n", res);
        f_close(&file);
        free(buffer);
        return res;
      }
    }
  }
  FSIZE_t romBytes = size - f_tell(&file);
  if (romBytes > ROM_FILE_MAX_BYTES) {
    DPRINTF("File too large: %u bytes after its header, the limit is %u\n",
            (unsigned int)romBytes, (unsigned int)ROM_FILE_MAX_BYTES);
    f_close(&file);
    free(buffer);
    return FR_INVALID_PARAMETER;
  }

  // Calculate the flash programming offset relative to XIP_BASE.
  uint32_t offset = flashAddress - XIP_BASE;

  // Read and program the file in FLASH_SECTOR_SIZE chunks.
  while (1) {
    // The ST keeps its answers and SELECT is seen between chunks.
    emul_pollTick();

    // Read a chunk of data from the file.
    DPRINTF("Reading %u bytes from file at offset 0x%X\n", FLASH_SECTOR_SIZE,
            offset);
    res = f_read(&file, buffer, FLASH_SECTOR_SIZE, &bytesRead);
    if (res != FR_OK) {
      DPRINTF("Error reading file: %d\n", res);
      f_close(&file);
      free(buffer);
      return res;
    }
    if (bytesRead == 0) {
      // End of file reached.
      break;
    }

    // Pad the data to FLASH_PAGE_SIZE alignment if needed.
    size_t programSize = bytesRead;
    if (programSize % FLASH_PAGE_SIZE != 0) {
      size_t paddedSize =
          ((programSize + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) *
          FLASH_PAGE_SIZE;
      memset(buffer + programSize, FLASH_PAGE_SIZE, paddedSize - programSize);
      programSize = paddedSize;
    }

    // Transform buffer's words from little endian to big endian inline
    CHANGE_ENDIANESS_BLOCK16(buffer, programSize);

    DPRINTF("Programming %u bytes at offset 0x%X\n", programSize, offset);
    // Disable interrupts during flash programming.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, programSize);
    flash_range_program(offset, buffer, programSize);
    restore_interrupts(ints);

    // Increment the flash offset by the actual bytes read.
    offset += bytesRead;
  }

  f_close(&file);
  free(buffer);
  DPRINTF("File %s stored to flash at address 0x%X\n", filename, flashAddress);
  return FR_OK;
}

// Tries to autorun a ROM specified in /roms/.autorun (or custom ROM folder)
static AutorunResult autorunIfRequested(void) {
  char autorunPath[MAX_PATH_SIZE];
  snprintf(autorunPath, sizeof(autorunPath), "%s/.autorun", romsFolder);
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
  snprintf(romPath, sizeof(romPath), "%s/%s", romsFolder, filenameStart);

  // Ensure the target file exists and is not a directory
  FILINFO fno;
  res = f_stat(romPath, &fno);
  if (res != FR_OK || (fno.fattrib & AM_DIR)) {
    DPRINTF("Autorun file not found or is a directory: %s\n", romPath);
    free(fileBuf);
    return AUTORUN_ERR_ROM_NOT_FOUND;  // ROM file not found or is a directory
  }

  // Copy ROM into flash
  unsigned int flashAddress = (unsigned int)&_rom_temp_start;
  res = storeFileToFlash(romPath, flashAddress);
  if (res != FR_OK) {
    DPRINTF("Failed to store autorun ROM to flash: %d\n", res);
    free(fileBuf);
    return AUTORUN_ERR_FLASH_STORE;  // Failed to store ROM in flash
  }

  // Update settings to boot directly into this ROM
  settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED,
                      filenameStart);
  free(fileBuf);
  settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                       ROM_MODE_DIRECT);
  settings_save(aconfig_getContext(), true);

  // Blink the LED (if available) forever instead of resetting. The ST and
  // SELECT stay serviced: a short press restarts the RP, which boots into the
  // ROM just stored (MODE is ROM_MODE_DIRECT now), as v2.1.2 did.
  DPRINTF("Autorun successful. Blinking LED to indicate autorun mode.\n");
  bool ledOn = false;
  uint32_t toggledUs = time_us_32();
  while (1) {
    emul_pollTick();
    if (time_us_32() - toggledUs < AUTORUN_BLINK_MS * 1000U) {
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

/**
 * @brief Comparison function for qsort to sort ROMs lexicographically
 * (case-insensitive).
 */
static int compareRoms(const void *first, const void *second) {
  const ROM *romA = (const ROM *)first;
  const ROM *romB = (const ROM *)second;
  return strcasecmp(romA->filename, romB->filename);
}

//-----------------------------------------------------------------
// Helper: URL-decode a string.
// Converts %xx sequences into their character values.
// dest_size includes room for the null terminator.
static void urlDecode(const char *src, char *dest, size_t destSize) {
  size_t idx = 0;
  while (*src && idx < destSize - 1) {
    if (*src == '%') {
      // Check if next two characters are valid hex digits.
      if (isxdigit((unsigned char)*(src + 1)) &&
          isxdigit((unsigned char)*(src + 2))) {
        char hex[3] = {*(src + 1), *(src + 2), '\0'};
        dest[idx++] = (char)strtol(hex, NULL, HEX_BASE);
        src += 3;
        continue;
      }
    }
    dest[idx++] = *src++;
  }
  dest[idx] = '\0';
}

// Percent-encode a path for a request line: everything but RFC 3986's
// unreserved characters and '/'. The catalog's names are decoded for the
// screen and the card ("Buggy Boy.img"); the request needs them encoded.
static void urlEncodePath(const char *src, char *dest, size_t destSize) {
  static const char hexDigits[] = "0123456789ABCDEF";
  size_t idx = 0;
  for (; *src != '\0'; src++) {
    unsigned char chr = (unsigned char)*src;
    bool plain = (isalnum(chr) != 0) || chr == '-' || chr == '_' ||
                 chr == '.' || chr == '~' || chr == '/';
    size_t need = plain ? 1U : 3U;
    if (idx + need >= destSize) {
      break;
    }
    if (plain) {
      dest[idx++] = (char)chr;
    } else {
      dest[idx++] = '%';
      dest[idx++] = hexDigits[chr / HEX_BASE];
      dest[idx++] = hexDigits[chr % HEX_BASE];
    }
  }
  dest[idx] = '\0';
}

static void readRomsSdcard(const char *folder) {
  FRESULT res;
  DIR dir;
  FILINFO fno;

  // Open the directory.
  res = f_opendir(&dir, folder);
  if (res != FR_OK) {
    DPRINTF("Error opening directory %s: %d\n", folder, res);
    return;
  }

  // Reset the ROM count.
  romsCount = 0;

  // Read each directory entry.
  for (;;) {
    // A large folder takes a while: keep answering the ST.
    emul_pollTick();
    res = f_readdir(&dir, &fno);
    if (res != FR_OK || fno.fname[0] == 0) {
      break;  // Break on error or end of directory
    }

    // Skip directories if you only want files.
    if (fno.fattrib & AM_DIR) {
      continue;
    }

    // Skip files starting with '.'
    if (fno.fname[0] == '.') {
      continue;
    }

    // Only add files with valid extensions.
    if (!hasValidExtension(fno.fname)) {
      continue;
    }

    // Store the filename into the roms[] array if there is space.
    if (romsCount < MAX_ROMS) {
      // Copy the filename from FatFS entry. fno.fname is a char array.
      strncpy(roms[romsCount].filename, fno.fname, MAX_FILENAME_LENGTH - 1);
      roms[romsCount].filename[MAX_FILENAME_LENGTH - 1] = '\0';
      strncpy(roms[romsCount].name, fno.fname, MAX_FILENAME_LENGTH - 1);
      roms[romsCount].name[MAX_FILENAME_LENGTH - 1] = '\0';
      romsCount++;
    } else {
      DPRINTF("Maximum ROM count reached (%d)\n", MAX_ROMS);
      break;
    }
  }

  f_closedir(&dir);

  // Sort the roms array alphabetically (lexicographically) by filename.
  qsort(roms, romsCount, sizeof(ROM), compareRoms);

  DPRINTF("Found %d ROMs on the SD card.\n", romsCount);
  maxRomPages = (romsCount + MAX_ROMS_PER_PAGE - 1) / MAX_ROMS_PER_PAGE;
}

static void readRomsCsv(const char *csvFilepath) {
  FRESULT res;
  FIL csvFile;
  const size_t lineSize = 256;
  char *line = malloc(lineSize);  // If you really have huge CSV lines, make
                                  // this larger, but 256 is often enough.
  int lineNum = 0;
  // Reduce stack usage: keep field buffers in static storage.
  static char field1[MAX_PATH_SIZE];  // URL
  static char field2[MAX_PATH_SIZE];  // Name
  static char field3[MAX_PATH_SIZE];  // Description (tune this)
  static char field4[MAX_PATH_SIZE];  // Tags (tune this)
  static char field5[12];             // Size (KB) (should never be big)

  romsCount = 0;

  if (line == NULL) {
    DPRINTF("Error allocating memory for CSV line buffer\n");
    return;
  }

  res = f_open(&csvFile, csvFilepath, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Error opening CSV file %s: %d\n", csvFilepath, res);
    free(line);
    return;
  }

  // Skip header
  if (f_gets(line, lineSize, &csvFile) == NULL) {
    DPRINTF("Error reading header from CSV file\n");
    f_close(&csvFile);
    free(line);
    return;
  }

  while (f_gets(line, lineSize, &csvFile) != NULL) {
    lineNum++;
    DPRINTF("Line %d: %s", lineNum, line);

    if (line[0] == '\0' || line[0] == '\n') continue;

    // These should be as small as possible!
    memset(field1, 0, sizeof(field1));
    memset(field2, 0, sizeof(field2));
    memset(field3, 0, sizeof(field3));
    memset(field4, 0, sizeof(field4));
    memset(field5, 0, sizeof(field5));

    char *ptr = line;
    int jdx;

// Tiny helper: extracts quoted CSV field (no inner quotes support)
#define EXTRACT_FIELD(dest)                                \
  do {                                                     \
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;    \
    if (*ptr != '\"') goto next_line;                      \
    ptr++;                                                 \
    jdx = 0;                                               \
    while (*ptr && *ptr != '\"' && jdx < sizeof(dest) - 1) \
      dest[jdx++] = *ptr++;                                \
    dest[jdx] = 0;                                         \
    if (*ptr == '\"') ptr++;                               \
    while (*ptr && (*ptr == ',' || isspace(*ptr))) ptr++;  \
  } while (0)

    EXTRACT_FIELD(field1);  // URL
    EXTRACT_FIELD(field2);  // Name
    EXTRACT_FIELD(field3);  // Description
    EXTRACT_FIELD(field4);  // Tags
    EXTRACT_FIELD(field5);  // Size

    // --- Decode fields directly into their struct fields ---
    if (romsCount < MAX_ROMS) {
      ROM *r = &roms[romsCount];

      urlDecode(field1, r->filename, sizeof(r->filename));
      urlDecode(field2, r->name, sizeof(r->name));
      urlDecode(field3, r->description, sizeof(r->description));
      urlDecode(field4, r->tags, sizeof(r->tags));
      r->size = atoi(field5);

      romsCount++;
    } else {
      DPRINTF("Maximum ROM count reached (%d)\n", MAX_ROMS);
      break;
    }
  next_line:;
  }
  f_close(&csvFile);
  free(line);

  qsort(roms, romsCount, sizeof(ROM), compareRoms);

  DPRINTF("Found %d ROMs in CSV file.\n", romsCount);
  maxRomPages = (romsCount + MAX_ROMS_PER_PAGE - 1) / MAX_ROMS_PER_PAGE;
#undef EXTRACT_FIELD
}

/**
 * @brief Displays a single page of ROM entries.
 *
 * @param roms        The array of ROM structures.
 * @param roms_count  The total number of ROM entries.
 * @param page_size   The number of lines (or rows) per page.
 * @param page_number The page number to display (starting with 0).
 */
static void displayRomsPage(const ROM roms[], int romsCount, int pageSize,
                            int pageNumber) {
  if (pageSize <= 0) {
    pageSize = 0;
  }

  int startIndex = pageNumber * pageSize;
  if (startIndex >= romsCount) {
    startIndex = romsCount - 1;
  }

  int endIndex = startIndex + pageSize;
  if (endIndex > romsCount) {
    endIndex = romsCount;
  }

  const size_t buffSize = TERM_SCREEN_SIZE_X;
  char *buff = malloc(buffSize);
  if (buff == NULL) {
    DPRINTF("Error allocating memory for display buffer\n");
    return;
  }
  // Page starts at 1 for user display.
  snprintf(buff, buffSize, "Page %d, ROMs %d to %d of %d:\n\n", pageNumber + 1,
           startIndex + 1, endIndex, romsCount);
  term_printString(buff);

  for (int i = startIndex; i < endIndex; i++) {
    // ROMs starts at 1 for user display.
    snprintf(buff, buffSize, "%d. %s\n", i + 1, roms[i].name);
    if (strlen(buff) >= (buffSize - 2)) {
      if (buff[strlen(buff) - 2] != '\n') {
        buff[strlen(buff) - 2] = '\n';
        buff[strlen(buff) - 1] = '\0';
      }
    }
    term_printString(buff);
  }
  free(buff);

  currentRomPage = pageNumber;
}

static void navigatePages(int pageNumber) {
  term_printString(
      "\x1B"
      "E");
  displayRomsPage(roms, romsCount, MAX_ROMS_PER_PAGE, pageNumber);
  term_printString("\n");
  if (pageNumber < maxRomPages - 1) {
    term_printString("[N]ext ");
  }
  if (pageNumber > 0) {
    term_printString("[P]rev ");
  }
  term_printString("[M]enu or ROM number");
}

static void showTitle() {
  term_printString(
      "\x1B"
      "E"
      "ROM Emulator - " RELEASE_VERSION "\n");
}

static void menu(void) {
  menuState.menuLevel = TERM_ROMS_MENU_MAIN;
  // Before [D] is offered: v2.1.2 set this only while drawing the status line
  // below, and showed [D] because it drew the menu twice at boot.
  ip_addr_t currentIp = network_getCurrentIp();
  hasNetwork = wifiConnected || (currentIp.addr != 0);
  showTitle();
  term_printString("\n\n");
  term_printString("[B] Browse ROMs in microSD card\n");
  if (hasNetwork && catalogAvailable) {
    term_printString("[D] Download ROMs from internet server\n");
  }
  term_printString("[S] Settings\n\n");
  term_printString("[E] Exit to desktop\n");
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
    term_printString(romSelected->value);
    term_printString("\n");
  }
  term_printString("\n");

  term_printString("[M] Refresh this menu\n");

  term_printString("\n");

  // Display network status
  term_printString("Network status: ");
  if (hasNetwork) {
    term_printString("Connected\n");
  } else {
    term_printString("Not connected\n");
  }

  term_printString("\n");
  term_printString("Select an option: ");
}

// Command handlers
void cmdMenu(const char *arg) { menu(); }

void cmdNext(const char *arg) {
  if (currentRomPage < maxRomPages - 1) {
    currentRomPage++;
  }
  navigatePages(currentRomPage);
}

void cmdPrev(const char *arg) {
  currentRomPage--;
  if (currentRomPage < 0) {
    currentRomPage = 0;
  }
  navigatePages(currentRomPage);
}

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

// The ROM list is allocated once, when the menu starts; without it neither
// list can be shown.
static bool romsAvailable(void) {
  if (roms == NULL) {
    term_printString("Not enough memory for the ROM list.\n");
    return false;
  }
  return true;
}

void cmdCard(const char *arg) {
  if (!romsAvailable()) {
    return;
  }
  readRomsSdcard(romsFolder);
  menuState.menuLevel = TERM_ROMS_MENU_BROWSE_SD;

  if (romsCount == 0) {
    term_printString("No ROMs found in the SD card.\n");
    term_printString("Download ROMs from internet,\n");
    term_printString("or copy them to folder '");
    term_printString(romsFolder);
    term_printString("'\n\n");
  } else {
    currentRomPage = 0;
    navigatePages(currentRomPage);
  }
}

void cmdNetwork(const char *arg) {
  if (!catalogAvailable || network_getCurrentIp().addr == 0) {
    term_printString("Network catalog not available.\n");
    return;
  }
  if (!romsAvailable()) {
    return;
  }
  char csvPath[MAX_PATH_SIZE];
  snprintf(csvPath, sizeof(csvPath), "%s/roms.csv", romsFolder);
  readRomsCsv(csvPath);
  menuState.menuLevel = TERM_ROMS_MENU_BROWSE_NETWORK;
  currentRomPage = 0;
  navigatePages(currentRomPage);
}

void cmdLaunch(const char *arg) {
  menuState.menuLevel = TERM_ROMS_MENU_LAUNCH;
  term_printString("The ROM will boot shortly...\n\n");
  if (delayMode) {
    term_printString(
        "ROM delay/ripper mode enabled. You must press SELECT to activate the "
        "ROM.\n");
  }
  term_printString("To return to this menu, press SELECT\n");
  term_printString("If ROM doesn't boot, reset the computer\n");
  SettingsConfigEntry *romFile =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED);
  if (romFile != NULL) {
    // Load the ROM file from the SD card
    char filename[MAX_PATH_SIZE];
    snprintf(filename, MAX_PATH_SIZE, "%s/%s", romsFolder, romFile->value);
    unsigned int flashAddress = (unsigned int)&_rom_temp_start;
    DPRINTF("Loading ROM file into FLASH: %s at 0x%X\n", filename,
            flashAddress);
    FRESULT fresult = storeFileToFlash(filename, flashAddress);
    if (fresult != FR_OK) {
      DPRINTF("Error loading ROM file into FLASH: %d\n", fresult);
    } else {
      // Now we can set the ROM emulation mode here
      // Set the ROM emulation mode to 0 (ROM no delay)
      settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_MODE,
                           delayMode ? ROM_MODE_DELAY : ROM_MODE_DIRECT);
      settings_save(aconfig_getContext(), true);

      keepActive = false;  // Exit the active loop
    }
  } else {
    DPRINTF("No ROM file selected.\n");
  }
}

void cmdUnknown(const char *arg) {
  switch (menuState.menuLevel) {
    case TERM_ROMS_MENU_MAIN:
      menu();
      break;
    case TERM_ROMS_MENU_BROWSE_SD: {
      // Convert to integer the argument
      int romNumber = atoi(arg);
      if (romNumber > 0 && romNumber <= romsCount) {
        term_printString("Selected ROM: ");
        term_printString(roms[romNumber - 1].filename);
        term_printString("\n");
        // Save the selected ROM to the settings
        settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED,
                            roms[romNumber - 1].filename);
        settings_save(aconfig_getContext(), true);
        menu();
      } else {
        term_printString(
            "Invalid ROM number. Please select a valid ROM "
            "number.\n");
      }
    } break;
    case TERM_ROMS_MENU_BROWSE_NETWORK: {
      // Convert to integer the argument
      int romNumber = atoi(arg);
      if (romNumber > 0 && romNumber <= romsCount) {
        term_printString("\nROM number: ");
        term_printString(arg);
        term_printString("\n");

        // The
        term_printString("Name: ");
        term_printString(roms[romNumber - 1].name);
        term_printString("\n");

        term_printString("Filename: ");
        term_printString(roms[romNumber - 1].filename);
        term_printString("\n");

        term_printString("Description: ");
        term_printString(roms[romNumber - 1].description);
        term_printString("\n");

        term_printString("Tags: ");
        term_printString(roms[romNumber - 1].tags);
        term_printString("\n");

        term_printString("Size: ");
        char sizeStr[MAX_PATH_SIZE / 4];
        snprintf(sizeStr, sizeof(sizeStr), "%d KB\n", roms[romNumber - 1].size);
        term_printString(sizeStr);

        term_printString("\nPress RETURN to load the ROM.\n");
        term_printString("Press any other key to return to the menu.\n");
        downloadRomSelected = romNumber - 1;
        menuState.menuLevel =
            TERM_ROMS_MENU_BROWSE_NETWORK + TERM_ROMS_MENU_SUBMENU;

      } else {
        term_printString(
            "Invalid ROM number. Please select a valid ROM "
            "number.\n");
      }
    } break;
    case TERM_ROMS_MENU_BROWSE_NETWORK + TERM_ROMS_MENU_SUBMENU:
      if (arg[0] == '\0' || arg[0] == '\n') {
        // Clean the ROM_SELECTED setting
        settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED,
                            "");
        settings_save(aconfig_getContext(), true);

        // The ROM is fetched from the catalog's own host, at the root.
        const download_url_components_t *catalogUrl =
            download_getUrlComponents();
        char encoded[MAX_PATH_SIZE * 3];
        urlEncodePath(roms[downloadRomSelected].filename, encoded,
                      sizeof(encoded));
        char url[DOWNLOAD_URL_SIZE];
        if (catalogUrl->port != 0) {
          snprintf(url, sizeof(url), "%s://%s:%u/%s", catalogUrl->protocol,
                   catalogUrl->host, (unsigned)catalogUrl->port, encoded);
        } else {
          snprintf(url, sizeof(url), "%s://%s/%s", catalogUrl->protocol,
                   catalogUrl->host, encoded);
        }
        DPRINTF("Downloading ROM: %s/%s\n", romsFolder,
                roms[downloadRomSelected].filename);
        DPRINTF("URL: %s\n", url);
        download_setFilepath(url);
        download_err_t err = download_start();
        if (err != DOWNLOAD_OK) {
          DPRINTF("Error starting download: %d\n", err);
        } else {
          romDownloadActive = true;
        }
        menuState.menuLevel = TERM_ROMS_MENU_MAIN;
        menu();
      } else {
        menuState.menuLevel = TERM_ROMS_MENU_BROWSE_NETWORK;
        navigatePages(currentRomPage);
      }
      break;
    case TERM_ROMS_MENU_LAUNCH:
      break;
    default:
      term_printString(
          "Unknown command. Type 'help' for a list of commands.\n");
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

static void preinit() {
  // Initialize the terminal
  term_init();

  // Clear the screen
  term_clearScreen();

  // Show the title
  showTitle();
  term_printString("\n\n");
  term_printString("Configuring network... please wait...\n");
  term_printString("or press SHIFT to boot to desktop.\n");

  display_refresh();
}

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

static void romDownloadUpdate() {
  // Save the selected ROM to the settings
  if (downloadRomSelected > 0) {
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED,
                        roms[downloadRomSelected].filename);
    settings_save(aconfig_getContext(), true);
    menu();
  }
}

// One step of a download the user started from the catalog: poll it while it
// runs, then keep the file and select it, or drop it. Downloads started
// elsewhere (the devdownload hook) are their starter's to finish.
static void romDownloadPoll(void) {
  if (!romDownloadActive) {
    return;
  }
  download_status_t status = download_getStatus();
  if (status == DOWNLOAD_STATUS_STARTED ||
      status == DOWNLOAD_STATUS_IN_PROGRESS) {
    download_poll();
    return;
  }
  if (status != DOWNLOAD_STATUS_COMPLETED &&
      status != DOWNLOAD_STATUS_FAILED) {
    return;
  }
  romDownloadActive = false;
  download_err_t err = download_finish();
  if (err == DOWNLOAD_OK) {
    err = download_confirm();
  }
  download_setStatus(DOWNLOAD_STATUS_IDLE);
  if (err != DOWNLOAD_OK) {
    DPRINTF("ROM download failed: %d\n", err);
    return;
  }
  romDownloadUpdate();
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

static void __not_in_flash_func(romModeWaitForSelect)(void) {
  selectPressed = false;
  while (!selectPressed) {
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

#define ROM4_EDGE_FALL_BITS (GPIO_IRQ_EDGE_FALL << (4 * (ROM4_GPIO % 8)))

// One-shot: records the first access and switches itself off, with register
// writes only (no flash code in an interrupt).
static void __not_in_flash_func(romModeFirstAccessIrq)(void) {
  if ((gpio_get_irq_event_mask(ROM4_GPIO) & GPIO_IRQ_EDGE_FALL) == 0U) {
    return;
  }
  gpio_acknowledge_irq(ROM4_GPIO, GPIO_IRQ_EDGE_FALL);
  romModeFirstAccessUs = time_us_32();
  hw_clear_bits(&io_bank0_hw->proc0_irq_ctrl.inte[ROM4_GPIO / 8],
                ROM4_EDGE_FALL_BITS);
}

// Called the moment the engine serves the bus: takes the time, reads the
// latch of edges since reset, then arms the one-shot for the next access.
static void romModeMarkLive(void) {
  romModeLiveUs = time_us_32();
  romModeAccessBeforeLive =
      (io_bank0_hw->intr[ROM4_GPIO / 8] & ROM4_EDGE_FALL_BITS) != 0U;
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
    romModeWaitForSelect();
  }

  // Copy the ROM in the flash to RAM, both banks
  DPRINTF("Copy the ROM firmware to RAM: 0x%X, length: %u bytes\n",
          (unsigned int)&_rom_temp_start, ROM_SIZE_BYTES * ROM_BANKS);
  COPY_FIRMWARE_TO_RAM((uint16_t *)&_rom_temp_start,
                       ROM_SIZE_WORDS * ROM_BANKS);
  if (init_romemul_two_banks(false) < 0) {
    panic("init_romemul_two_banks failed: PIO/DMA claim returned <0");
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
          (unsigned long)romModeLiveUs,
          romModeAccessBeforeLive ? "yes" : "no");

#ifdef BLINK_H
  blink_on();
#endif

  DPRINTF("ROM emulation mode started. Waiting for SELECT button\n");
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
  int sdcardErr = sdcard_initFilesystem(&fsys, folderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("Error initializing the SD card: %i\n", sdcardErr);
    failure(
        "SD card error.\nCheck the card is inserted correctly.\nInsert card "
        "and restart the computer.");
    while (1) {
      // Wait forever, answering the ST
      emul_pollTick();
#ifdef BLINK_H
      blink_toogle();
#endif
      sleep_ms(SLEEP_LOOP_MS);
    }
  } else {
    DPRINTF("SD card found & initialized\n");
    AutorunResult autorunResult = autorunIfRequested();
    if (autorunResult != AUTORUN_OK) {
      DPRINTF("Autorun error: %i. Continue.\n", autorunResult);
    }
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // The "please wait" screen while the network comes up
  preinit();

  // 6. The network. Its parameters are Booster's global settings.
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  wifi_mode_t wifiModeValue = WIFI_MODE_STA;
  wifiConnected = false;
  if (wifiMode == NULL) {
    DPRINTF("No WiFi mode found in the settings. No initializing.\n");
  } else {
    wifiModeValue = (wifi_mode_t)atoi(wifiMode->value);
    if (wifiModeValue != WIFI_MODE_AP) {
      DPRINTF("WiFi mode is STA\n");
      wifiModeValue = WIFI_MODE_STA;
      int err = network_wifiInit(wifiModeValue);
      if (err != 0) {
        DPRINTF("Error initializing the network: %i. No initializing.\n", err);
      } else {
        // Answer the ST during the (multi-second) connect, so its commands
        // don't pile up in the ROM3 ring.
        network_setPollingCallback(emul_pollTick);
        int maxAttempts = 3;
        int attempt = 0;
        err = NETWORK_WIFI_STA_CONN_ERR_TIMEOUT;

        while ((attempt < maxAttempts) &&
               (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
          err = network_wifiStaConnect();
          attempt++;

          if ((err > 0) && (err < NETWORK_WIFI_STA_CONN_ERR_TIMEOUT)) {
            DPRINTF("Error connecting to the WiFi network: %i\n", err);
          }
        }

        if (err == NETWORK_WIFI_STA_CONN_ERR_TIMEOUT) {
          DPRINTF("Timeout connecting to the WiFi network after %d attempts\n",
                  maxAttempts);
        } else if (err == 0) {
          wifiConnected = true;
        }
        network_setPollingCallback(NULL);
      }
    } else {
      DPRINTF("WiFi mode is AP. No initializing.\n");
    }
  }

  // 7. The ROM catalog, downloaded into the ROMs folder at every boot.
#if APP_DOWNLOAD_HTTPS == 1
  SettingsConfigEntry *catalog =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_HTTPS_CATALOG);
#else
  SettingsConfigEntry *catalog =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_HTTP_CATALOG);
#endif
  if (!wifiConnected) {
    DPRINTF("WiFi not connected. Skipping catalog download.\n");
    catalogAvailable = false;
  } else if (catalog == NULL) {
    DPRINTF("No catalog URL found in the settings. No initializing.\n");
    catalogAvailable = false;
  } else {
    DPRINTF("Catalog URL: %s\n", catalog->value);
    download_setFilepath(catalog->value);
    download_err_t err = download_start();
    if (err != DOWNLOAD_OK) {
      DPRINTF("Error starting catalog download: %d\n", err);
      catalogAvailable = false;
    } else {
      DPRINTF("Waiting for catalog download to complete...\n");
      download_status_t status = download_getStatus();
      while (status == DOWNLOAD_STATUS_STARTED ||
             status == DOWNLOAD_STATUS_IN_PROGRESS) {
        download_poll();
        emul_pollTick();
        status = download_getStatus();
      }
      err = download_finish();
      if (err == DOWNLOAD_OK) {
        err = download_confirm();
      }
      download_setStatus(DOWNLOAD_STATUS_IDLE);
      catalogAvailable = (err == DOWNLOAD_OK);
      if (!catalogAvailable) {
        DPRINTF("Catalog download failed: %d\n", err);
      }
    }
  }

  // 8. The terminal and the menu
  init(folderName);

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. The main loop, until a ROM is launched or Booster is chosen
  roms = malloc(MAX_ROMS * sizeof(ROM));
  if (roms == NULL) {
    DPRINTF("Error allocating memory for ROMs\n");
  }
#if defined(_DEBUG) && (_DEBUG != 0)
  // Debug builds only: serve the SWD mailbox of tools/dev/swd.py
  devhooks_setAppHandler(emul_devhooksApp);
#endif

  DPRINTF("Start the app loop here\n");
  absolute_time_t nextNetworkPoll = get_absolute_time();
  while (getKeepActive()) {
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
      menu();
      display_refresh();
    }
#if PICO_CYW43_ARCH_POLL
    // Wi-Fi every 10 ms, which is plenty for lwIP's timers and leaves the
    // loop to the command ring.
    if (absolute_time_diff_us(nextNetworkPoll, get_absolute_time()) >= 0) {
      network_safePoll();
      nextNetworkPoll = make_timeout_time_ms(10);
    }
#endif

    // Run the terminal foreground (consume the published command, render
    // output, etc.).
    term_loop();

    // A download the user started from the catalog
    romDownloadPoll();
  }
  if (roms != NULL) {
    free(roms);
    roms = NULL;
  }

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

    // Jump to the booster app
    DPRINTF("Jumping to the booster app...\n");
    emul_quiesce();
    reset_jump_to_booster();
  }
}
