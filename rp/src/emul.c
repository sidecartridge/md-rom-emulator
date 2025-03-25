/**
 * File: emul.c
 * Author: Diego Parrilla Santamaría
 * Date: February 2025
 * Copyright: 2025 - GOODDATA LABS
 * Description: Code for the ROM emulator core and setup features
 */

#include "emul.h"

// inclusw in the C file to avoid multiple definitions
#include "target_firmware.h"  // Include the target firmware binary

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
    {"", cmdUnknown},
};

// Number of commands in the table
static const size_t numCommands = sizeof(commands) / sizeof(commands[0]);

// Global array to store ROM info.
static ROM roms[MAX_ROMS];
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

// Delay/ripper mode?
static bool delayMode = false;

static FRESULT storeFileToFlash(const char *filename, uint32_t flashAddress) {
  FIL file;
  FRESULT res;
  UINT bytesRead;
  uint8_t buffer[FLASH_SECTOR_SIZE];
  FSIZE_t size;

  // Open the file (read-only, binary mode)
  res = f_open(&file, filename, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Error opening file %s: %d\n", filename, res);
    return res;
  }

  // Get file size (use FSIZE_t for portability)
  size = f_size(&file);
  DPRINTF("File size: %u bytes\n", (unsigned int)size);

  // If the file size is a multiple of FLASH_SECTOR_SIZE plus 4 bytes, check for
  // 4-byte padding.
  if (size > 4 && ((size - 4) % FLASH_SECTOR_SIZE == 0)) {
    // Read the first 4 bytes
    res = f_read(&file, buffer, 4, &bytesRead);
    if (res != FR_OK || bytesRead != 4) {
      DPRINTF("Error reading header of file: %d (bytes read: %u)\n", res,
              bytesRead);
      f_close(&file);
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
        return res;
      }
    }
  }

  // Calculate the flash programming offset relative to XIP_BASE.
  uint32_t offset = flashAddress - XIP_BASE;

  // Read and program the file in FLASH_SECTOR_SIZE chunks.
  while (1) {
    res = f_read(&file, buffer, FLASH_SECTOR_SIZE, &bytesRead);
    if (res != FR_OK) {
      DPRINTF("Error reading file: %d\n", res);
      f_close(&file);
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
  return FR_OK;
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
      roms[romsCount].path[0] = '\0';
      strncat(roms[romsCount].path, folder, MAX_PATH_SIZE - 1);
      strncat(roms[romsCount].path, "/", MAX_PATH_SIZE - 1);
      strncat(roms[romsCount].path, fno.fname, MAX_PATH_SIZE - 1);
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
  char line[FLASH_PAGE_SIZE * 2];
  int lineNum = 0;

  // Reset the ROM count.
  romsCount = 0;

  // Open the CSV file.
  res = f_open(&csvFile, csvFilepath, FA_READ);
  if (res != FR_OK) {
    DPRINTF("Error opening CSV file %s: %d\n", csvFilepath, res);
    return;
  }

  // Read and discard the header line.
  if (f_gets(line, sizeof(line), &csvFile) == NULL) {
    DPRINTF("Error reading header from CSV file\n");
    f_close(&csvFile);
    return;
  }

  // Read each subsequent line.
  while (f_gets(line, sizeof(line), &csvFile) != NULL) {
    lineNum++;

    // Skip empty lines.
    if (line[0] == '\0' || line[0] == '\n') continue;

    // Expected format:
    // "URL","Name","Description","Tags","Size (KB)"
    char field1[MAX_PATH_SIZE * 2] = {0};  // URL
    char field2[MAX_PATH_SIZE * 2] = {0};  // Name
    char field3[MAX_PATH_SIZE] = {0};      // Description
    char field4[MAX_PATH_SIZE * 2] = {0};  // Tags
    char field5[MAX_PATH_SIZE / 4] = {0};  // Size (KB)

    char *ptr = line;

// Helper macro to extract a quoted field.
#define EXTRACT_FIELD(dest)                                              \
  do {                                                                   \
    while (*ptr && isspace((unsigned char)*ptr)) ptr++;                  \
    if (*ptr != '"') {                                                   \
      DPRINTF("Line %d: expected '\"'\n", lineNum);                      \
      goto next_line;                                                    \
    }                                                                    \
    ptr++; /* skip opening quote */                                      \
    int jdx = 0;                                                         \
    while (*ptr && *ptr != '"' && jdx < (int)sizeof(dest) - 1) {         \
      dest[jdx++] = *ptr++;                                              \
    }                                                                    \
    dest[jdx] = '\0';                                                    \
    if (*ptr == '"') ptr++;                                              \
    while (*ptr && (*ptr == ',' || isspace((unsigned char)*ptr))) ptr++; \
  } while (0)

    EXTRACT_FIELD(field1);  // URL
    EXTRACT_FIELD(field2);  // Name
    EXTRACT_FIELD(field3);  // Description
    EXTRACT_FIELD(field4);  // Tags
    EXTRACT_FIELD(field5);  // Size (KB)

    {
      // Decode URL-encoded text from the fields.
      char decodedField1[MAX_PATH_SIZE * 2] = {0};
      char decodedField2[MAX_PATH_SIZE * 2] = {0};
      char decodedField3[MAX_PATH_SIZE] = {0};
      char decodedField4[MAX_PATH_SIZE * 2] = {0};
      urlDecode(field1, decodedField1, sizeof(decodedField1));
      urlDecode(field2, decodedField2, sizeof(decodedField2));
      urlDecode(field3, decodedField3, sizeof(decodedField3));
      urlDecode(field4, decodedField4, sizeof(decodedField4));

      int sizeKb = atoi(field5);

      // Store the parsed values into the ROM structure.
      if (romsCount < MAX_ROMS) {
        // Store the decoded URL into filename.
        strncpy(roms[romsCount].filename, field1, MAX_FILENAME_LENGTH - 1);
        roms[romsCount].filename[MAX_FILENAME_LENGTH - 1] = '\0';

        // Build the full path: ROMs folder + "/" + decoded URL.
        const char *romsFolder =
            settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROMS_FOLDER)
                ->value;
        roms[romsCount].path[0] = '\0';
        strncat(roms[romsCount].path, romsFolder, MAX_PATH_SIZE - 1);
        strncat(roms[romsCount].path, "/", MAX_PATH_SIZE - 1);
        strncat(roms[romsCount].path, decodedField1, MAX_PATH_SIZE - 1);

        // Store the friendly name.
        strncpy(roms[romsCount].name, decodedField2, MAX_FILENAME_LENGTH - 1);
        roms[romsCount].name[MAX_FILENAME_LENGTH - 1] = '\0';

        // Store the description.
        int sizeField3 = sizeof(field3) > 0 ? sizeof(field3) - 1 : 0;
        strncpy(roms[romsCount].description, decodedField3, sizeField3);
        roms[romsCount].description[sizeField3] = '\0';

        // Store the tags.
        strncpy(roms[romsCount].tags, decodedField4, MAX_FILENAME_LENGTH - 1);
        roms[romsCount].tags[MAX_FILENAME_LENGTH - 1] = '\0';

        // Store the size (in KB).
        roms[romsCount].size = sizeKb;

        romsCount++;
      } else {
        DPRINTF("Maximum ROM count reached (%d)\n", MAX_ROMS);
        break;
      }
    }

  next_line:;  // Label for skipping to next line.
  }

  f_close(&csvFile);

  // Sort the ROMs array alphabetically by filename.
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

  char buff[TERM_SCREEN_SIZE_X];
  // Page starts at 1 for user display.
  snprintf(buff, sizeof(buff), "Page %d, ROMs %d to %d of %d:\n\n",
           pageNumber + 1, startIndex + 1, endIndex, romsCount);
  term_printString(buff);

  for (int i = startIndex; i < endIndex; i++) {
    // ROMs starts at 1 for user display.
    snprintf(buff, sizeof(buff), "%d. %s\n", i + 1, roms[i].name);
    if (strlen(buff) >= (TERM_SCREEN_SIZE_X - 2)) {
      if (buff[strlen(buff) - 2] != '\n') {
        buff[strlen(buff) - 2] = '\n';
        buff[strlen(buff) - 1] = '\0';
      }
    }
    term_printString(buff);
  }

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
  showTitle();
  term_printString("\n\n");
  term_printString("[B] Browse ROMs in microSD card\n");
  term_printString("[D] Download ROMs from internet server\n");
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
  ip_addr_t currentIp = network_getCurrentIp();

  hasNetwork = currentIp.addr != 0;
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

void cmdCard(const char *arg) {
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
  readRomsCsv("/roms/roms.csv");
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
      settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_ROM_MODE,
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

        // Create full path to download the file
        char fullPath[MAX_PATH_SIZE];
        snprintf(fullPath, MAX_PATH_SIZE, "%s/%s", romsFolder,
                 roms[downloadRomSelected].filename);
        DPRINTF("Downloading ROM: %s\n", fullPath);
        download_url_components_t components = {};
        char url[MAX_PATH_SIZE * 2];
        snprintf(url, sizeof(url), "%s://%s/%s",
                 download_getUrlComponents()->protocol,
                 download_getUrlComponents()->host,
                 roms[downloadRomSelected].filename);
        DPRINTF("URL: %s\n", url);
        download_setFilepath(url);
        download_err_t err = download_start();
        if (err != DOWNLOAD_OK) {
          DPRINTF("Error starting download: %d\n", err);
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

  // Example 1: Move the cursor up one line.
  // VT52 sequence: ESC A (moves cursor up)
  // The escape sequence "\x1BA" will move the cursor up one line.
  // term_printString("\x1B" "A");
  // After moving up, print text that overwrites part of the previous line.
  // term_printString("Line 2 (modified by ESC A)\n");

  // Example 2: Move the cursor right one character.
  // VT52 sequence: ESC C (moves cursor right)
  // term_printString("\x1B" "C");
  // term_printString(" <-- Moved right with ESC C\n");

  // Example 3: Direct cursor addressing.
  // VT52 direct addressing uses ESC Y <row> <col>, where:
  //   row_char = row + 0x20, col_char = col + 0x20.
  // For instance, to move the cursor to row 0, column 10:
  //   row: 0 -> 0x20 (' ')
  //   col: 10 -> 0x20 + 10 = 0x2A ('*')
  // term_printString("\x1B" "Y" "\x20" "\x2A");
  // term_printString("Text at row 0, column 10 via ESC Y\n");

  // term_printString("\x1B" "Y" "\x2A" "\x20");

  display_refresh();
}

void emul_start() {
  // The anatomy of an app or microfirmware is as follows:
  // - The driver code running in the remote device (the computer)
  // - the driver code running in the host device (the rp2040/rp2350)
  //
  // The driver code running in the remote device is responsible for:
  // 1. Perform the emulation of the device (ex: a ROM cartridge)
  // 2. Handle the communication with the host device
  // 3. Handle the configuration of the driver (ex: the ROM file to load)
  // 4. Handle the communication with the user (ex: the terminal)
  //
  // The driver code running in the host device is responsible for:
  // 1. Handle the communication with the remote device
  // 2. Handle the configuration of the driver (ex: the ROM file to load)
  // 3. Handle the communication with the user (ex: the terminal)
  //
  // Hence, we effectively have two drivers running in two different devices
  // with different architectures and capabilities.
  //
  // Please read the documentation to learn to use the communication protocol
  // between the two devices in the tprotocol.h file.
  //

  // 1. Check if the host device must be initialized to perform the emulation
  //    of the device, or start in setup/configuration mode
  SettingsConfigEntry *appMode =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_MODE);
  int appModeValue = ROM_MODE_SETUP;  // Setup menu
  if (appMode == NULL) {
    DPRINTF("ROM_MODE not found in the configuration. Using default value\n");
  } else {
    appModeValue = atoi(appMode->value);
    DPRINTF("Start ROM emulation in mode: %i\n", appModeValue);
  }

  // 2. Initialiaze the normal operation of the app, unless the configuration
  // option says to start the config app Or a SELECT button is (or was) pressed
  // to start the configuration section of the app

  if ((appModeValue == ROM_MODE_DIRECT) || (appModeValue == ROM_MODE_DELAY)) {
    // If in ROM delay or ripper mode, we need to wait for the SELECT button
    // to start the ROM emulation So the user boots as usual, but the ROM
    // emulation is not started until the SELECT button is pressed because
    // that is how the old ripper cartridges worked
    if (appModeValue == ROM_MODE_DELAY) {
      select_configure();
      select_setLongResetCallback(reset_deviceAndEraseFlash);
      // Wait until SELECT is pressed
      while (!select_detectPush()) {
        // Run the ROM emulation state machine
        sleep_ms(SLEEP_LOOP_MS);
      }
      // Select button pressed. Wait until it is released
      select_waitPush();
    }

    // Copy the ROM in the flash to RAM
    unsigned int flashAddress = (unsigned int)&_rom_temp_start;
    DPRINTF("Copy the ROM firmware to RAM: 0x%X, length: %u bytes\n",
            flashAddress, ROM_SIZE_BYTES * ROM_BANKS);
    COPY_FIRMWARE_TO_RAM((uint16_t *)flashAddress, ROM_SIZE_BYTES * ROM_BANKS);
    init_romemul(NULL, NULL, false);

#ifdef BLINK_H
    blink_on();
#endif

    select_configure();
    select_setLongResetCallback(reset_deviceAndEraseFlash);

    // Wait until SELECT is pressed
    while (!select_detectPush()) {
      // Run the ROM emulation state machine
      sleep_ms(SLEEP_LOOP_MS);
    }
    // Select button pressed. Wait until it is released
    select_waitPush();
    // Change the mode to setup menu
    // Set the ROM emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_ROM_MODE,
                         ROM_MODE_SETUP);
    settings_save(aconfig_getContext(), true);
    // Now reset the device
    reset_device();
  }

  // 3. If we are here, it means the app is not in ROM emulation mode, but in
  // setup/configuration mode

  // As a rule of thumb, the remote device (the computer) driver code must
  // be copied to the RAM of the host device where the emulation will take
  // place.
  // The code is stored as an array in the target_firmware.h file
  //
  // Copy the terminal firmware to RAM
  COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);
  init_romemul(NULL, term_dma_irq_handler_lookup, false);

  // 4. During the setup/configuration mode, the driver code must interact
  // with the user to configure the device. To simplify the process, the
  // terminal emulator is used to interact with the user.
  // The terminal emulator is a simple text-based interface that allows the
  // user to configure the device using text commands.

  // Initialize the display
  display_setupU8g2();

  // 5. Init the sd card
  // Most of the apps or microfirmwares will need to read and write files
  // to the SD card. The SD card is used to store the ROM files, configuration
  // files, and other data.
  // The SD card is initialized here. If the SD card is not present, the
  // app will show an error message and wait for the user to insert the SD card.
  // The app will not start until the SD card is inserted correctly.
  // Each app or microfirmware must have a folder in the SD card where the
  // files are stored. The folder name is defined in the configuration.

  FATFS fsys;
  SettingsConfigEntry *romsFolder =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROMS_FOLDER);
  char *romsFolderName = "/roms";
  if (romsFolder == NULL) {
    DPRINTF(
        "ROMS_FOLDER not found in the configuration. Using default value\n");
  } else {
    DPRINTF("ROMS_FOLDER: %s\n", romsFolder->value);
    romsFolderName = romsFolder->value;
  }
  int sdcardErr = sdcard_initFilesystem(&fsys, romsFolderName);
  if (sdcardErr != SDCARD_INIT_OK) {
    DPRINTF("Error initializing the SD card: %i\n", sdcardErr);
    failure(
        "SD card error.\nCheck the card is inserted correctly.\nInsert card "
        "and restart the computer.");
    while (1) {
      // Wait forever
      term_loop();
#ifdef BLINK_H
      blink_toogle();
#endif
    }
  } else {
    DPRINTF("SD card found & initialized\n");
  }

  // Initialize the display again (in case the terminal emulator changed it)
  display_setupU8g2();

  // Pre-init the terminal emulator for ROMS waiting for the network
  preinit();

  // 6. Init the network, if needed
  // It's always a good idea to wait for the network to be ready
  // Get the WiFi mode from the settings
  SettingsConfigEntry *wifiMode =
      settings_find_entry(gconfig_getContext(), PARAM_WIFI_MODE);
  wifi_mode_t wifiModeValue = WIFI_MODE_STA;
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
        // Set the term_loop as a callback during the polling period
        network_setPollingCallback(term_loop);
        // Connect to the WiFi network
        int maxAttempts = 3;  // or any other number defined elsewhere
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
          // Optionally, return an error code here.
        }
        network_setPollingCallback(NULL);
      }
    } else {
      DPRINTF("WiFi mode is AP. No initializing.\n");
    }
  }

  // 7. Download the list of available ROMs from the network
  // Since the list of availanble ROMs is stored in a remote server and does not
  // change frequently, it is a good idea to download the list of ROMs at the
  // beginning of the app. This way, the user can browse the list of ROMs
  // available in the server and download the ROMs to the SD card.
  // The list of ROMs is stored in a CSV file in the server. The CSV file
  // contains the URL of the ROM file, the name of the ROM, the description,
  // the tags, and the size of the ROM file.

  char *catalogUrl = NULL;
