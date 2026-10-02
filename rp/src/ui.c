/**
 * File: ui.c
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The setup screens' drawing on top of the terminal (ui.h).
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "display_term.h"
#include "term.h"

#define UI_ROW_Y(row) ((uint16_t)((row) * DISPLAY_TERM_CHAR_HEIGHT))
#define UI_CELL 8
// The ink colour, the paper colour, and XOR (u8g2's draw colours)
#define UI_INK 1
#define UI_PAPER 0
#define UI_XOR 2
// The title bar's icons, left of the cell the cursor is parked in
#define UI_TITLE_SD_X (DISPLAY_WIDTH - 36)
#define UI_TITLE_WIFI_X (DISPLAY_WIDTH - 23)
// A group box's sides, and its icon on the top edge at the right
#define UI_GROUP_LEFT 3
#define UI_GROUP_RIGHT (DISPLAY_WIDTH - 4)
#define UI_GROUP_ICON_X (DISPLAY_WIDTH - 24)
// The edges run through the middle of the top and bottom rows; the gap
// around the label and the icon
#define UI_GROUP_TOP_Y 3
#define UI_GROUP_BOTTOM_Y 4
#define UI_GROUP_GAP 3
// The small font's baseline in a row: its glyphs sit on rows 1-6
#define UI_SMALL_BASELINE 7
#define UI_STRIP_TEXT_X 3
// A notice's icon and text, and the longest line it takes
#define UI_NOTICE_ICON_X 4
#define UI_NOTICE_TEXT_X 16
#define UI_NOTICE_LINE_BYTES 96
#define UI_NOTICE_MAX_ROWS 2
// A progress bar: a row and the blank pixel row above it
#define UI_BAR_HEIGHT (UI_CELL + 1)

static u8g2_t *ui(void) { return display_getU8g2Ref(); }

// The terminal's font and solid glyphs back, after drawing with others
static void uiRestore(void) {
  u8g2_SetFont(ui(), u8g2_font_amstrad_cpc_extended_8f);
  u8g2_SetFontMode(ui(), 0);
  u8g2_SetDrawColor(ui(), UI_INK);
}

void ui_cursorAt(uint8_t row, uint8_t col) {
  char seq[] = {'\x1B', 'Y', (char)(TERM_POS_Y + row), (char)(TERM_POS_X + col),
                '\0'};
  term_printString(seq);
}

void ui_parkCursor(void) { ui_cursorAt(UI_TITLE_ROW, TERM_SCREEN_SIZE_X - 1); }

void ui_printField(uint8_t row, uint8_t col, uint8_t width, const char *text) {
  char field[TERM_SCREEN_SIZE_X + 1];
  if (width > TERM_SCREEN_SIZE_X) {
    width = TERM_SCREEN_SIZE_X;
  }
  snprintf(field, sizeof(field), "%-*.*s", width, width, text);
  term_printAt(row, col, field);
}

void ui_invertRows(uint8_t row, uint8_t rows) {
  u8g2_SetDrawColor(ui(), UI_XOR);
  u8g2_DrawBox(ui(), 0, UI_ROW_Y(row), DISPLAY_WIDTH,
               (uint16_t)(rows * DISPLAY_TERM_CHAR_HEIGHT));
  u8g2_SetDrawColor(ui(), UI_INK);
}

void ui_icon(uint16_t pixelX, uint8_t row, uint8_t glyph, bool onInk) {
  u8g2_SetDrawColor(ui(), onInk ? UI_INK : UI_PAPER);
  u8g2_DrawBox(ui(), pixelX, UI_ROW_Y(row), UI_CELL, UI_CELL);
  if (glyph != UI_GLYPH_NONE) {
    u8g2_SetFont(ui(), u8g2_font_open_iconic_embedded_1x_t);
    u8g2_SetFontMode(ui(), 1);
    u8g2_SetDrawColor(ui(), onInk ? UI_PAPER : UI_INK);
    u8g2_DrawGlyph(ui(), pixelX, UI_ROW_Y(row) + UI_CELL, glyph);
  }
  uiRestore();
}

uint8_t ui_notice(uint8_t row, uint8_t rows, const char *text) {
  uint16_t top = UI_ROW_Y(row);
  uint16_t maxWidth = DISPLAY_WIDTH - UI_NOTICE_TEXT_X - UI_NOTICE_TEXT_X;
  char parts[UI_NOTICE_MAX_ROWS][UI_NOTICE_LINE_BYTES];
  uint8_t used = 0;
  if (rows > UI_NOTICE_MAX_ROWS) {
    rows = UI_NOTICE_MAX_ROWS;
  }
  u8g2_SetFont(ui(), u8g2_font_squeezed_b7_tr);
  for (; used < rows && *text != '\0'; used++) {
    while (*text == ' ' || *text == '\n') {
      text++;
    }
    // The longest run of whole words that fits, or a cut word if one alone
    // does not.
    char *part = parts[used];
    size_t fit = 0;
    size_t len = 0;
    while (text[len] != '\0' && len + 1 < UI_NOTICE_LINE_BYTES) {
      part[len] = (text[len] == '\n') ? ' ' : text[len];
      part[len + 1] = '\0';
      if (u8g2_GetStrWidth(ui(), part) > maxWidth) {
        break;
      }
      len++;
      if (text[len] == '\0' || text[len] == ' ' || text[len] == '\n') {
        fit = len;
      }
    }
    if (fit == 0) {
      fit = len;
    }
    part[fit] = '\0';
    text += fit;
  }
  if (used == 0) {
    uiRestore();
    return 0;
  }
  u8g2_SetDrawColor(ui(), UI_INK);
  u8g2_DrawBox(ui(), 0, top, DISPLAY_WIDTH,
               (uint16_t)(used * DISPLAY_TERM_CHAR_HEIGHT));
  ui_icon(UI_NOTICE_ICON_X, row, UI_GLYPH_WARNING, true);
  u8g2_SetFont(ui(), u8g2_font_squeezed_b7_tr);
  u8g2_SetFontMode(ui(), 1);
  u8g2_SetDrawColor(ui(), UI_PAPER);
  for (uint8_t line = 0; line < used; line++) {
    u8g2_DrawStr(
        ui(), UI_NOTICE_TEXT_X,
        top + (uint16_t)(line * DISPLAY_TERM_CHAR_HEIGHT) + UI_SMALL_BASELINE,
        parts[line]);
    term_recordAt((uint8_t)(row + line), UI_NOTICE_COL, parts[line]);
  }
  uiRestore();
  return used;
}

void ui_titleIcons(uint8_t sdGlyph, uint8_t wifiGlyph) {
  ui_icon(UI_TITLE_SD_X, UI_TITLE_ROW, sdGlyph, true);
  ui_icon(UI_TITLE_WIFI_X, UI_TITLE_ROW, wifiGlyph, true);
}

void ui_titleBar(uint8_t sdGlyph, uint8_t wifiGlyph) {
  ui_invertRows(UI_TITLE_ROW, 1);
  ui_titleIcons(sdGlyph, wifiGlyph);
}

void ui_group(uint8_t topRow, uint8_t bottomRow, uint8_t labelCol,
              uint8_t labelLen, uint8_t glyph) {
  uint16_t yTop = UI_ROW_Y(topRow) + UI_GROUP_TOP_Y;
  uint16_t yBottom = UI_ROW_Y(bottomRow) + UI_GROUP_BOTTOM_Y;
  uint16_t labelStart = (uint16_t)(labelCol * UI_CELL) - UI_GROUP_GAP - 1;
  uint16_t labelEnd =
      (uint16_t)((labelCol + labelLen) * UI_CELL) + UI_GROUP_GAP;
  uint16_t iconStart = UI_GROUP_ICON_X - UI_GROUP_GAP;
  uint16_t iconEnd = UI_GROUP_ICON_X + UI_CELL + UI_GROUP_GAP;
  u8g2_SetDrawColor(ui(), UI_INK);
  u8g2_DrawHLine(ui(), UI_GROUP_LEFT, yTop, labelStart - UI_GROUP_LEFT);
  u8g2_DrawHLine(ui(), labelEnd, yTop, iconStart - labelEnd);
  u8g2_DrawHLine(ui(), iconEnd, yTop, UI_GROUP_RIGHT - iconEnd + 1);
  u8g2_DrawVLine(ui(), UI_GROUP_LEFT, yTop, yBottom - yTop + 1);
  u8g2_DrawVLine(ui(), UI_GROUP_RIGHT, yTop, yBottom - yTop + 1);
  u8g2_DrawHLine(ui(), UI_GROUP_LEFT, yBottom,
                 UI_GROUP_RIGHT - UI_GROUP_LEFT + 1);
  ui_groupIcon(topRow, glyph);
}

void ui_groupIcon(uint8_t topRow, uint8_t glyph) {
  ui_icon(UI_GROUP_ICON_X, topRow, glyph, false);
}

void ui_rule(uint8_t row) {
  u8g2_SetDrawColor(ui(), UI_INK);
  u8g2_DrawHLine(ui(), 0, UI_ROW_Y(row) + UI_GROUP_TOP_Y, DISPLAY_WIDTH);
}

void ui_strip(const char *text, bool centred) {
  uint16_t top = UI_ROW_Y(UI_STRIP_ROW);
  u8g2_SetDrawColor(ui(), UI_INK);
  u8g2_DrawBox(ui(), 0, top, DISPLAY_WIDTH, UI_CELL);
  u8g2_SetFont(ui(), u8g2_font_squeezed_b7_tr);
  u8g2_SetFontMode(ui(), 1);
  uint16_t left = UI_STRIP_TEXT_X;
  if (centred) {
    uint16_t width = u8g2_GetStrWidth(ui(), text);
    left =
        (width < DISPLAY_WIDTH) ? (uint16_t)((DISPLAY_WIDTH - width) / 2) : 0;
  }
  u8g2_SetDrawColor(ui(), UI_PAPER);
  u8g2_DrawStr(ui(), left, top + UI_SMALL_BASELINE, text);
  uiRestore();
}

void ui_bar(uint8_t row, uint8_t col, uint8_t cols, uint32_t done,
            uint32_t total, const char *text) {
  // One pixel taller than the row, reaching into the blank row above: the
  // small font's seven rows fit inside the frame, clear of it.
  uint16_t left = (uint16_t)(col * UI_CELL);
  uint16_t top = UI_ROW_Y(row) - 1;
  uint16_t width = (uint16_t)(cols * UI_CELL);
  uint16_t height = UI_BAR_HEIGHT;
  uint16_t inner = width - 2;
  if (total == 0) {
    total = 1;
  }
  if (done > total) {
    done = total;
  }
  uint16_t filled = (uint16_t)(((uint64_t)inner * done) / total);
  u8g2_SetDrawColor(ui(), UI_PAPER);
  u8g2_DrawBox(ui(), left, top, width, height);
  u8g2_SetDrawColor(ui(), UI_INK);
  u8g2_DrawFrame(ui(), left, top, width, height);
  if (filled > 0) {
    u8g2_DrawBox(ui(), left + 1, top + 1, filled, height - 2);
  }
  // The text twice, clipped to each side of the edge: paper on the filled
  // part, ink on the rest (md-devops' countdown bar).
  u8g2_SetFont(ui(), u8g2_font_squeezed_b7_tr);
  u8g2_SetFontMode(ui(), 1);
  uint16_t textWidth = u8g2_GetStrWidth(ui(), text);
  uint16_t textX = (textWidth < width)
                       ? (uint16_t)(left + ((width - textWidth) / 2))
                       : left + 1;
  uint16_t baseline = top + height - 1;
  if (filled > 0) {
    u8g2_SetClipWindow(ui(), left + 1, top + 1, left + 1 + filled,
                       top + height - 1);
    u8g2_SetDrawColor(ui(), UI_PAPER);
    u8g2_DrawStr(ui(), textX, baseline, text);
  }
  if (filled < inner) {
    u8g2_SetClipWindow(ui(), left + 1 + filled, top + 1, left + width - 1,
                       top + height - 1);
    u8g2_SetDrawColor(ui(), UI_INK);
    u8g2_DrawStr(ui(), textX, baseline, text);
  }
  u8g2_SetMaxClipWindow(ui());
  uiRestore();
}
