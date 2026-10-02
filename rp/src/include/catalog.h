/**
 * File: catalog.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The ROM catalog, read from the CSV file on the SD card a page
 * at a time: one pass records where each page starts, then a page or a
 * single entry is read by seeking there. Only the page on screen is held in
 * memory, so the catalog has no entry limit but CATALOG_MAX_ENTRIES.
 *
 * The file: a header line, then "URL","Name","Description","Tags","Size (KB)"
 * per line. Fields may be quoted, with "" for a quote inside; lines may end
 * in LF or CRLF, the last one without either; a line may be of any length.
 */

#ifndef CATALOG_H
#define CATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Entries per page: the list navigator's lines.
#define CATALOG_PAGE_ENTRIES 16
// The largest catalog read; past it the rest is not listed. 4 bytes of index
// per page: 1 KB for 4,096 entries.
#define CATALOG_MAX_ENTRIES 4096
#define CATALOG_MAX_PAGES (CATALOG_MAX_ENTRIES / CATALOG_PAGE_ENTRIES)

#define CATALOG_URL_BYTES 128   // the URL field, decoded; longer: skipped
#define CATALOG_NAME_BYTES 64   // cut when longer
#define CATALOG_TEXT_BYTES 128  // the description, cut when longer
#define CATALOG_TAGS_BYTES 40   // cut when longer
#define CATALOG_SHOWN_BYTES 40  // a name as a page shows it
// The largest name a selection can store (a settings value, 96 bytes).
#define CATALOG_SAVED_NAME_BYTES 96

typedef struct {
  char url[CATALOG_URL_BYTES];  // decoded: a path on the catalog's host
  char name[CATALOG_NAME_BYTES];
  char description[CATALOG_TEXT_BYTES];
  char tags[CATALOG_TAGS_BYTES];
  uint32_t sizeKb;
} catalog_entry_t;

typedef struct {
  char path[CATALOG_URL_BYTES];
  uint32_t count;         // entries listed
  uint32_t skipped;       // lines that are not entries (bad or too long)
  uint32_t *pageOffsets;  // where each page's first entry starts
  uint32_t pages;
} catalog_t;

typedef enum {
  CATALOG_OK = 0,
  CATALOG_NOT_FOUND,   // no file
  CATALOG_READ_ERROR,  // the card failed
  CATALOG_NO_MEMORY,   // no room for the page index
} catalog_result_t;

// Why an entry can't be downloaded, checked when it is chosen.
typedef enum {
  CATALOG_ENTRY_OK = 0,
  CATALOG_ENTRY_TOO_LARGE,  // its size column is over 128 KB
  CATALOG_ENTRY_BAD_NAME,   // the name it would be saved under is unsafe
  CATALOG_ENTRY_LONG_NAME,  // too long to be stored as the selection
} catalog_check_t;

// One pass over the file: counts the entries and records the page offsets.
// Releases what a previous open held.
catalog_result_t catalog_open(catalog_t *cat, const char *path);
void catalog_close(catalog_t *cat);

// The names shown on page `page` (the entry's name, or its file name when the
// name is empty), cut to the line. Returns how many were read.
int catalog_readPage(const catalog_t *cat, uint32_t page,
                     char names[][CATALOG_SHOWN_BYTES], int max);

// Entry `index`, every field.
catalog_result_t catalog_readEntry(const catalog_t *cat, uint32_t index,
                                   catalog_entry_t *entry);

// The name the entry's file is saved under: the URL's last segment.
const char *catalog_fileName(const catalog_entry_t *entry);

// The entry's checks before a download.
catalog_check_t catalog_check(const catalog_entry_t *entry);

// %XX and + in a URL field, decoded into out (bounded). False when it did
// not fit.
bool catalog_urlDecode(const char *text, char *out, size_t outSize);

// A path made safe for a request: every byte but unreserved characters and
// '/' as %XX. False when it did not fit.
bool catalog_urlEncodePath(const char *text, char *out, size_t outSize);

#endif  // CATALOG_H