#if APP_DOWNLOAD_HTTPS == 1
  SettingsConfigEntry *catalog = settings_find_entry(
      aconfig_get_context(), ACONFIG_PARAM_ROM_HTTPS_CATALOG);
#else
  SettingsConfigEntry *catalog =
      settings_find_entry(aconfig_getContext(), ACONFIG_PARAM_ROM_HTTP_CATALOG);
#endif
  if (catalog == NULL) {
    DPRINTF("No catalog URL found in the settings. No initializing.\n");
  } else {
    catalogUrl = catalog->value;
    DPRINTF("Catalog URL: %s\n", catalogUrl);
    download_setFilepath(catalogUrl);
    download_start();
  }

  // 8. Now complete the terminal emulator initialization
  // The terminal emulator is used to interact with the user to configure the
  // device.
  init(romsFolderName);

  // Blink on
#ifdef BLINK_H
  blink_on();
#endif

  // 9. Start the main loop
  // The main loop is the core of the app. It is responsible for running the
  // app, handling the user input, and performing the tasks of the app.
  // The main loop runs until the user decides to launch a ROM or exit the app.
  DPRINTF("Start the app loop here\n");
  absolute_time_t wifiScanTime = make_timeout_time_ms(
      WIFI_SCAN_TIME_MS);  // 3 seconds minimum for network scanning

  absolute_time_t startDownloadTime =
      make_timeout_time_ms(DOWNLOAD_DAY_MS);  // Future time
  while (getKeepActive()) {
#if PICO_CYW43_ARCH_POLL
    network_safe_poll();
    cyw43_arch_wait_for_work_until(wifi_scan_time);
#else
    sleep_ms(SLEEP_LOOP_MS);
#endif
    // Check remote commands
    term_loop();

    // Check the download status
    switch (download_getStatus()) {
      case DOWNLOAD_STATUS_REQUESTED: {
        startDownloadTime = make_timeout_time_ms(
            DOWNLOAD_START_MS);  // 3 seconds to start the download
        download_setStatus(DOWNLOAD_STATUS_NOT_STARTED);
        break;
      }
      case DOWNLOAD_STATUS_NOT_STARTED: {
        if ((absolute_time_diff_us(get_absolute_time(), startDownloadTime) <
             0)) {
          download_err_t err = download_start();
          if (err != DOWNLOAD_OK) {
            DPRINTF("Error downloading app. Drive to error page.\n");
          }
        }
        break;
      }
      case DOWNLOAD_STATUS_IN_PROGRESS: {
        download_poll();
        break;
      }
      case DOWNLOAD_STATUS_COMPLETED: {
        // Save the app info to the SD card
        download_finish();
        download_confirm();
        download_setStatus(DOWNLOAD_STATUS_IDLE);
        romDownloadUpdate();
        break;
      }
    }
  }
  // 10. Send RESET computer command
  // Exiting the loop means we are done with the setup/configuration mode and we
  // are ready to start the ROM emulation or the booster app.

  // We must reset the computer
  SEND_COMMAND_TO_DISPLAY(DISPLAY_COMMAND_RESET);
  sleep_ms(SLEEP_LOOP_MS);
  if (getResetDevice()) {
    // Reset the device
    reset_device();
  } else {
    // Before jumping to the booster app, let's clean the settings
    // Clean the ROM_SELECTED setting
    settings_put_string(aconfig_getContext(), ACONFIG_PARAM_ROM_SELECTED, "");
    // Set the ROM emulation mode to 255 (setup menu)
    settings_put_integer(aconfig_getContext(), ACONFIG_PARAM_ROM_MODE,
                         ROM_MODE_SETUP);
    settings_save(aconfig_getContext(), true);

    // Jump to the booster app
    reset_jump_to_booster();
  }
}