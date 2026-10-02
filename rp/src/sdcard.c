#include "sdcard.h"

#include "diskio.h"
#include "health.h"

static FATFS *mountedFsPtr = NULL;
static bool sdMounted = false;

// Remount, from md-devops' sdcard.c (v1.1.0). The card is mounted once at
// boot, so a card pulled and put back stayed dead until a reset: FatFs keeps
// the volume registered, and with card-detect disabled on this board nothing
// ever marks the drive uninitialised, so mount_volume() sees a mounted volume
// and never asks the driver to re-initialise the card. Unregistering the
// volume is what breaks that: with fs_type cleared, the next f_mount() runs
// disk_initialize(), which calls the card's own init unconditionally
// (fatfs-sdk src/glue.c), and a freshly inserted card comes up.
static FATFS *bootFsPtr = NULL;
static char bootFolder[SDCARD_FOLDER_NAME_MAX] = "";
static absolute_time_t nextRemountAt;
static bool remountScheduled = false;
static uint32_t remountAttempts = 0;
static uint32_t remountRecoveries = 0;
static FRESULT lastMountResult = FR_OK;

#if defined(_DEBUG) && (_DEBUG != 0)
// Debug builds only: the card looks pulled while set (the presence check
// fails and nothing remounts), and is found again once cleared. The same path
// a real pull and reinsertion take.
static bool sdcardTestRemoved = false;
void sdcard_testSetRemoved(bool removed) { sdcardTestRemoved = removed; }
#else
static const bool sdcardTestRemoved = false;
#endif

static void sdcard_warnDebugRisk(void) {
  size_t sdCount = sd_get_num();
  for (size_t i = 0; i < sdCount; i++) {
    sd_card_t *sdCard = sd_get_by_num(i);
    if ((sdCard != NULL) && !sdCard->use_card_detect) {
      DPRINTF(
          "WARNING: SD card-detect disabled on slot %u. "
          "When debugging, starting without an SD card can trigger assertions "
          "during init.\n",
          (unsigned)i);
    }
  }
}

static sdcard_status_t sdcardInit() {
  DPRINTF("Initializing SD card...\n");
  sdcard_warnDebugRisk();
  // Initialize the SD card
  bool success = sd_init_driver();
  if (!success) {
    DPRINTF("ERROR: Could not initialize SD card\r\n");
    return SDCARD_INIT_ERROR;
  }
  DPRINTF("SD card initialized.\n");

  sdcard_setSpiSpeedSettings();
  return SDCARD_INIT_OK;
}

FRESULT sdcard_mountFilesystem(FATFS *fsys, const char *drive) {
  // Mount the drive
  FRESULT fres = f_mount(fsys, drive, 1);
  lastMountResult = fres;
  if (fres != FR_OK) {
    DPRINTF("ERROR: Could not mount the filesystem. Error code: %d\n", fres);
  } else {
    DPRINTF("Filesystem mounted.\n");
  }
  return fres;
}

bool sdcard_dirExist(const char *dir) {
  FILINFO fno;
  FRESULT res = f_stat(dir, &fno);

  // Check if the result is OK and if the attribute indicates it's a directory
  bool dirExist = (res == FR_OK && (fno.fattrib & AM_DIR));
  DPRINTF("Directory %s exists: %s\n", dir, dirExist ? "true" : "false");
  return dirExist;
}

sdcard_status_t sdcard_ensureFolder(const char *folderName) {
  if ((folderName == NULL) || (folderName[0] == '\0') ||
      (strcmp(folderName, "/") == 0)) {
    DPRINTF("Empty or root folder name. Ignoring.\n");
    return SDCARD_INIT_OK;
  }

  bool folderExists = sdcard_dirExist(folderName);
  DPRINTF("Folder exists: %s\n", folderExists ? "true" : "false");
  if (folderExists) {
    return SDCARD_INIT_OK;
  }

  FRESULT fres = f_mkdir(folderName);
  if (fres != FR_OK) {
    DPRINTF("Error creating the folder.\n");
    return SDCARD_CREATE_FOLDER_ERROR;
  }
  DPRINTF("Folder created.\n");
  return SDCARD_INIT_OK;
}

