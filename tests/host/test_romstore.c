// units: rp/src/romstore.c
/* Writing a ROM into ROM_TEMP (rp/src/romstore.c), over files in memory and
 * a fake NOR flash: erase sets 0xFF, program can only clear bits. Every erase
 * and program is logged and must lie inside ROM_TEMP; the selection is saved
 * only after the last sector was read back. */
#include "romstore.h"

#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "test.h"

#define FLASH_BYTES (2u * 1024u * 1024u)
#define ROM_TEMP FLASH_ROM_LOAD_OFFSET

static uint8_t flash[FLASH_BYTES];

enum { OP_ERASE, OP_PROGRAM, OP_SELECT };
typedef struct {
  int op;
  uint32_t offset;
  size_t bytes;
} op_t;
static op_t ops[256];
static int opCount;
static int ticks;
static uint32_t corruptAt = UINT32_MAX;  // a program that does not take

static void logOp(int op, uint32_t offset, size_t bytes) {
  if (opCount < (int)(sizeof(ops) / sizeof(ops[0]))) {
    ops[opCount++] = (op_t){op, offset, bytes};
  }
}

static void fakeErase(uint32_t offset, size_t bytes) {
  logOp(OP_ERASE, offset, bytes);
  memset(flash + offset, 0xFF, bytes);
}

static void fakeProgram(uint32_t offset, const uint8_t *data, size_t bytes) {
  logOp(OP_PROGRAM, offset, bytes);
  for (size_t i = 0; i < bytes; i++) {
    flash[offset + i] &= data[i];
  }
  if (corruptAt >= offset && corruptAt < offset + bytes) {
    flash[corruptAt] ^= 0x01;
  }
}

static const uint8_t *fakeRead(uint32_t offset) { return flash + offset; }

static void fakeTick(uint32_t done, uint32_t total) {
  (void)done;
  (void)total;
  ticks++;
}

static const romstore_flash_t fake = {fakeErase, fakeProgram, fakeRead,
                                      fakeTick};

static void select(void) { logOp(OP_SELECT, 0, 0); }

// The card: one file at a time.
static const char *cardPath;
static uint8_t *cardData;
static FSIZE_t cardSize;

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)mode;
  if (cardPath == NULL || strcmp(path, cardPath) != 0) {
    return FR_NO_FILE;
  }
  fp->data = cardData;
  fp->size = cardSize;
  fp->pos = 0;
  return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
  FSIZE_t left = fp->size - fp->pos;
  UINT n = btr < left ? btr : (UINT)left;
  memcpy(buff, fp->data + fp->pos, n);
  fp->pos += n;
  *br = n;
  return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs) {
  fp->pos = ofs;
  return FR_OK;
}

FRESULT f_close(FIL *fp) {
  (void)fp;
  return FR_OK;
}

static void reset(void) {
  memset(flash, 0x5A, sizeof(flash));  // the previous ROM, everywhere
  opCount = 0;
  ticks = 0;
  corruptAt = UINT32_MAX;
}

static void putFile(const char *path, uint32_t bytes, bool steemHeader) {
  free(cardData);
  cardPath = path;
  cardSize = bytes;
  cardData = malloc(bytes ? bytes : 1);
  for (uint32_t i = 0; i < bytes; i++) {
    cardData[i] = (uint8_t)(i * 7 + 3);
  }
  if (steemHeader && bytes >= 4) {
    memset(cardData, 0, 4);
  }
}

// Every logged erase and program is inside ROM_TEMP; erases are whole
// sectors, programs whole pages.
static void checkOpsInside(void) {
  for (int i = 0; i < opCount; i++) {
    if (ops[i].op == OP_SELECT) {
      continue;
    }
    CHECK(ops[i].offset >= ROM_TEMP);
    CHECK(ops[i].offset + ops[i].bytes <= ROM_TEMP + ROMSTORE_MAX_BYTES);
    if (ops[i].op == OP_ERASE) {
      CHECK_EQ(ops[i].offset % ROMSTORE_SECTOR_BYTES, 0);
      CHECK_EQ(ops[i].bytes, ROMSTORE_SECTOR_BYTES);
    } else {
      CHECK_EQ(ops[i].offset % ROMSTORE_PAGE_BYTES, 0);
      CHECK_EQ(ops[i].bytes % ROMSTORE_PAGE_BYTES, 0);
    }
  }
}

// ROM_TEMP holds the ROM byte-swapped, then erased flash to the end.
static void checkWindow(uint32_t romFrom, uint32_t romBytes) {
  int bad = 0;
  for (uint32_t i = 0; i < ROMSTORE_MAX_BYTES; i++) {
    uint8_t want;
    uint32_t pair = i ^ 1u;  // the byte the swap brings here
    if (pair < romBytes) {
      want = cardData[romFrom + pair];
    } else {
      want = 0xFF;
    }
    if (flash[ROM_TEMP + i] != want && bad++ == 0) {
      fprintf(stderr, "ROM_TEMP+%u is 0x%02x, expected 0x%02x\n", i,
              flash[ROM_TEMP + i], want);
    }
  }
  CHECK_EQ(bad, 0);
}

