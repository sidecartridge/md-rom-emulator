/**
 * File: catalog.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The ROM catalog, read from the SD card a page at a time
 * (catalog.h).
 */

#include "catalog.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "ff.h"

// The URL field as the file has it, before decoding: up to 3 bytes per
// character.
#define CATALOG_RAW_URL_BYTES (CATALOG_URL_BYTES * 3)
#define CATALOG_SIZE_BYTES 12
#define CATALOG_READ_BYTES 128
// The size column's limit: 128 KB, and 129 for a STEEM image's 4-byte header
// rounded up. romstore checks the real size at launch.
#define CATALOG_MAX_SIZE_KB 129

// A buffered reader over the file that knows the offset of every byte.
typedef struct {
  FIL file;
  uint8_t buffer[CATALOG_READ_BYTES];
  UINT length;
  UINT position;
  uint32_t offset;  // of the next byte
  bool end;
} reader_t;

static bool readerOpen(reader_t *reader, const char *path, uint32_t offset,
                       catalog_result_t *result) {
  memset(reader, 0, sizeof(*reader));
  FRESULT res = f_open(&reader->file, path, FA_READ);
  if (res != FR_OK) {
    if (res == FR_NO_FILE || res == FR_NO_PATH) {
      *result = CATALOG_NOT_FOUND;
    } else if (res == FR_NOT_ENOUGH_CORE) {
      *result = CATALOG_NO_MEMORY;  // no heap for FatFs's long-name buffer
    } else {
      *result = CATALOG_READ_ERROR;
    }
    return false;
  }
  if (offset != 0U && f_lseek(&reader->file, offset) != FR_OK) {
    f_close(&reader->file);
    *result = CATALOG_READ_ERROR;
    return false;
  }
  reader->offset = offset;
  *result = CATALOG_OK;
  return true;
}

static int readerGet(reader_t *reader) {
  if (reader->position >= reader->length) {
    if (reader->end) {
      return -1;
    }
    if (f_read(&reader->file, reader->buffer, sizeof(reader->buffer),
               &reader->length) != FR_OK ||
        reader->length == 0) {
      reader->end = true;
      reader->length = 0;
      return -1;
    }
    reader->position = 0;
  }
  reader->offset++;
  return reader->buffer[reader->position++];
}

// One field being filled: cut at its size, and marked when it was.
typedef struct {
  char *text;
  size_t size;
  size_t length;
  bool cut;
} field_t;

static void fieldPut(field_t *field, int c) {
  if (field->length + 1 < field->size) {
    field->text[field->length++] = (char)c;
    field->text[field->length] = '\0';
  } else {
    field->cut = true;
  }
}

// Reads one line, of any length, into up to count fields. Quoted fields may
// hold commas, newlines and "" for a quote; a CR outside quotes is dropped.
// Returns the number of fields seen (0 for an empty line), or -1 at the end
// of the file.
static int readLine(reader_t *reader, field_t *fields, int count) {
  for (int i = 0; i < count; i++) {
    fields[i].length = 0;
    fields[i].cut = false;
    fields[i].text[0] = '\0';
  }
  int c = readerGet(reader);
  if (c < 0) {
    return -1;
  }
  int index = 0;
  bool seen = false;
  bool atStart = true;
  bool quoted = false;
  bool inQuotes = false;
  while (c >= 0 && (c != '\n' || inQuotes)) {
    field_t *field = (index < count) ? &fields[index] : NULL;
    if (inQuotes) {
      if (c == '"') {
        c = readerGet(reader);
        if (c == '"') {
          if (field != NULL) {
            fieldPut(field, '"');
          }
          c = readerGet(reader);
        } else {
          inQuotes = false;
        }
        continue;
      }
      if (field != NULL) {
        fieldPut(field, c);
      }
    } else if (c == ',') {
      index++;
      atStart = true;
      quoted = false;
    } else if (c == '\r') {
      // dropped
    } else if (atStart && (c == ' ' || c == '\t')) {
      // leading blanks
    } else if (atStart && c == '"') {
      quoted = true;
      inQuotes = true;
      atStart = false;
      seen = true;
    } else {
      atStart = false;
      seen = true;
      // After a closing quote, anything before the comma is ignored.
      if (!quoted && field != NULL) {
        fieldPut(field, c);
      }
    }
    c = readerGet(reader);
  }
  if (!seen && index == 0) {
    return 0;
  }
  return index + 1;
}