sdcard_status_t sdcard_initFilesystem(FATFS *fsPtr, const char *folderName) {
  sdMounted = false;
  mountedFsPtr = NULL;

  if ((fsPtr == NULL) || (folderName == NULL) || (folderName[0] == '\0')) {
    DPRINTF("Invalid SD filesystem initialization arguments.\n");
    return SDCARD_INIT_ERROR;
  }

  // Remember what to mount with before trying, not after succeeding: a device
  // booted with no card has to keep retrying too, and that is the case where
  // the retry matters most.
  bootFsPtr = fsPtr;
  snprintf(bootFolder, sizeof(bootFolder), "%s", folderName);

  // Check the status of the sd card
  sdcard_status_t sdcardOk = sdcardInit();
  if (sdcardOk != SDCARD_INIT_OK) {
    DPRINTF("Error initializing the SD card.\n");
    return SDCARD_INIT_ERROR;
  }

  // Now try to mount the filesystem
  FRESULT fres;
  fres = sdcard_mountFilesystem(fsPtr, "0:");
  if (fres != FR_OK) {
    DPRINTF("Error mounting the filesystem.\n");
    return SDCARD_MOUNT_ERROR;
  }
  DPRINTF("Filesystem mounted.\n");

  sdcard_status_t folderStatus = sdcard_ensureFolder(folderName);
  if (folderStatus != SDCARD_INIT_OK) {
    return folderStatus;
  }

  mountedFsPtr = fsPtr;
  sdMounted = true;
  return SDCARD_INIT_OK;
}

void sdcard_changeSpiSpeed(int baudRateKbits) {
  size_t sdNum = sd_get_num();
  if (sdNum > 0) {
    int baudRate = baudRateKbits;
    if (baudRate > 0) {
      DPRINTF("Changing SD card baud rate to %i\n", baudRate);
      sd_card_t *sdCard = sd_get_by_num(sdNum - 1);
      if ((sdCard == NULL) || (sdCard->spi_if_p == NULL) ||
          (sdCard->spi_if_p->spi == NULL)) {
        DPRINTF("SD card SPI interface is not available\n");
        return;
      }
      sdCard->spi_if_p->spi->baud_rate = baudRate * SDCARD_KILOBAUD;
    } else {
      DPRINTF("Invalid baud rate. Using default value\n");
    }
  } else {
    DPRINTF("SD card not found\n");
  }
}

void sdcard_setSpiSpeedSettings() {
  // Get the SPI speed from the configuration
  SettingsConfigEntry *spiSpeed =
      settings_find_entry(gconfig_getContext(), PARAM_SD_BAUD_RATE_KB);
  int baudRate = 0;
  if (spiSpeed != NULL) {
    baudRate = atoi(spiSpeed->value);
  }

  // Clamp to a sane range; PARAM_SD_BAUD_RATE_KB is just a string in
  // shared config and a stale/typoed value (e.g. 999999) would otherwise
  // ask the SPI driver to clock past what the hardware sustains.
  if (baudRate > SDCARD_MAX_KHZ) {
    DPRINTF("Baud rate too high. Clamping to %d KHz\n", SDCARD_MAX_KHZ);
    baudRate = SDCARD_MAX_KHZ;
  }
  if (baudRate < SDCARD_MIN_KHZ) {
    DPRINTF("Baud rate too low. Clamping to %d KHz\n", SDCARD_MIN_KHZ);
    baudRate = SDCARD_MIN_KHZ;
  }

  sdcard_changeSpiSpeed(baudRate);
}

