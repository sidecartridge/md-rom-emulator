/**
 * File: navlist.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: Moving through a list a page at a time (navlist.h).
 */

#include "navlist.h"

#include "term.h"

void navlist_reset(navlist_t *nav, uint32_t count) {
  nav->count = count;
  nav->selected = 0;
}

uint32_t navlist_page(const navlist_t *nav) {
  return nav->selected / NAVLIST_PAGE_LINES;
}

uint32_t navlist_pages(const navlist_t *nav) {
  return (nav->count + NAVLIST_PAGE_LINES - 1) / NAVLIST_PAGE_LINES;
}

uint32_t navlist_first(const navlist_t *nav) {
  return navlist_page(nav) * NAVLIST_PAGE_LINES;
}

uint32_t navlist_onPage(const navlist_t *nav) {
  uint32_t first = navlist_first(nav);
  if (first >= nav->count) {
    return 0;
  }
  uint32_t left = nav->count - first;
  return (left < NAVLIST_PAGE_LINES) ? left : NAVLIST_PAGE_LINES;
}

navlist_action_t navlist_key(navlist_t *nav, char key) {
  uint32_t page = navlist_page(nav);
  uint32_t line = nav->selected % NAVLIST_PAGE_LINES;
  switch (key) {
    case TERM_KEY_UP:
      if (line > 0) {
        nav->selected--;
        return NAVLIST_MOVED;
      }
      return NAVLIST_NONE;
    case TERM_KEY_DOWN:
      if (nav->selected + 1 < nav->count && line + 1 < NAVLIST_PAGE_LINES) {
        nav->selected++;
        return NAVLIST_MOVED;
      }
      return NAVLIST_NONE;
    case TERM_KEY_LEFT:
      if (page > 0) {
        nav->selected = (page - 1) * NAVLIST_PAGE_LINES;
        return NAVLIST_PAGE_TURNED;
      }
      return NAVLIST_NONE;
    case TERM_KEY_RIGHT:
      if (page + 1 < navlist_pages(nav)) {
        nav->selected = (page + 1) * NAVLIST_PAGE_LINES;
        return NAVLIST_PAGE_TURNED;
      }
      return NAVLIST_NONE;
    case '\r':
    case '\n':
      return (nav->count > 0) ? NAVLIST_CHOSEN : NAVLIST_NONE;
    case TERM_KEY_ESC:
    case 'm':
    case 'M':
      return NAVLIST_LEAVE;
    default:
      return NAVLIST_NONE;
  }
}