bool catalog_urlDecode(const char *in, char *out, size_t outSize) {
  size_t length = 0;
  for (size_t i = 0; in[i] != '\0'; i++) {
    char c = in[i];
    if (c == '%' && isxdigit((unsigned char)in[i + 1]) &&
        isxdigit((unsigned char)in[i + 2])) {
      char hex[3] = {in[i + 1], in[i + 2], '\0'};
      c = (char)strtol(hex, NULL, 16);
      i += 2;
    } else if (c == '+') {
      c = ' ';
    }
    if (length + 1 >= outSize) {
      out[length] = '\0';
      return false;
    }
    out[length++] = c;
  }
  out[length] = '\0';
  return true;
}

bool catalog_urlEncodePath(const char *in, char *out, size_t outSize) {
  static const char hex[] = "0123456789ABCDEF";
  size_t length = 0;
  for (size_t i = 0; in[i] != '\0'; i++) {
    unsigned char c = (unsigned char)in[i];
    bool plain =
        isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
    size_t need = plain ? 1U : 3U;
    if (length + need >= outSize) {
      out[length] = '\0';
      return false;
    }
    if (plain) {
      out[length++] = (char)c;
    } else {
      out[length++] = '%';
      out[length++] = hex[c >> 4];
      out[length++] = hex[c & 0x0F];
    }
  }
  out[length] = '\0';
  return true;
}

// The five fields of a line, raw.
typedef struct {
  char url[CATALOG_RAW_URL_BYTES];
  char name[CATALOG_NAME_BYTES * 3];
  char description[CATALOG_TEXT_BYTES];
  char tags[CATALOG_TAGS_BYTES];
  char size[CATALOG_SIZE_BYTES];
  field_t fields[5];
} line_t;

static void lineInit(line_t *line) {
  char *texts[5] = {line->url, line->name, line->description, line->tags,
                    line->size};
  size_t sizes[5] = {sizeof(line->url), sizeof(line->name),
                     sizeof(line->description), sizeof(line->tags),
                     sizeof(line->size)};
  for (int i = 0; i < 5; i++) {
    line->fields[i] = (field_t){texts[i], sizes[i], 0, false};
  }
}

// The next entry: -1 at the end of the file, 0 for a line that is not one
// (empty, no URL, a URL that does not fit), 1 for an entry, decoded into
// entry when it is not NULL.
static int nextEntry(reader_t *reader, line_t *line, catalog_entry_t *entry) {
  int fields = readLine(reader, line->fields, 5);
  if (fields < 0) {
    return -1;
  }
  if (fields == 0 || line->url[0] == '\0' || line->fields[0].cut) {
    return 0;
  }
  catalog_entry_t local;
  catalog_entry_t *out = (entry != NULL) ? entry : &local;
  if (!catalog_urlDecode(line->url, out->url, sizeof(out->url))) {
    return 0;
  }
  catalog_urlDecode(line->name, out->name, sizeof(out->name));
  catalog_urlDecode(line->description, out->description,
                    sizeof(out->description));
  catalog_urlDecode(line->tags, out->tags, sizeof(out->tags));
  out->sizeKb = (uint32_t)strtoul(line->size, NULL, 10);
  return 1;
}

void catalog_close(catalog_t *cat) {
  free(cat->pageOffsets);
  memset(cat, 0, sizeof(*cat));
}

catalog_result_t catalog_open(catalog_t *cat, const char *path) {
  catalog_close(cat);
  snprintf(cat->path, sizeof(cat->path), "%s", path);
  cat->pageOffsets = (uint32_t *)malloc(CATALOG_MAX_PAGES * sizeof(uint32_t));
  line_t *line = (line_t *)malloc(sizeof(line_t));
  reader_t *reader = (reader_t *)malloc(sizeof(reader_t));
  catalog_result_t result = CATALOG_NO_MEMORY;
  if (cat->pageOffsets != NULL && line != NULL && reader != NULL &&
      readerOpen(reader, path, 0, &result)) {
    lineInit(line);
    readLine(reader, line->fields, 5);  // the header
    for (;;) {
      uint32_t start = reader->offset;
      int found = nextEntry(reader, line, NULL);
      if (found < 0) {
        break;
      }
      if (found == 0) {
        if (line->fields[0].length > 0 || line->fields[0].cut) {
          cat->skipped++;
        }
        continue;
      }
      if (cat->count >= CATALOG_MAX_ENTRIES) {
        cat->skipped++;
        continue;
      }
      if (cat->count % CATALOG_PAGE_ENTRIES == 0) {
        cat->pageOffsets[cat->pages++] = start;
      }
      cat->count++;
    }
    f_close(&reader->file);
    result = CATALOG_OK;
  }
  free(line);
  free(reader);
  if (result != CATALOG_OK) {
    free(cat->pageOffsets);
    cat->pageOffsets = NULL;
    cat->count = 0;
    cat->pages = 0;
  }
  DPRINTF("Catalog %s: %lu entries, %lu lines skipped (%d)\n", path,
          (unsigned long)cat->count, (unsigned long)cat->skipped, (int)result);
  return result;
}

