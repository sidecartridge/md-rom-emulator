/**
 * File: romstore.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Writing a ROM file into ROM_TEMP. The size is checked before
 * anything is erased, the whole area is erased, the ROM is programmed
 * byte-swapped for the big-endian ST, and every sector is read back. The
 * flash is reached through romstore_flash_t, so the host tests can stand in
 * for it.
 */

#ifndef ROMSTORE_H
#define ROMSTORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"

// ROM_TEMP: the two 64 KB banks the ST reads.
#define ROMSTORE_MAX_BYTES (ROM_SIZE_BYTES * ROM_BANKS)
// A STEEM .stc image starts with 4 zero bytes before the ROM.
#define ROMSTORE_STEEM_HEADER_BYTES 4
#define ROMSTORE_SECTOR_BYTES 4096
#define ROMSTORE_PAGE_BYTES 256
// What erased flash reads as, and what pads a partial last page.
#define ROMSTORE_ERASED_BYTE 0xFF

typedef enum {
  ROMSTORE_OK = 0,
  ROMSTORE_NOT_SELECTED,  // no ROM chosen (the caller's check)
  ROMSTORE_NOT_FOUND,     // no such file
  ROMSTORE_EMPTY,         // nothing left after the header
  ROMSTORE_TOO_LARGE,     // more than ROMSTORE_MAX_BYTES of ROM
  ROMSTORE_READ_ERROR,    // the card failed, or the file changed size
  ROMSTORE_VERIFY_ERROR,  // the flash did not read back as written
  ROMSTORE_NO_MEMORY,     // no buffer for a sector
} romstore_result_t;

// The flash as the writer sees it. Offsets count from the start of flash.
// Each erase or program call covers one sector at most, so the caller's
// wrapper keeps interrupts off for no longer than that. read() returns the
// bytes as the CPU reads them back.
typedef struct {
  void (*erase)(uint32_t offset, size_t bytes);
  void (*program)(uint32_t offset, const uint8_t *data, size_t bytes);
  const uint8_t *(*read)(uint32_t offset);
  // Between sectors: answer the ST, see SELECT, show progress.
  void (*tick)(uint32_t done, uint32_t total);
} romstore_flash_t;

// What the writer found, for the caller's message.
typedef struct {
  uint32_t fileBytes;  // the file's size
  uint32_t romBytes;   // the ROM's, past a STEEM header
  bool steemHeader;
} romstore_info_t;

// The bytes of ROM in a file of fileBytes, past a STEEM header when it has
// one: ROMSTORE_OK, ROMSTORE_EMPTY or ROMSTORE_TOO_LARGE.
romstore_result_t romstore_checkSize(uint32_t fileBytes, bool steemHeader,
                                     uint32_t *romBytes);

// Writes the file at path into ROM_TEMP, at flashOffset from the start of
// flash. Nothing is erased unless the size check passes. info may be NULL.
romstore_result_t romstore_write(const char *path, uint32_t flashOffset,
                                 const romstore_flash_t *flash,
                                 romstore_info_t *info);

// romstore_write, then select() only when the ROM was written and read back
// correctly: the selection (EMULATED, MODE) is saved after the verify, so a
// device that loses power during the write boots into the setup menu.
romstore_result_t romstore_launch(const char *path, uint32_t flashOffset,
                                  const romstore_flash_t *flash,
                                  romstore_info_t *info, void (*select)(void));

// One line for the user, without the sizes.
const char *romstore_message(romstore_result_t result);

#endif  // ROMSTORE_H
