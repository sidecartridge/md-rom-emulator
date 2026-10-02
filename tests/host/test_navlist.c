// units: rp/src/navlist.c
/* The list navigator's arithmetic (rp/src/navlist.c): first, middle and last
 * pages, a last page of one entry, empty and one-entry lists, the selection
 * on the first line after a page turn, and the keys that leave or choose. */
#include "navlist.h"
#include "term.h"
#include "test.h"

int main(void) {
  navlist_t nav;

  // Empty: nothing moves, RETURN chooses nothing, ESC leaves.
  navlist_reset(&nav, 0);
  CHECK_EQ(navlist_pages(&nav), 0);
  CHECK_EQ(navlist_onPage(&nav), 0);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_DOWN), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_RIGHT), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, '\r'), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_ESC), NAVLIST_LEAVE);

  // One entry.
  navlist_reset(&nav, 1);
  CHECK_EQ(navlist_pages(&nav), 1);
  CHECK_EQ(navlist_onPage(&nav), 1);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_DOWN), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_UP), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, '\r'), NAVLIST_CHOSEN);
  CHECK_EQ(nav.selected, 0);

  // A page exactly full: down stops on its last line.
  navlist_reset(&nav, NAVLIST_PAGE_LINES);
  for (int i = 0; i < NAVLIST_PAGE_LINES + 3; i++) {
    navlist_key(&nav, TERM_KEY_DOWN);
  }
  CHECK_EQ(nav.selected, NAVLIST_PAGE_LINES - 1);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_RIGHT), NAVLIST_NONE);

  // 17: a last page of one entry.
  navlist_reset(&nav, NAVLIST_PAGE_LINES + 1);
  CHECK_EQ(navlist_pages(&nav), 2);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_RIGHT), NAVLIST_PAGE_TURNED);
  CHECK_EQ(nav.selected, NAVLIST_PAGE_LINES);
  CHECK_EQ(navlist_first(&nav), NAVLIST_PAGE_LINES);
  CHECK_EQ(navlist_onPage(&nav), 1);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_DOWN), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_RIGHT), NAVLIST_NONE);

  // 40: first, middle, last; up and down stay on the page, a page turn puts
  // the selection on the new page's first line.
  navlist_reset(&nav, 40);
  CHECK_EQ(navlist_pages(&nav), 3);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_DOWN), NAVLIST_MOVED);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_DOWN), NAVLIST_MOVED);
  CHECK_EQ(nav.selected, 2);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_LEFT), NAVLIST_NONE);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_RIGHT), NAVLIST_PAGE_TURNED);
  CHECK_EQ(nav.selected, 16);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_UP), NAVLIST_NONE);  // the page's top
  CHECK_EQ(navlist_key(&nav, TERM_KEY_RIGHT), NAVLIST_PAGE_TURNED);
  CHECK_EQ(nav.selected, 32);
  CHECK_EQ(navlist_onPage(&nav), 8);
  for (int i = 0; i < 20; i++) {
    navlist_key(&nav, TERM_KEY_DOWN);
  }
  CHECK_EQ(nav.selected, 39);
  CHECK_EQ(navlist_key(&nav, TERM_KEY_LEFT), NAVLIST_PAGE_TURNED);
  CHECK_EQ(nav.selected, 16);
  CHECK_EQ(navlist_key(&nav, 'm'), NAVLIST_LEAVE);
  CHECK_EQ(navlist_key(&nav, 'x'), NAVLIST_NONE);

  TEST_END();
}