// Reads from page's first entry: skips `skip` entries, then calls take for
// up to max more. Returns how many were taken.
static int readFrom(const catalog_t *cat, uint32_t page, uint32_t skip, int max,
                    void (*take)(int n, const catalog_entry_t *e, void *ctx),
                    void *ctx) {
  if (cat->pageOffsets == NULL || page >= cat->pages || max <= 0) {
    return 0;
  }
  line_t *line = (line_t *)malloc(sizeof(line_t));
  reader_t *reader = (reader_t *)malloc(sizeof(reader_t));
  catalog_entry_t *entry = (catalog_entry_t *)malloc(sizeof(catalog_entry_t));
  int taken = 0;
  catalog_result_t result;
  if (line != NULL && reader != NULL && entry != NULL &&
      readerOpen(reader, cat->path, cat->pageOffsets[page], &result)) {
    lineInit(line);
    while (taken < max) {
      int found = nextEntry(reader, line, entry);
      if (found < 0) {
        break;
      }
      if (found == 0) {
        continue;
      }
      if (skip > 0) {
        skip--;
        continue;
      }
      take(taken++, entry, ctx);
    }
    f_close(&reader->file);
  }
  free(entry);
  free(reader);
  free(line);
  return taken;
}

static void takeName(int n, const catalog_entry_t *e, void *ctx) {
  char (*names)[CATALOG_SHOWN_BYTES] = (char (*)[CATALOG_SHOWN_BYTES])ctx;
  const char *shown = (e->name[0] != '\0') ? e->name : catalog_fileName(e);
  snprintf(names[n], CATALOG_SHOWN_BYTES, "%.*s", CATALOG_SHOWN_BYTES - 1,
           shown);
}

int catalog_readPage(const catalog_t *cat, uint32_t page,
                     char names[][CATALOG_SHOWN_BYTES], int max) {
  uint32_t left =
      (page < cat->pages) ? cat->count - page * CATALOG_PAGE_ENTRIES : 0U;
  if ((uint32_t)max > left) {
    max = (int)left;
  }
  return readFrom(cat, page, 0, max, takeName, names);
}

static void takeEntry(int n, const catalog_entry_t *e, void *ctx) {
  (void)n;
  memcpy(ctx, e, sizeof(*e));
}

catalog_result_t catalog_readEntry(const catalog_t *cat, uint32_t index,
                                   catalog_entry_t *entry) {
  if (index >= cat->count) {
    return CATALOG_NOT_FOUND;
  }
  int got = readFrom(cat, index / CATALOG_PAGE_ENTRIES,
                     index % CATALOG_PAGE_ENTRIES, 1, takeEntry, entry);
  return (got == 1) ? CATALOG_OK : CATALOG_READ_ERROR;
}

const char *catalog_fileName(const catalog_entry_t *entry) {
  const char *slash = strrchr(entry->url, '/');
  return (slash != NULL) ? slash + 1 : entry->url;
}

// A ".." path segment anywhere in the URL.
static bool hasDotDotSegment(const char *url) {
  for (const char *seg = url; seg != NULL;) {
    const char *end = strchr(seg, '/');
    size_t length = (end != NULL) ? (size_t)(end - seg) : strlen(seg);
    if (length == 2 && seg[0] == '.' && seg[1] == '.') {
      return true;
    }
    seg = (end != NULL) ? end + 1 : NULL;
  }
  return false;
}

catalog_check_t catalog_check(const catalog_entry_t *entry) {
  // Only the URL's last segment names the saved file, so a '/' cannot leave
  // the ROM folder; still, an entry that tries (a ".." segment, a backslash,
  // a drive prefix) is refused whole.
  const char *name = catalog_fileName(entry);
  if (name[0] == '\0' || strcmp(name, ".") == 0 ||
      hasDotDotSegment(entry->url) || strchr(entry->url, '\\') != NULL ||
      strchr(entry->url, ':') != NULL) {
    return CATALOG_ENTRY_BAD_NAME;
  }
  if (strlen(name) >= CATALOG_SAVED_NAME_BYTES) {
    return CATALOG_ENTRY_LONG_NAME;
  }
  if (entry->sizeKb > CATALOG_MAX_SIZE_KB) {
    return CATALOG_ENTRY_TOO_LARGE;
  }
  return CATALOG_ENTRY_OK;
}
