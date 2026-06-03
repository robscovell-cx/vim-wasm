#include <string.h>
#include <stdint.h>
#include <emscripten.h>
#include "terminal.h"

static Terminal term;

static void blank_cell(Cell *c) {
    c->ch   = ' ';
    c->fg   = COLOR_DEFAULT;
    c->bg   = COLOR_DEFAULT;
    c->attr = 0;
    c->_pad = 0;
}

static void blank_row(int row) {
    for (int col = 0; col < TERM_COLS; col++)
        blank_cell(&term.cells[row][col]);
}

static void scroll_up(void) {
    memmove(&term.cells[0], &term.cells[1],
            sizeof(Cell) * TERM_COLS * (TERM_ROWS - 1));
    blank_row(TERM_ROWS - 1);
}

static void newline(void) {
    term.cy++;
    if (term.cy >= TERM_ROWS) {
        scroll_up();
        term.cy = TERM_ROWS - 1;
    }
}

static void apply_sgr(void) {
    if (term.param_idx == 0 && term.param_cur == 0) {
        /* bare ESC[m — reset */
        term.cur_fg   = COLOR_DEFAULT;
        term.cur_bg   = COLOR_DEFAULT;
        term.cur_attr = 0;
        return;
    }

    /* commit last accumulated param */
    int params[17];
    int n = term.param_idx;
    memcpy(params, term.params, sizeof(int) * n);
    params[n] = term.param_cur;
    n++;

    for (int i = 0; i < n; i++) {
        int p = params[i];
        if (p == 0)  { term.cur_fg = COLOR_DEFAULT; term.cur_bg = COLOR_DEFAULT; term.cur_attr = 0; }
        else if (p == 1)  term.cur_attr |= ATTR_BOLD;
        else if (p == 4)  term.cur_attr |= ATTR_UNDERLINE;
        else if (p == 5)  term.cur_attr |= ATTR_BLINK;
        else if (p == 7)  term.cur_attr |= ATTR_REVERSE;
        else if (p == 22) term.cur_attr &= ~ATTR_BOLD;
        else if (p == 24) term.cur_attr &= ~ATTR_UNDERLINE;
        else if (p == 25) term.cur_attr &= ~ATTR_BLINK;
        else if (p == 27) term.cur_attr &= ~ATTR_REVERSE;
        else if (p >= 30 && p <= 37) term.cur_fg = p - 30;
        else if (p == 39) term.cur_fg = COLOR_DEFAULT;
        else if (p >= 40 && p <= 47) term.cur_bg = p - 40;
        else if (p == 49) term.cur_bg = COLOR_DEFAULT;
        else if (p >= 90 && p <= 97) term.cur_fg = (p - 90) + 8;
        else if (p >= 100 && p <= 107) term.cur_bg = (p - 100) + 8;
    }
}

