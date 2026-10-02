// units: rp/src/catalog.c
/* The ROM catalog (rp/src/catalog.c), read page by page from files in
 * memory: the page index, page and entry reads across page boundaries, the
 * CSV forms the parser must take (quotes, doubled quotes, CRLF, no final
 * newline, a line of any length), the lines it must skip, and the checks an
 * entry passes before a download. */
#include <stdlib.h>
#include <string.h>

#include "catalog.h"
#include "ff.h"
#include "test.h"

// The card: one file.
static const char *cardPath = "/roms/roms.csv";
static char *cardData;
static FSIZE_t cardSize;

// When set, what every f_open answers.
static FRESULT openResult = FR_OK;

FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
  (void)mode;
  if (openResult != FR_OK) {
    return openResult;
  }
  if (cardData == NULL || strcmp(path, cardPath) != 0) {
    return FR_NO_FILE;
  }
  fp->data = (const BYTE *)cardData;
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

static void putCard(const char *text) {
  free(cardData);
  cardSize = (FSIZE_t)strlen(text);
  cardData = malloc(cardSize + 1);
  memcpy(cardData, text, cardSize + 1);
}

#define HEADER "\"URL\",\"Name\",\"Description\",\"Tags\",\"Size (KB)\"\n"

// A catalog of n entries "rom-NNNN.img","ROM NNNN","...","t","64".
static char *makeCatalog(int n, const char *eol) {
  size_t cap = 64 + (size_t)n * 64;
  char *text = malloc(cap);
  size_t len = (size_t)snprintf(text, cap, "%s", HEADER);
  for (int i = 0; i < n; i++) {
    len += (size_t)snprintf(
        text + len, cap - len,
        "\"rom-%04d.img\",\"ROM %04d\",\"d\",\"t\",\"64\"%s", i, i, eol);
  }
  return text;
}

