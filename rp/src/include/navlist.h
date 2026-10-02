/**
 * File: navlist.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Moving through a list a page at a time, as md-drives-emulator's
 * folder navigator does: up and down within the page, left and right a page
 * at a time with the selection on the page's first line, RETURN to choose,
 * ESC (or M) to leave. Only the arithmetic: the caller draws the page and
 * fetches its entries.
 */

#ifndef NAVLIST_H
#define NAVLIST_H

#include <stdint.h>

#define NAVLIST_PAGE_LINES 16

typedef struct {
  uint32_t count;
  uint32_t selected;
} navlist_t;

typedef enum {
  NAVLIST_NONE = 0,     // nothing changed
  NAVLIST_MOVED,        // the selection moved within the page
  NAVLIST_PAGE_TURNED,  // another page: fetch it
  NAVLIST_CHOSEN,       // RETURN on an entry
  NAVLIST_LEAVE,        // ESC or M
} navlist_action_t;

void navlist_reset(navlist_t *nav, uint32_t count);
navlist_action_t navlist_key(navlist_t *nav, char key);
uint32_t navlist_page(const navlist_t *nav);
uint32_t navlist_pages(const navlist_t *nav);
// The index of the page's first entry, and how many entries the page has.
uint32_t navlist_first(const navlist_t *nav);
uint32_t navlist_onPage(const navlist_t *nav);

#endif  // NAVLIST_H
