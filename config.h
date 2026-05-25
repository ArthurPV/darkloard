#ifndef DARKLOARD_CONFIG_H
#define DARKLOARD_CONFIG_H

#define RGB(r, g, b) ((r) << 16 | (g) << 8 | (b))

#define DARKLOARD_FONT_NAME "Monaspace Xenon"
#define DARKLOARD_FONT_SIZE 20

#define DARKLOARD_MARGIN_TOP 10
#define DARKLOARD_MARGIN_BOTTOM 10
#define DARKLOARD_MARGIN_LEFT 10
#define DARKLOARD_MARGIN_RIGHT 10

#define DARKLOARD_DEFAULT_FG RGB(255, 255, 255)
#define DARKLOARD_DEFAULT_BG RGB(0, 0, 0)
#define DARKLOARD_CURSOR_COLOR RGB(255, 255, 255)
#define DARKLOARD_SELECTION_BG RGB(74, 144, 217)

// ANSI color palette (colors 0-15)
#define DARKLOARD_COLOR_0 RGB(0, 0, 0)        // black
#define DARKLOARD_COLOR_1 RGB(187, 0, 0)      // red
#define DARKLOARD_COLOR_2 RGB(0, 187, 0)      // green
#define DARKLOARD_COLOR_3 RGB(187, 187, 0)    // yellow
#define DARKLOARD_COLOR_4 RGB(0, 0, 187)      // blue
#define DARKLOARD_COLOR_5 RGB(187, 0, 187)    // magenta
#define DARKLOARD_COLOR_6 RGB(0, 187, 187)    // cyan
#define DARKLOARD_COLOR_7 RGB(187, 187, 187)  // white
#define DARKLOARD_COLOR_8 RGB(85, 85, 85)     // bright black
#define DARKLOARD_COLOR_9 RGB(255, 85, 85)    // bright red
#define DARKLOARD_COLOR_10 RGB(85, 255, 85)   // bright green
#define DARKLOARD_COLOR_11 RGB(255, 255, 85)  // bright yellow
#define DARKLOARD_COLOR_12 RGB(85, 85, 255)   // bright blue
#define DARKLOARD_COLOR_13 RGB(255, 85, 255)  // bright magenta
#define DARKLOARD_COLOR_14 RGB(85, 255, 255)  // bright cyan
#define DARKLOARD_COLOR_15 RGB(255, 255, 255) // bright white

#endif // DARKLOARD_CONFIG_H