int main(void) {
  catalog_t cat = {0};
  char names[CATALOG_PAGE_ENTRIES][CATALOG_SHOWN_BYTES];
  catalog_entry_t e;

  // No file.
  CHECK_EQ(catalog_open(&cat, "/roms/none.csv"), CATALOG_NOT_FOUND);
  CHECK_EQ(cat.count, 0);

  // No heap for FatFs's long-name buffer: out of memory, not a read error.
  putCard(HEADER);
  openResult = FR_NOT_ENOUGH_CORE;
  CHECK_EQ(catalog_open(&cat, cardPath), CATALOG_NO_MEMORY);
  openResult = FR_OK;

  // A header alone: no entries, no pages.
  putCard(HEADER);
  CHECK_EQ(catalog_open(&cat, cardPath), CATALOG_OK);
  CHECK_EQ(cat.count, 0);
  CHECK_EQ(catalog_readPage(&cat, 0, names, CATALOG_PAGE_ENTRIES), 0);

  // One entry, no final newline.
  putCard(HEADER
          "\"Buggy%20Boy.img\",\"Buggy Boy\",\"A game.\",\"game\",\"128\"");
  CHECK_EQ(catalog_open(&cat, cardPath), CATALOG_OK);
  CHECK_EQ(cat.count, 1);
  CHECK_EQ(catalog_readPage(&cat, 0, names, CATALOG_PAGE_ENTRIES), 1);
  CHECK(strcmp(names[0], "Buggy Boy") == 0);
  CHECK_EQ(catalog_readEntry(&cat, 0, &e), CATALOG_OK);
  CHECK(strcmp(e.url, "Buggy Boy.img") == 0);
  CHECK(strcmp(catalog_fileName(&e), "Buggy Boy.img") == 0);
  CHECK(strcmp(e.description, "A game.") == 0);
  CHECK_EQ(e.sizeKb, 128);

  // 40 entries with CRLF: 16, 16 and 8 a page; every entry where it should be.
  char *text = makeCatalog(40, "\r\n");
  putCard(text);
  free(text);
  CHECK_EQ(catalog_open(&cat, cardPath), CATALOG_OK);
  CHECK_EQ(cat.count, 40);
  CHECK_EQ(cat.pages, 3);
  CHECK_EQ(catalog_readPage(&cat, 0, names, CATALOG_PAGE_ENTRIES), 16);
  CHECK(strcmp(names[0], "ROM 0000") == 0);
  CHECK(strcmp(names[15], "ROM 0015") == 0);
  CHECK_EQ(catalog_readPage(&cat, 2, names, CATALOG_PAGE_ENTRIES), 8);
  CHECK(strcmp(names[0], "ROM 0032") == 0);
  CHECK(strcmp(names[7], "ROM 0039") == 0);
  CHECK_EQ(catalog_readPage(&cat, 3, names, CATALOG_PAGE_ENTRIES), 0);
  for (uint32_t i = 0; i < 40; i++) {
    char want[16];
    snprintf(want, sizeof(want), "rom-%04u.img", (unsigned)i);
    CHECK_EQ(catalog_readEntry(&cat, i, &e), CATALOG_OK);
    CHECK(strcmp(e.url, want) == 0);
  }
  CHECK_EQ(catalog_readEntry(&cat, 40, &e), CATALOG_NOT_FOUND);

  // Quotes, doubled quotes, an empty name, a very long line, a URL too long
  // to keep, an empty line: the entries around them stay where they are.
  char longDesc[1001];
  memset(longDesc, 'x', 1000);
  longDesc[1000] = '\0';
  char longUrl[301];
  memset(longUrl, 'u', 300);
  longUrl[300] = '\0';
  size_t cap = 4096;
  text = malloc(cap);
  snprintf(text, cap,
           HEADER
           "\"a.img\",\"Commas, \"\"quotes\"\"\",\"d\",\"t\",\"64\"\n"
           "\"b.img\",\"\",\"no name\",\"t\",\"64\"\n"
           "\"c.img\",\"Long\",\"%s\",\"t\",\"64\"\n"
           "\"%s.img\",\"Too long a URL\",\"d\",\"t\",\"64\"\n"
           "\n"
           "d.img,Plain fields,d,t,32\n",
           longDesc, longUrl);
  putCard(text);
  free(text);
  CHECK_EQ(catalog_open(&cat, cardPath), CATALOG_OK);
  CHECK_EQ(cat.count, 4);
  CHECK_EQ(cat.skipped, 1);
  CHECK_EQ(catalog_readPage(&cat, 0, names, CATALOG_PAGE_ENTRIES), 4);
  CHECK(strcmp(names[0], "Commas, \"quotes\"") == 0);
  CHECK(strcmp(names[1], "b.img") == 0);  // listed by its file name
  CHECK(strcmp(names[2], "Long") == 0);
  CHECK(strcmp(names[3], "Plain fields") == 0);
  CHECK_EQ(catalog_readEntry(&cat, 2, &e), CATALOG_OK);
  CHECK_EQ(strlen(e.description), CATALOG_TEXT_BYTES - 1);  // cut, not lost
  CHECK_EQ(catalog_readEntry(&cat, 3, &e), CATALOG_OK);
  CHECK(strcmp(e.url, "d.img") == 0);
  CHECK_EQ(e.sizeKb, 32);

  // The checks before a download.
  catalog_entry_t c = {.sizeKb = 64};
  snprintf(c.url, sizeof(c.url), "%s", "roms/game.img");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_OK);
  CHECK(strcmp(catalog_fileName(&c), "game.img") == 0);
  snprintf(c.url, sizeof(c.url), "%s", "../..");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "..");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "../escape.img");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "roms/../../escape.img");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "autorun-on/.autorun");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_OK);  // a server folder is fine
  snprintf(c.url, sizeof(c.url), "%s", "game..v2.img");
  CHECK_EQ(catalog_check(&c),
           CATALOG_ENTRY_OK);  // ".." inside a name is not a segment
  snprintf(c.url, sizeof(c.url), "%s", "C:evil.img");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "x\\..\\evil.img");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "dir/");
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_BAD_NAME);
  snprintf(c.url, sizeof(c.url), "%s", "game.stc");
  c.sizeKb = 129;  // a STEEM image: 128 KB and its header
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_OK);
  c.sizeKb = 130;
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_TOO_LARGE);
  c.sizeKb = 64;
  memset(c.url, 'n', 100);
  c.url[100] = '\0';
  CHECK_EQ(catalog_check(&c), CATALOG_ENTRY_LONG_NAME);

  // URLs: decoded for the name and the path, re-encoded for the request.
  char out[64];
  CHECK(catalog_urlDecode("Buggy%20Boy.img", out, sizeof(out)));
  CHECK(strcmp(out, "Buggy Boy.img") == 0);
  CHECK(catalog_urlEncodePath("dir/Buggy Boy (v1).img", out, sizeof(out)));
  CHECK(strcmp(out, "dir/Buggy%20Boy%20%28v1%29.img") == 0);
  CHECK(!catalog_urlEncodePath("a b c d e f g", out, 8));
  CHECK(!catalog_urlDecode("0123456789", out, 4));

  catalog_close(&cat);
  free(cardData);
  TEST_END();
}