void sdcard_getInfo(FATFS *fsPtr, uint32_t *totalSizeMb,
                    uint32_t *freeSpaceMb) {
  if ((fsPtr == NULL) || (totalSizeMb == NULL) || (freeSpaceMb == NULL)) {
    DPRINTF("Invalid SD card info arguments.\n");
    return;
  }

  DWORD freClust;

  // Set initial values to zero as a precaution
  *totalSizeMb = 0;
  *freeSpaceMb = 0;

  // Get volume information and free clusters of drive
  FRESULT res = f_getfree("", &freClust, &fsPtr);
  if (res != FR_OK) {
    DPRINTF("Error getting free space information: %d\n", res);
    return;  // Error handling: Set values to zero if getfree fails
  }

  // Calculate total sectors in the SD card
  uint64_t totalSectors = (fsPtr->n_fatent - 2) * fsPtr->csize;

  // Convert total sectors to bytes and then to megabytes
  *totalSizeMb = (totalSectors * NUM_BYTES_PER_SECTOR) / SDCARD_MEGABYTE;

  // Convert free clusters to sectors and then to bytes
  uint64_t freeSpaceBytes =
      (uint64_t)freClust * fsPtr->csize * NUM_BYTES_PER_SECTOR;

  // Convert bytes to megabytes
  *freeSpaceMb = freeSpaceBytes / SDCARD_MEGABYTE;
}

bool sdcard_isMounted(void) { return sdMounted && (mountedFsPtr != NULL); }

uint32_t sdcard_getRemountRecoveries(void) { return remountRecoveries; }

static void sdcard_markGone(const char *why) {
  DPRINTF("SD card: %s -- card gone\n", why);
  sdMounted = false;
  mountedFsPtr = NULL;
  remountScheduled = true;
  nextRemountAt = get_absolute_time();
}

// Whether the card is still there cannot be inferred from FatFs results: with
// the card physically out FatFs serves what it has cached and never reports a
// disk error, and card-detect is not wired on this board. The only honest
// question is a real one: read sector 0 off the card, bypassing FatFs's cache.
static uint8_t presenceBuf[NUM_BYTES_PER_SECTOR];

bool sdcard_checkPresence(void) {
  if (!sdcard_isMounted()) {
    return false;
  }
  if (sdcardTestRemoved || disk_read(0, presenceBuf, 0, 1) != RES_OK) {
    sdcard_markGone("sector 0 read failed");
    return false;
  }
  return true;
}

static absolute_time_t nextPresenceCheckAt;
static bool presenceCheckStarted = false;

static void sdcard_pollPresence(void) {
  absolute_time_t now = get_absolute_time();
  if (!presenceCheckStarted) {
    presenceCheckStarted = true;
    nextPresenceCheckAt = delayed_by_ms(now, SDCARD_PRESENCE_POLL_MS);
    return;
  }
  if (absolute_time_diff_us(now, nextPresenceCheckAt) > 0) {
    return;
  }
  nextPresenceCheckAt = delayed_by_ms(now, SDCARD_PRESENCE_POLL_MS);
  sdcard_checkPresence();
}

// This board has no chip-select line (hw_config.c: ss_gpio = -1), so the card
// is always selected. An RP restart in the middle of a multi-block write (a
// SELECT press during a download, a crash, the watchdog) leaves the card
// waiting for data or a stop token; it takes the driver's reset clocks and
// CMD0 as data, and never mounts again until it loses power. Finishing the
// block, sending the stop token and clocking it through its busy phase frees
// it (found and proven over the probe).
#define SDCARD_UNSTICK_BYTES 1200U
#define SDCARD_STOP_TRAN_TOKEN 0xFDU

static void sdcard_unstick(void) {
  sd_card_t *sdCard = sd_get_by_num(0);
  if ((sdCard == NULL) || (sdCard->spi_if_p == NULL) ||
      (sdCard->spi_if_p->spi == NULL)) {
    return;
  }
  DPRINTF("SD card: not ready; ending a write it may be stuck in\n");
  spi_t *spi = sdCard->spi_if_p->spi;
  static const uint8_t stopToken = SDCARD_STOP_TRAN_TOKEN;
  spi_lock(spi);
  spi_transfer(spi, NULL, NULL, SDCARD_UNSTICK_BYTES);  // 0xFF: the block
  spi_transfer(spi, &stopToken, NULL, 1);
  spi_transfer(spi, NULL, NULL, SDCARD_UNSTICK_BYTES);  // 0xFF: busy
  spi_unlock(spi);
}