static void csi_dispatch(uint8_t final) {
    /* commit last param digit */
    int n = term.param_idx;
    int params[17];
    memcpy(params, term.params, sizeof(int) * n);
    params[n] = term.param_cur;
    if (term.param_idx > 0 || term.param_cur > 0) n++;

    int p1 = (n >= 1) ? params[0] : 0;
    int p2 = (n >= 2) ? params[1] : 0;

    switch (final) {
    case 'H': case 'f': /* cursor position */
        term.cy = (p1 > 0 ? p1 - 1 : 0);
        term.cx = (p2 > 0 ? p2 - 1 : 0);
        if (term.cy >= TERM_ROWS) term.cy = TERM_ROWS - 1;
        if (term.cx >= TERM_COLS) term.cx = TERM_COLS - 1;
        break;
    case 'A': /* cursor up */
        term.cy -= (p1 > 0 ? p1 : 1);
        if (term.cy < 0) term.cy = 0;
        break;
    case 'B': /* cursor down */
        term.cy += (p1 > 0 ? p1 : 1);
        if (term.cy >= TERM_ROWS) term.cy = TERM_ROWS - 1;
        break;
    case 'C': /* cursor forward */
        term.cx += (p1 > 0 ? p1 : 1);
        if (term.cx >= TERM_COLS) term.cx = TERM_COLS - 1;
        break;
    case 'D': /* cursor back */
        term.cx -= (p1 > 0 ? p1 : 1);
        if (term.cx < 0) term.cx = 0;
        break;
    case 'G': /* cursor col absolute */
        term.cx = (p1 > 0 ? p1 - 1 : 0);
        if (term.cx >= TERM_COLS) term.cx = TERM_COLS - 1;
        break;
    case 'd': /* cursor row absolute */
        term.cy = (p1 > 0 ? p1 - 1 : 0);
        if (term.cy >= TERM_ROWS) term.cy = TERM_ROWS - 1;
        break;
    case 'J': /* erase in display */
        if (p1 == 0) {
            /* erase from cursor to end */
            for (int c = term.cx; c < TERM_COLS; c++)
                blank_cell(&term.cells[term.cy][c]);
            for (int r = term.cy + 1; r < TERM_ROWS; r++)
                blank_row(r);
        } else if (p1 == 1) {
            /* erase from start to cursor */
            for (int r = 0; r < term.cy; r++)
                blank_row(r);
            for (int c = 0; c <= term.cx; c++)
                blank_cell(&term.cells[term.cy][c]);
        } else if (p1 == 2) {
            for (int r = 0; r < TERM_ROWS; r++)
                blank_row(r);
        }
        break;
    case 'K': /* erase in line */
        if (p1 == 0) {
            for (int c = term.cx; c < TERM_COLS; c++)
                blank_cell(&term.cells[term.cy][c]);
        } else if (p1 == 1) {
            for (int c = 0; c <= term.cx; c++)
                blank_cell(&term.cells[term.cy][c]);
        } else if (p1 == 2) {
            blank_row(term.cy);
        }
        break;
    case 'S': /* scroll up */
        { int cnt = (p1 > 0 ? p1 : 1);
          for (int i = 0; i < cnt; i++) scroll_up(); }
        break;
    case 'L': /* insert line */
        { int cnt = (p1 > 0 ? p1 : 1);
          for (int i = 0; i < cnt; i++) {
              if (term.cy < TERM_ROWS - 1)
                  memmove(&term.cells[term.cy + 1], &term.cells[term.cy],
                          sizeof(Cell) * TERM_COLS * (TERM_ROWS - term.cy - 1));
              blank_row(term.cy);
          } }
        break;
    case 'M': /* delete line */
        { int cnt = (p1 > 0 ? p1 : 1);
          for (int i = 0; i < cnt; i++) {
              if (term.cy < TERM_ROWS - 1)
                  memmove(&term.cells[term.cy], &term.cells[term.cy + 1],
                          sizeof(Cell) * TERM_COLS * (TERM_ROWS - term.cy - 1));
              blank_row(TERM_ROWS - 1);
          } }
        break;
    case 'm': /* SGR */
        apply_sgr();
        break;
    case 's': /* save cursor */
        term.saved_cx = term.cx;
        term.saved_cy = term.cy;
        break;
    case 'u': /* restore cursor */
        term.cx = term.saved_cx;
        term.cy = term.saved_cy;
        break;
    case 'h': case 'l': /* private mode set/reset — ignore */
        break;
    default:
        break;
    }
    term.dirty = 1;
}

