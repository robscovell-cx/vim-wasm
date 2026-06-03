#pragma once
#include <stdint.h>

#define TERM_COLS 80
#define TERM_ROWS 24

#define ATTR_BOLD      0x01
#define ATTR_UNDERLINE 0x02
#define ATTR_REVERSE   0x04
#define ATTR_BLINK     0x08

#define COLOR_DEFAULT 8

typedef struct {
    uint32_t ch;
    uint8_t  fg;
    uint8_t  bg;
    uint8_t  attr;
    uint8_t  _pad;
} Cell;

typedef enum {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_CSI_ENTRY,
    STATE_CSI_PARAM,
    STATE_CSI_IGNORE,
    STATE_DCS,
    STATE_OSC,
} ParseState;

typedef struct {
    Cell       cells[TERM_ROWS][TERM_COLS];
    int        cx, cy;
    int        saved_cx, saved_cy;
    uint8_t    cur_fg, cur_bg, cur_attr;
    ParseState state;
    int        params[16];
    int        param_idx;
    int        param_cur;
    int        dirty;
} Terminal;

/* internal — called by app.c */
void terminal_write(const char *data, int len);