static int count(int op) {
  int n = 0;
  for (int i = 0; i < opCount; i++) {
    n += ops[i].op == op;
  }
  return n;
}

int main(void) {
  uint32_t rom = 0;

  // The size check alone.
  CHECK_EQ(romstore_checkSize(0, false, &rom), ROMSTORE_EMPTY);
  CHECK_EQ(romstore_checkSize(4, true, &rom), ROMSTORE_EMPTY);
  CHECK_EQ(romstore_checkSize(1, false, &rom), ROMSTORE_OK);
  CHECK_EQ(rom, 1);
  CHECK_EQ(romstore_checkSize(131072, false, &rom), ROMSTORE_OK);
  CHECK_EQ(rom, 131072);
  CHECK_EQ(romstore_checkSize(131076, true, &rom), ROMSTORE_OK);
  CHECK_EQ(rom, 131072);
  CHECK_EQ(romstore_checkSize(131076, false, &rom), ROMSTORE_TOO_LARGE);
  CHECK_EQ(romstore_checkSize(131073, false, &rom), ROMSTORE_TOO_LARGE);
  CHECK_EQ(romstore_checkSize(196608, false, &rom), ROMSTORE_TOO_LARGE);

  romstore_info_t info;

  // Too large: refused before anything is erased.
  reset();
  putFile("/roms/big.img", 196608, false);
  CHECK_EQ(romstore_launch("/roms/big.img", ROM_TEMP, &fake, &info, select),
           ROMSTORE_TOO_LARGE);
  CHECK_EQ(info.fileBytes, 196608);
  CHECK_EQ(opCount, 0);
  CHECK_EQ(flash[ROM_TEMP], 0x5A);

  // Empty, missing: nothing touched either.
  reset();
  putFile("/roms/empty.bin", 0, false);
  CHECK_EQ(romstore_write("/roms/empty.bin", ROM_TEMP, &fake, &info),
           ROMSTORE_EMPTY);
  CHECK_EQ(romstore_write("/roms/gone.img", ROM_TEMP, &fake, &info),
           ROMSTORE_NOT_FOUND);
  CHECK_EQ(opCount, 0);

  // 64 KB: the whole area erased first, the ROM programmed, the rest 0xFF,
  // then the selection.
  reset();
  putFile("/roms/pattern-64k.img", 65536, false);
  CHECK_EQ(romstore_launch("/roms/pattern-64k.img", ROM_TEMP, &fake, &info,
                           select),
           ROMSTORE_OK);
  CHECK_EQ(info.romBytes, 65536);
  CHECK_EQ(count(OP_ERASE), ROMSTORE_MAX_BYTES / ROMSTORE_SECTOR_BYTES);
  CHECK_EQ(count(OP_PROGRAM), 65536 / ROMSTORE_SECTOR_BYTES);
  for (int i = 0; i < 32; i++) {
    CHECK_EQ(ops[i].op, OP_ERASE);
  }
  CHECK_EQ(ops[opCount - 1].op, OP_SELECT);
  CHECK_EQ(ticks, 32 + 16);
  checkOpsInside();
  checkWindow(0, 65536);

  // A STEEM image: the 4-byte header skipped, 128 KB of ROM.
  reset();
  putFile("/roms/PATTERN-128K.STC", 131076, true);
  CHECK_EQ(romstore_launch("/roms/PATTERN-128K.STC", ROM_TEMP, &fake, &info,
                           select),
           ROMSTORE_OK);
  CHECK(info.steemHeader);
  CHECK_EQ(info.romBytes, 131072);
  checkOpsInside();
  checkWindow(4, 131072);

  // The same size without a zero header is too large.
  reset();
  putFile("/roms/notsteem.bin", 131076, false);
  CHECK_EQ(romstore_write("/roms/notsteem.bin", ROM_TEMP, &fake, &info),
           ROMSTORE_TOO_LARGE);
  CHECK_EQ(opCount, 0);

  // An odd length: the last page padded as erased flash.
  reset();
  putFile("/roms/odd-40001.IMG", 40001, false);
  CHECK_EQ(romstore_write("/roms/odd-40001.IMG", ROM_TEMP, &fake, &info),
           ROMSTORE_OK);
  checkOpsInside();
  checkWindow(0, 40001);

  // A sector that does not read back: an error, and no selection.
  reset();
  putFile("/roms/pattern-128k.rom", 131072, false);
  corruptAt = ROM_TEMP + 70000;
  CHECK_EQ(romstore_launch("/roms/pattern-128k.rom", ROM_TEMP, &fake, &info,
                           select),
           ROMSTORE_VERIFY_ERROR);
  CHECK_EQ(count(OP_SELECT), 0);
  checkOpsInside();

  free(cardData);
  TEST_END();
}