static void parse_byte(uint8_t b) {
    switch (term.state) {
    case STATE_GROUND:
        if (b == 0x1B) {
            term.state = STATE_ESCAPE;
        } else if (b == '\r') {
            term.cx = 0;
            term.dirty = 1;
        } else if (b == '\n') {
            newline();
            term.dirty = 1;
        } else if (b == '\x08') {
            if (term.cx > 0) term.cx--;
            term.dirty = 1;
        } else if (b == '\x07') {
            /* BEL — ignore */
        } else if (b == '\t') {
            term.cx = ((term.cx / 8) + 1) * 8;
            if (term.cx >= TERM_COLS) term.cx = TERM_COLS - 1;
            term.dirty = 1;
        } else if (b >= 0x20) {
            /* printable */
            Cell *c = &term.cells[term.cy][term.cx];
            c->ch   = b;
            c->fg   = term.cur_fg;
            c->bg   = term.cur_bg;
            c->attr = term.cur_attr;
            term.cx++;
            if (term.cx >= TERM_COLS) {
                term.cx = 0;
                newline();
            }
            term.dirty = 1;
        }
        break;

    case STATE_ESCAPE:
        if (b == '[') {
            term.state     = STATE_CSI_ENTRY;
            term.param_idx = 0;
            term.param_cur = 0;
            memset(term.params, 0, sizeof(term.params));
        } else if (b == '7') {
            term.saved_cx  = term.cx;
            term.saved_cy  = term.cy;
            term.state     = STATE_GROUND;
        } else if (b == '8') {
            term.cx        = term.saved_cx;
            term.cy        = term.saved_cy;
            term.state     = STATE_GROUND;
            term.dirty     = 1;
        } else if (b == 'D') {
            /* index — like LF */
            newline();
            term.state = STATE_GROUND;
            term.dirty = 1;
        } else if (b == 'M') {
            /* reverse index */
            term.cy--;
            if (term.cy < 0) { term.cy = 0; /* TODO: insert line */ }
            term.state = STATE_GROUND;
            term.dirty = 1;
        } else if (b == 'c') {
            /* full reset */
            term.cx = 0; term.cy = 0;
            term.cur_fg = COLOR_DEFAULT; term.cur_bg = COLOR_DEFAULT;
            term.cur_attr = 0;
            for (int r = 0; r < TERM_ROWS; r++) blank_row(r);
            term.state = STATE_GROUND;
            term.dirty = 1;
        } else if (b == 'P') {
            term.state = STATE_DCS;
        } else if (b == ']') {
            term.state = STATE_OSC;
        } else {
            term.state = STATE_GROUND;
        }
        break;

    case STATE_CSI_ENTRY:
        if (b >= '0' && b <= '9') {
            term.param_cur = b - '0';
            term.state     = STATE_CSI_PARAM;
        } else if (b == ';') {
            term.params[term.param_idx++] = 0;
            term.param_cur = 0;
        } else if (b == '?') {
            /* private mode prefix — stay in CSI_ENTRY */
        } else if (b >= 0x40 && b <= 0x7E) {
            term.param_cur = 0;
            csi_dispatch(b);
            term.state = STATE_GROUND;
        } else {
            term.state = STATE_CSI_IGNORE;
        }
        break;

    case STATE_CSI_PARAM:
        if (b >= '0' && b <= '9') {
            term.param_cur = term.param_cur * 10 + (b - '0');
        } else if (b == ';') {
            if (term.param_idx < 15)
                term.params[term.param_idx++] = term.param_cur;
            term.param_cur = 0;
        } else if (b >= 0x40 && b <= 0x7E) {
            csi_dispatch(b);
            term.state = STATE_GROUND;
        } else {
            term.state = STATE_CSI_IGNORE;
        }
        break;

    case STATE_CSI_IGNORE:
        if (b >= 0x40 && b <= 0x7E)
            term.state = STATE_GROUND;
        break;

    case STATE_DCS:
        /* skip until ST (ESC \) or BEL */
        if (b == 0x07 || b == 0x1B)
            term.state = STATE_GROUND;
        break;

    case STATE_OSC:
        if (b == 0x07 || b == 0x1B)
            term.state = STATE_GROUND;
        break;
    }
}

void terminal_write(const char *data, int len) {
    for (int i = 0; i < len; i++)
        parse_byte((uint8_t)data[i]);
}

/* ---- WASM exports ---- */

EMSCRIPTEN_KEEPALIVE void term_init(void) {
    memset(&term, 0, sizeof(term));
    term.cur_fg = COLOR_DEFAULT;
    term.cur_bg = COLOR_DEFAULT;
    term.state  = STATE_GROUND;
    for (int r = 0; r < TERM_ROWS; r++)
        blank_row(r);
    term.dirty = 1;

    /* app startup banner drawn from app.c */
    extern void app_init(void);
    app_init();
}

EMSCRIPTEN_KEEPALIVE uintptr_t term_get_cells(void) {
    return (uintptr_t)&term.cells[0][0];
}

EMSCRIPTEN_KEEPALIVE int term_cell_size(void) {
    return (int)sizeof(Cell);
}

EMSCRIPTEN_KEEPALIVE int term_is_dirty(void) {
    return term.dirty;
}

EMSCRIPTEN_KEEPALIVE void term_clear_dirty(void) {
    term.dirty = 0;
}

EMSCRIPTEN_KEEPALIVE int term_cursor_col(void) {
    return term.cx;
}

EMSCRIPTEN_KEEPALIVE int term_cursor_row(void) {
    return term.cy;
}

EMSCRIPTEN_KEEPALIVE void term_send_key(const char *key) {
    extern void app_handle_key(const char *key);
    app_handle_key(key);
}
