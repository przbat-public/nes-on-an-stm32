/*
 * font5x7.h — the panel's text font, as data.
 *
 * 95 glyphs covering printable ASCII, 0x20 (space) through 0x7E (tilde), in
 * order, so a character is found at index (c - 0x20). Each glyph is five
 * bytes, one per pixel column, and inside a byte the rows run from the top
 * down starting at the least significant bit. The eighth bit stays clear,
 * which is where the 5x7 shape comes from.
 *
 * This file holds data only. lcd_text() in lcd.c draws it and is the only
 * code that should index the table, because that function clamps a string
 * into the range above; a stray byte outside it would read past the end.
 *
 * The self-test cartridges carry their own font, built as CHR tiles by
 * tools/make_test_rom.py. The two are independent: this one is drawn by the
 * firmware into the display framebuffer, that one is drawn by an emulated
 * NES out of pattern-table memory.
 */
#pragma once
#include <stdint.h>

extern const uint8_t Font5x7[][5];
