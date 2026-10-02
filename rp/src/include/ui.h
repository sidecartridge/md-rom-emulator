/**
 * File: ui.h
 * Author: Diego Parrilla Santamaría
 * Date: October 2026
 * Copyright: 2026 - GOODDATA LABS SL
 * Description: The setup screens' drawing on top of the terminal, after
 * md-devops' menu: an inverted title bar with status icons, group boxes with
 * their label on the top edge, inverted rows, a strip in the small font on
 * the bottom row, and progress bars. The terminal prints the words (so
 * `swd.py text` reads them); these draw into the framebuffer around them.
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

// Rows are the terminal's 8-pixel rows. The terminal prints to rows 0-23;
// the 25th, the strip, is only drawn on.
#define UI_STRIP_ROW 24

// The title bar's last cell holds the cursor while a screen waits for single
// keys: its block is white on the white bar there (ui_parkCursor()).
#define UI_TITLE_ROW 0

// Glyphs of u8g2_font_open_iconic_embedded_1x_t (8x8); UI_GLYPH_NONE draws
// an empty cell.
#define UI_GLYPH_NONE 0
#define UI_GLYPH_COG 0x42
#define UI_GLYPH_WARNING 0x47
#define UI_GLYPH_CARTRIDGE 0x49
#define UI_GLYPH_DRIVE 0x4C
#define UI_GLYPH_RELOAD 0x4F
#define UI_GLYPH_WIFI 0x50

/**
 * @brief Moves the terminal's cursor (VT52 ESC Y). The cell it leaves is
 * blanked: draw frames after the moves.
 */
void ui_cursorAt(uint8_t row, uint8_t col);

/**
 * @brief Moves the cursor to the title bar's last cell, where it does not
 * show: for screens that wait for single keys, not at a prompt.
 */
void ui_parkCursor(void);

/**
 * @brief Prints text at row, col without moving the cursor, cut or padded
 * with spaces to width columns, so it covers what was there.
 */
void ui_printField(uint8_t row, uint8_t col, uint8_t width, const char *text);

/**
 * @brief Inverts rows (XOR): the text printed there turns white on black.
 */
void ui_invertRows(uint8_t row, uint8_t rows);

/**
 * @brief The title bar: row 0 inverted, then its icons. The title's text
 * must be printed on row 0 first.
 */
void ui_titleBar(uint8_t sdGlyph, uint8_t wifiGlyph);

/**
 * @brief Redraws the title bar's two icon cells only.
 */
void ui_titleIcons(uint8_t sdGlyph, uint8_t wifiGlyph);

/**
 * @brief A group box from topRow to bottomRow, blank rows apart from the
 * label, which is printed on topRow at labelCol, labelLen columns long; its
 * icon sits on the top edge at the right.
 */
void ui_group(uint8_t topRow, uint8_t bottomRow, uint8_t labelCol,
              uint8_t labelLen, uint8_t glyph);

/**
 * @brief Redraws a group box's icon cell only.
 */
void ui_groupIcon(uint8_t topRow, uint8_t glyph);

/**
 * @brief A horizontal rule across the middle of a blank row.
 */
void ui_rule(uint8_t row);

/**
 * @brief The bottom strip: an inverted row with text in the small font,
 * centred or from the left.
 */
void ui_strip(const char *text, bool centred);

/**
 * @brief A progress bar on one row, from col for cols columns, and the
 * row above's last pixel row (keep that row blank): a frame, the done part
 * filled, and text in the small font centred over it, in the filled part's
 * colour on each side of the edge.
 */
void ui_bar(uint8_t row, uint8_t col, uint8_t cols, uint32_t done,
            uint32_t total, const char *text);

/**
 * @brief A notice from row: text in the small font split at spaces into at
 * most rows lines (two at most; a new line in it is a space), the rest cut,
 * on inverted rows with the warning icon. Only the rows the lines take are
 * drawn: the caller clears the others. The words also go into the
 * terminal's screen buffer, from column UI_NOTICE_COL.
 * @return The rows taken.
 */
#define UI_NOTICE_COL 2
uint8_t ui_notice(uint8_t row, uint8_t rows, const char *text);

/**
 * @brief An icon in an 8x8 cell at pixelX on row, ink on paper or, on an
 * inverted row, paper on ink.
 */
void ui_icon(uint16_t pixelX, uint8_t row, uint8_t glyph, bool onInk);

#endif  // UI_H