// One mount from scratch: the driver forgotten (sd_card_spi_init() returns at
// once unless STA_NOINIT is set, and nothing sets it when a card is pulled),
// the volume unregistered, then the card brought up again. A card stuck
// inside a write answers FR_NOT_READY: freed and tried once more.
static sdcard_status_t sdcard_mountAgain(void) {
  for (int attempt = 0; attempt < 2; attempt++) {
    sd_card_t *sdCard = sd_get_by_num(0);
    if ((sdCard != NULL) && (sdCard->deinit != NULL)) {
      sdCard->deinit(sdCard);
    }
    f_mount(NULL, "0:", 0);
    sdcard_status_t status = sdcard_initFilesystem(bootFsPtr, bootFolder);
    health_feed();
    if (status == SDCARD_INIT_OK || lastMountResult != FR_NOT_READY ||
        attempt > 0) {
      return status;
    }
    sdcard_unstick();
  }
  return SDCARD_MOUNT_ERROR;
}

sdcard_status_t sdcard_recoverAtBoot(sdcard_status_t status) {
  if ((status != SDCARD_MOUNT_ERROR) || (lastMountResult != FR_NOT_READY) ||
      (bootFsPtr == NULL)) {
    return status;
  }
  sdcard_unstick();
  return sdcard_mountAgain();
}

void sdcard_pollRemount(void) {
  if (sdcard_isMounted()) {
    sdcard_pollPresence();
    return;
  }
  if ((bootFsPtr == NULL) || (bootFolder[0] == '\0') || sdcardTestRemoved) {
    return;
  }
  if (!remountScheduled) {
    remountScheduled = true;
    nextRemountAt = get_absolute_time();
  }
  if (absolute_time_diff_us(get_absolute_time(), nextRemountAt) > 0) {
    return;
  }
  // A remount talks to the card over SPI and can take a moment, and a card
  // that is half-inserted can take longer still.
  health_feed();
  health_setPhase(HEALTH_PHASE_SD_CARD);
  remountAttempts++;
  if (sdcard_mountAgain() == SDCARD_INIT_OK) {
    remountRecoveries++;
    presenceCheckStarted = false;
    DPRINTF("SD card: mounted after %lu attempt(s)\n",
            (unsigned long)remountAttempts);
    remountAttempts = 0;
    remountScheduled = false;
    return;
  }
  nextRemountAt = delayed_by_ms(get_absolute_time(), SDCARD_REMOUNT_RETRY_MS);
}

bool sdcard_getMountedInfo(uint32_t *totalSizeMb, uint32_t *freeSpaceMb) {
  if ((totalSizeMb == NULL) || (freeSpaceMb == NULL)) {
    return false;
  }

  *totalSizeMb = 0;
  *freeSpaceMb = 0;

  if (!sdcard_isMounted()) {
    return false;
  }

  FATFS *fs = mountedFsPtr;
  DWORD freeClusters = 0;
  FRESULT res = f_getfree("", &freeClusters, &fs);
  if ((res != FR_OK) || (fs == NULL)) {
    DPRINTF("Error getting mounted free space information: %d\n", res);
    return false;
  }

  uint64_t totalSectors = (uint64_t)(fs->n_fatent - 2U) * fs->csize;
  *totalSizeMb =
      (uint32_t)((totalSectors * NUM_BYTES_PER_SECTOR) / SDCARD_MEGABYTE);

  uint64_t freeSpaceBytes =
      (uint64_t)freeClusters * fs->csize * NUM_BYTES_PER_SECTOR;
  *freeSpaceMb = (uint32_t)(freeSpaceBytes / SDCARD_MEGABYTE);
  return true;
}
