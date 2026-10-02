/**
 * File: romstore.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Writing a ROM file into ROM_TEMP (romstore.h).
 */

#include "romstore.h"

#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "ff.h"

romstore_result_t romstore_checkSize(uint32_t fileBytes, bool steemHeader,
                                     uint32_t *romBytes) {
  uint32_t header = steemHeader ? ROMSTORE_STEEM_HEADER_BYTES : 0U;
  if (fileBytes <= header) {
    return ROMSTORE_EMPTY;
  }
  uint32_t rom = fileBytes - header;
  if (rom > ROMSTORE_MAX_BYTES) {
    return ROMSTORE_TOO_LARGE;
  }
  if (romBytes != NULL) {
    *romBytes = rom;
  }
  return ROMSTORE_OK;
}

// The ST is big-endian and the window serves 16-bit words as the RP stores
// them: every pair of bytes is swapped on the way into flash.
static void swapWords(uint8_t *data, size_t bytes) {
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    uint8_t first = data[i];
    data[i] = data[i + 1];
    data[i + 1] = first;
  }
}

// A STEEM image: a multiple of a sector plus 4 bytes, the first 4 zero. The
// file position is left at the ROM's first byte.
static romstore_result_t readSteemHeader(FIL *file, uint32_t fileBytes,
                                         uint8_t *buffer, bool *steem) {
  *steem = false;
  if (fileBytes <= ROMSTORE_STEEM_HEADER_BYTES ||
      (fileBytes - ROMSTORE_STEEM_HEADER_BYTES) % ROMSTORE_SECTOR_BYTES != 0) {
    return ROMSTORE_OK;
  }
  UINT got = 0;
  if (f_read(file, buffer, ROMSTORE_STEEM_HEADER_BYTES, &got) != FR_OK ||
      got != ROMSTORE_STEEM_HEADER_BYTES) {
    return ROMSTORE_READ_ERROR;
  }
  *steem = buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 0;
  if (!*steem && f_lseek(file, 0) != FR_OK) {
    return ROMSTORE_READ_ERROR;
  }
  return ROMSTORE_OK;
}

static romstore_result_t writeOpenFile(FIL *file, uint32_t flashOffset,
                                       const romstore_flash_t *flash,
                                       uint8_t *buffer, romstore_info_t *info) {
  uint32_t fileBytes = (uint32_t)f_size(file);
  info->fileBytes = fileBytes;
  romstore_result_t result =
      readSteemHeader(file, fileBytes, buffer, &info->steemHeader);
  if (result != ROMSTORE_OK) {
    return result;
  }
  uint32_t romBytes = 0;
  result = romstore_checkSize(fileBytes, info->steemHeader, &romBytes);
  if (result != ROMSTORE_OK) {
    DPRINTF("ROM refused before any erase: %lu bytes (%d)\n",
            (unsigned long)fileBytes, (int)result);
    return result;
  }
  info->romBytes = romBytes;
  const uint32_t total = ROMSTORE_MAX_BYTES + romBytes;

  // The whole of ROM_TEMP first, a sector at a time: what follows a short ROM
  // is erased flash, never the previous ROM's bytes.
  for (uint32_t done = 0; done < ROMSTORE_MAX_BYTES;
       done += ROMSTORE_SECTOR_BYTES) {
    flash->erase(flashOffset + done, ROMSTORE_SECTOR_BYTES);
    flash->tick(done + ROMSTORE_SECTOR_BYTES, total);
  }

  // Then the ROM, a sector at a time, each read back before the next. The
  // loop is bounded by the checked size, whatever the file does meanwhile.
  uint32_t written = 0;
  while (written < romBytes) {
    uint32_t want = romBytes - written;
    if (want > ROMSTORE_SECTOR_BYTES) {
      want = ROMSTORE_SECTOR_BYTES;
    }
    UINT got = 0;
    if (f_read(file, buffer, want, &got) != FR_OK || got != want) {
      DPRINTF("Read %u of %lu bytes at %lu\n", got, (unsigned long)want,
              (unsigned long)written);
      return ROMSTORE_READ_ERROR;
    }
    // A partial last page is padded as erased flash reads.
    size_t padded = ((size_t)got + ROMSTORE_PAGE_BYTES - 1) /
                    ROMSTORE_PAGE_BYTES * ROMSTORE_PAGE_BYTES;
    if (written + padded > ROMSTORE_MAX_BYTES) {
      return ROMSTORE_TOO_LARGE;
    }
    memset(buffer + got, ROMSTORE_ERASED_BYTE, padded - got);
    swapWords(buffer, padded);
    flash->program(flashOffset + written, buffer, padded);
    if (memcmp(flash->read(flashOffset + written), buffer, padded) != 0) {
      DPRINTF("Flash differs from the file in the sector at %lu\n",
              (unsigned long)written);
      return ROMSTORE_VERIFY_ERROR;
    }
    written += got;
    flash->tick(ROMSTORE_MAX_BYTES + written, total);
  }
  return ROMSTORE_OK;
}

romstore_result_t romstore_write(const char *path, uint32_t flashOffset,
                                 const romstore_flash_t *flash,
                                 romstore_info_t *info) {
  romstore_info_t local;
  if (info == NULL) {
    info = &local;
  }
  memset(info, 0, sizeof(*info));

  FIL file;
  FRESULT res = f_open(&file, path, FA_READ);
  if (res == FR_NO_FILE || res == FR_NO_PATH) {
    return ROMSTORE_NOT_FOUND;
  }
  if (res == FR_NOT_ENOUGH_CORE) {
    // FatFs's long-name buffer is on the heap.
    return ROMSTORE_NO_MEMORY;
  }
  if (res != FR_OK) {
    return ROMSTORE_READ_ERROR;
  }
  uint8_t *buffer = (uint8_t *)malloc(ROMSTORE_SECTOR_BYTES);
  if (buffer == NULL) {
    f_close(&file);
    return ROMSTORE_NO_MEMORY;
  }
  romstore_result_t result =
      writeOpenFile(&file, flashOffset, flash, buffer, info);
  free(buffer);
  f_close(&file);
  return result;
}

romstore_result_t romstore_launch(const char *path, uint32_t flashOffset,
                                  const romstore_flash_t *flash,
                                  romstore_info_t *info, void (*select)(void)) {
  romstore_result_t result = romstore_write(path, flashOffset, flash, info);
  if (result == ROMSTORE_OK && select != NULL) {
    select();
  }
  return result;
}

const char *romstore_message(romstore_result_t result) {
  switch (result) {
    case ROMSTORE_OK:
      return "ROM written to flash.";
    case ROMSTORE_NOT_SELECTED:
      return "No ROM selected: choose one with\n[B]rowse or [D]ownload.";
    case ROMSTORE_NOT_FOUND:
      return "The ROM file is not on the SD card.";
    case ROMSTORE_EMPTY:
      return "The ROM file is empty.";
    case ROMSTORE_TOO_LARGE:
      return "The ROM is too large.";
    case ROMSTORE_READ_ERROR:
      return "The ROM file could not be read\nfrom the SD card.";
    case ROMSTORE_VERIFY_ERROR:
      return "The ROM did not read back correctly\nfrom flash.";
    case ROMSTORE_NO_MEMORY:
      return "Not enough memory to write the ROM.";
    default:
      return "The ROM could not be written.";
  }
}
