// SSD1306 monochrome OLED display, 128x32 (1U/20HP) or 128x64 (3U/6HP).
//
// Drawing works on a framebuffer in RAM and ssd1306_flush() sends it to the
// display. The flush compares against the last frame sent and transmits
// nothing when nothing changed: at 400kHz a whole frame costs about 11ms
// (128x32) or 22ms (128x64), and repeating that thirty times a second to
// redraw identical pixels would be core0 time thrown away.
//
// The panel is picked at compile time with OLED_PANEL_128X64 (see
// MURDRUM_PANEL in CMakeLists.txt / PANEL in build.sh), not at runtime: it
// is fixed per format, like the rest of "one board, two panels": the 3U
// panel carries a 0.96" 128x64 display because the 1U's 0.91" 128x32 would
// read sideways once the board stands upright behind a 6HP panel.
#ifndef SSD1306_H
#define SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#define OLED_W 128
#if defined(OLED_PANEL_128X64)
#define OLED_H 64
#else
#define OLED_H 32
#endif
#define OLED_PAGES  (OLED_H / 8)
#define OLED_FB_LEN (OLED_W * OLED_PAGES)

// Font metrics, exposed here because the UI layout leans on them to place the
// cells. Including font5x7.h elsewhere would duplicate the glyph table in
// every translation unit.
#define OLED_CHAR_W   5
#define OLED_CHAR_ADV 6
#define OLED_COLS     (OLED_W / OLED_CHAR_ADV)  // 21 characters per row

// Sends the init sequence. false if the display does not answer: in that case
// every other call stays harmless, so an unplugged OLED does not stop the
// module from playing.
bool ssd1306_init(void);
bool ssd1306_present(void);

void ssd1306_clear(void);
void ssd1306_pixel(int x, int y, bool on);
void ssd1306_fill(int x, int y, int w, int h, bool on);

// Blits a 1-bit bitmap: row-major, MSB-first, ceil(w/8) bytes per row - the
// format PIL's Image.convert('1').tobytes() produces and what
// tools/make_logo.py packs. Goes through ssd1306_pixel() pixel by pixel, so
// it is only meant for something drawn rarely (the boot splash), not the
// per-frame render loop.
void ssd1306_bitmap(int x, int y, int w, int h, const uint8_t *bits);

// `on` false draws in reverse video, for the text inside inverted cells.
void ssd1306_text(int x, int y, const char *s, bool on);
void ssd1306_text_trunc(int x, int y, const char *s, int max_chars, bool on);

// Pixel width of the text that would be drawn, trailing advance excluded.
// Used for right-aligning.
int ssd1306_text_w(const char *s);

// Same font at 2x: every source pixel becomes a 2x2 block. Only the boot
// splash uses this - it is not a general text size, just a way to give the
// product name the same visual weight as the logo beside it.
void ssd1306_text_2x(int x, int y, const char *s, bool on);
int ssd1306_text_2x_w(const char *s);

// Contrast register (0x81), 0..255. The init sequence sets 0x8F; the config
// page moves it for a panel that reads too bright behind a dark front.
// Harmless with no display, like everything else here.
void ssd1306_set_contrast(uint8_t value);

// Sends the framebuffer if it changed. Returns true if it transmitted.
bool ssd1306_flush(void);

// Tests only: lets them inspect what was drawn.
const uint8_t *ssd1306_framebuffer(void);

#endif // SSD1306_H
