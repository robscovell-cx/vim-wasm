#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <emscripten.h>
#include "terminal.h"
#include "curses.h"

/* forward declarations from terminal.c and app.c */
void terminal_write(const char *data, int len);
extern int curses_active;        /* defined in app.c */
void app_init(void);             /* defined in app.c */

/* ---- globals ---- */
WINDOW *stdscr = NULL;
WINDOW *curscr = NULL;
int     LINES  = TERM_ROWS;
int     COLS   = TERM_COLS;

/* shadow buffer: what is actually on screen right now */
static Cell screen_shadow[TERM_ROWS][TERM_COLS];
static bool shadow_valid = false;

/* color pairs table */
static short color_pairs[COLOR_PAIRS][2]; /* [pair] = {fg, bg} */
static bool  colors_started = false;

/* ---- helpers ---- */
static void tw(const char *s) { terminal_write(s, (int)strlen(s)); }
static void twf(const char *fmt, ...) {
    char buf[128]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    terminal_write(buf, (int)strlen(buf));
}

/* ---- key queue ---- */
#define KEY_QUEUE_SZ 64
static int  kq[KEY_QUEUE_SZ];
static int  kq_head = 0, kq_tail = 0;

EMSCRIPTEN_KEEPALIVE void curses_push_key(int key) {
    int next = (kq_head + 1) % KEY_QUEUE_SZ;
    if (next != kq_tail) {
        kq[kq_head] = key;
        kq_head = next;
    }
}

static int kq_pop(void) {
    if (kq_head == kq_tail) return ERR;
    int k = kq[kq_tail];
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SZ;
    return k;
}

/* ---- WINDOW allocation ---- */
static WINDOW *win_alloc(int nlines, int ncols, int begy, int begx) {
    WINDOW *w = calloc(1, sizeof(WINDOW));
    if (!w) return NULL;
    w->_maxy    = nlines;
    w->_maxx    = ncols;
    w->_begy    = begy;
    w->_begx    = begx;
    w->_cury    = 0;
    w->_curx    = 0;
    w->_attrs   = A_NORMAL;
    w->_keypad  = false;
    w->_nodelay = false;
    w->_timeout = -1;   /* blocking by default */
    w->_cells   = calloc(nlines * ncols, sizeof(Cell));
    if (!w->_cells) { free(w); return NULL; }
    /* fill with spaces */
    for (int i = 0; i < nlines * ncols; i++) {
        w->_cells[i].ch   = ' ';
        w->_cells[i].fg   = 8;
        w->_cells[i].bg   = 8;
        w->_cells[i].attr = 0;
    }
    return w;
}

static Cell *win_cell(WINDOW *w, int y, int x) {
    return &w->_cells[y * w->_maxx + x];
}

/* ---- init / teardown ---- */
WINDOW *initscr(void) {
    if (stdscr) return stdscr;

    memset(color_pairs, 0, sizeof(color_pairs));
    colors_started = false;
    shadow_valid   = false;
    kq_head = kq_tail = 0;

    stdscr = win_alloc(TERM_ROWS, TERM_COLS, 0, 0);
    stdscr->_timeout = -1;

    /* clear physical screen */
    tw("\x1B[2J\x1B[H");
    tw("\x1B[?25l");   /* hide cursor while drawing */

    return stdscr;
}

int endwin(void) {
    if (!stdscr) return ERR;

    tw("\x1B[0m\x1B[?25h");  /* reset attrs, show cursor */
    tw("\x1B[2J\x1B[H");

    free(stdscr->_cells);
    free(stdscr);
    stdscr = NULL;

    shadow_valid = false;

    /* return to normal shell */
    curses_active = 0;
    app_init();

    return OK;
}

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x) {
    return win_alloc(nlines, ncols, begin_y, begin_x);
}

int delwin(WINDOW *win) {
    if (!win || win == stdscr) return ERR;
    free(win->_cells);
    free(win);
    return OK;
}

int mvwin(WINDOW *win, int y, int x) {
    if (!win) return ERR;
    win->_begy = y;
    win->_begx = x;
    return OK;
}

/* ---- wrefresh ---- */
int wrefresh(WINDOW *win) {
    if (!win) return ERR;

    attr_t last_attr = (attr_t)-1;

    for (int row = 0; row < win->_maxy; row++) {
        int sy = win->_begy + row;
        if (sy < 0 || sy >= TERM_ROWS) continue;

        for (int col = 0; col < win->_maxx; col++) {
            int sx = win->_begx + col;
            if (sx < 0 || sx >= TERM_COLS) continue;

            Cell *wc = win_cell(win, row, col);

            /* skip if matches shadow */
            if (shadow_valid) {
                Cell *sc = &screen_shadow[sy][sx];
                if (sc->ch == wc->ch && sc->fg == wc->fg &&
                    sc->bg == wc->bg && sc->attr == wc->attr)
                    continue;
            }

            /* move cursor */
            twf("\x1B[%d;%dH", sy + 1, sx + 1);

            /* emit SGR only if attrs changed */
            attr_t cell_attr = wc->attr | ((attr_t)(wc->fg == 8 && wc->bg == 8 ? 0 :
                /* rebuild pair from cell fg/bg for shadow-only cells */ 0));
            /* For window cells written via waddch with active attrs, attrs are
               stored in the cell. Just re-emit if different from last. */
            (void)cell_attr;
            /* Build a synthetic attr_t from cell fg/bg/attr for SGR emission */
            attr_t sgr_key = ((attr_t)wc->attr)
                           | ((attr_t)(wc->fg < 8 ? wc->fg + 1 : 0) << 16)
                           | ((attr_t)(wc->bg < 8 ? wc->bg + 1 : 0) << 24);
            if (sgr_key != last_attr) {
                /* emit SGR directly from cell fields */
                char buf[64]; int n = 0;
                n += snprintf(buf+n, sizeof(buf)-n, "\x1B[0");
                if (wc->fg < 8) n += snprintf(buf+n, sizeof(buf)-n, ";%d", 30 + wc->fg);
                if (wc->bg < 8) n += snprintf(buf+n, sizeof(buf)-n, ";%d", 40 + wc->bg);
                if (wc->attr & A_BOLD)      n += snprintf(buf+n, sizeof(buf)-n, ";1");
                if (wc->attr & A_UNDERLINE) n += snprintf(buf+n, sizeof(buf)-n, ";4");
                if (wc->attr & A_BLINK)     n += snprintf(buf+n, sizeof(buf)-n, ";5");
                if (wc->attr & A_REVERSE)   n += snprintf(buf+n, sizeof(buf)-n, ";7");
                snprintf(buf+n, sizeof(buf)-n, "m");
                tw(buf);
                last_attr = sgr_key;
            }

            /* emit character */
            if (wc->ch >= 0x20 && wc->ch < 0x7F) {
                char c = (char)wc->ch;
                terminal_write(&c, 1);
            } else if (wc->ch == 0 || wc->ch == ' ') {
                tw(" ");
            } else {
                tw(" ");
            }

            /* update shadow */
            screen_shadow[sy][sx] = *wc;
        }
    }

    /* position cursor at window's current cursor position */
    int cy = win->_begy + win->_cury;
    int cx = win->_begx + win->_curx;
    if (cy >= 0 && cy < TERM_ROWS && cx >= 0 && cx < TERM_COLS)
        twf("\x1B[%d;%dH", cy + 1, cx + 1);

    tw("\x1B[?25h");   /* show cursor */
    shadow_valid = true;
    return OK;
}

int wnoutrefresh(WINDOW *win) { return wrefresh(win); }
int doupdate(void)             { return OK; }

/* ---- clear / erase ---- */
int wclear(WINDOW *win) {
    if (!win) return ERR;
    for (int i = 0; i < win->_maxy * win->_maxx; i++) {
        win->_cells[i].ch   = ' ';
        win->_cells[i].fg   = 8;
        win->_cells[i].bg   = 8;
        win->_cells[i].attr = 0;
    }
    win->_cury = 0;
    win->_curx = 0;
    shadow_valid = false;   /* force full redraw */
    return OK;
}

int werase(WINDOW *win) { return wclear(win); }

int wclrtoeol(WINDOW *win) {
    if (!win) return ERR;
    for (int x = win->_curx; x < win->_maxx; x++) {
        Cell *c = win_cell(win, win->_cury, x);
        c->ch = ' '; c->fg = 8; c->bg = 8; c->attr = 0;
    }
    return OK;
}

int wclrtobot(WINDOW *win) {
    if (!win) return ERR;
    wclrtoeol(win);
    for (int y = win->_cury + 1; y < win->_maxy; y++)
        for (int x = 0; x < win->_maxx; x++) {
            Cell *c = win_cell(win, y, x);
            c->ch = ' '; c->fg = 8; c->bg = 8; c->attr = 0;
        }
    return OK;
}

/* ---- move ---- */
int wmove(WINDOW *win, int y, int x) {
    if (!win) return ERR;
    if (y < 0 || y >= win->_maxy || x < 0 || x >= win->_maxx) return ERR;
    win->_cury = y;
    win->_curx = x;
    return OK;
}

/* ---- addch / addstr ---- */
int waddch(WINDOW *win, chtype ch) {
    if (!win) return ERR;
    /* extract character and any inline attributes */
    uint32_t c    = ch & 0xFF;       /* ASCII portion */
    attr_t   ina  = ch & ~0xFFu;     /* attrs encoded in chtype */
    attr_t   attr = win->_attrs | ina;

    /* resolve color pair */
    int pair = (int)PAIR_NUMBER(attr);
    short fg = 8, bg = 8;
    if (pair >= 0 && pair < COLOR_PAIRS && colors_started) {
        fg = color_pairs[pair][0];
        bg = color_pairs[pair][1];
    }
    uint8_t cell_attr = (uint8_t)(attr & 0xFF);  /* A_BOLD/UL/REV/BLINK */

    if (c == '\n') {
        win->_cury++;
        win->_curx = 0;
        if (win->_cury >= win->_maxy) win->_cury = win->_maxy - 1;
        return OK;
    }
    if (c == '\r') { win->_curx = 0; return OK; }
    if (c == '\b') { if (win->_curx > 0) win->_curx--; return OK; }

    if (win->_cury < win->_maxy && win->_curx < win->_maxx) {
        Cell *cell = win_cell(win, win->_cury, win->_curx);
        cell->ch   = c ? c : ' ';
        cell->fg   = (uint8_t)fg;
        cell->bg   = (uint8_t)bg;
        cell->attr = cell_attr;
    }

    win->_curx++;
    if (win->_curx >= win->_maxx) {
        win->_curx = 0;
        win->_cury++;
        if (win->_cury >= win->_maxy) win->_cury = win->_maxy - 1;
    }
    return OK;
}

int waddnstr(WINDOW *win, const char *str, int n) {
    if (!win || !str) return ERR;
    for (int i = 0; (n < 0 || i < n) && str[i]; i++)
        waddch(win, (unsigned char)str[i]);
    return OK;
}

int waddstr(WINDOW *win, const char *str) { return waddnstr(win, str, -1); }

int wprintw(WINDOW *win, const char *fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return waddstr(win, buf);
}

int vw_printw(WINDOW *win, const char *fmt, va_list ap) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    return waddstr(win, buf);
}

int mvwaddch(WINDOW *win, int y, int x, chtype ch) {
    wmove(win, y, x); return waddch(win, ch);
}
int mvwaddstr(WINDOW *win, int y, int x, const char *s) {
    wmove(win, y, x); return waddstr(win, s);
}
int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...) {
    wmove(win, y, x);
    char buf[512]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return waddstr(win, buf);
}

/* ---- border / box ---- */
int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs,
            chtype tl, chtype tr, chtype bl, chtype br) {
    if (!win) return ERR;
    int ym = win->_maxy - 1, xm = win->_maxx - 1;
    wmove(win, 0, 0);       waddch(win, tl);
    for (int x = 1; x < xm; x++) { wmove(win, 0, x);  waddch(win, ts); }
    wmove(win, 0, xm);      waddch(win, tr);
    for (int y = 1; y < ym; y++) {
        wmove(win, y, 0);   waddch(win, ls);
        wmove(win, y, xm);  waddch(win, rs);
    }
    wmove(win, ym, 0);      waddch(win, bl);
    for (int x = 1; x < xm; x++) { wmove(win, ym, x); waddch(win, bs); }
    wmove(win, ym, xm);     waddch(win, br);
    return OK;
}

int box(WINDOW *win, chtype verch, chtype horch) {
    chtype v = verch ? verch : '|';
    chtype h = horch ? horch : '-';
    return wborder(win, v, v, h, h, '+', '+', '+', '+');
}

int whline(WINDOW *win, chtype ch, int n) {
    for (int i = 0; i < n; i++) waddch(win, ch);
    return OK;
}
int wvline(WINDOW *win, chtype ch, int n) {
    int sy = win->_cury, sx = win->_curx;
    for (int i = 0; i < n; i++) { wmove(win, sy + i, sx); waddch(win, ch); }
    return OK;
}

/* ---- attributes ---- */
int wattron(WINDOW *win, int attrs)  { if (win) win->_attrs |= (attr_t)attrs; return OK; }
int wattroff(WINDOW *win, int attrs) { if (win) win->_attrs &= ~(attr_t)attrs; return OK; }
int wattrset(WINDOW *win, int attrs) { if (win) win->_attrs = (attr_t)attrs; return OK; }
int wcolor_set(WINDOW *win, short pair, void *opts) {
    (void)opts;
    if (win) {
        win->_attrs = (win->_attrs & ~(0xFF << 8)) | COLOR_PAIR(pair);
    }
    return OK;
}

/* ---- color ---- */
int start_color(void) { colors_started = true; return OK; }
bool has_colors(void) { return true; }

int init_pair(short pair, short fg, short bg) {
    if (pair < 0 || pair >= COLOR_PAIRS) return ERR;
    color_pairs[pair][0] = fg;
    color_pairs[pair][1] = bg;
    return OK;
}

int color_content(short color, short *r, short *g, short *b) {
    (void)color; if (r) *r = 0; if (g) *g = 0; if (b) *b = 0;
    return OK;
}
int pair_content(short pair, short *fg, short *bg) {
    if (pair < 0 || pair >= COLOR_PAIRS) return ERR;
    if (fg) *fg = color_pairs[pair][0];
    if (bg) *bg = color_pairs[pair][1];
    return OK;
}

/* ---- input modes ---- */
int noecho(void)            { return OK; }
int echo(void)              { return OK; }
int cbreak(void)            { return OK; }
int nocbreak(void)          { return OK; }
int raw(void)               { return OK; }
int noraw(void)             { return OK; }
int scrollok(WINDOW *w, bool bf) { if (w) w->_scroll = bf; return OK; }
int idlok(WINDOW *w, bool bf)    { (void)w; (void)bf; return OK; }
int idcok(WINDOW *w, bool bf)    { (void)w; (void)bf; return OK; }
int clearok(WINDOW *w, bool bf)  { (void)bf; if (w) shadow_valid = false; return OK; }
int notimeout(WINDOW *w, bool bf){ (void)w; (void)bf; return OK; }

int keypad(WINDOW *win, bool bf) {
    if (win) win->_keypad = bf;
    return OK;
}
int nodelay(WINDOW *win, bool bf) {
    if (win) { win->_nodelay = bf; win->_timeout = bf ? 0 : -1; }
    return OK;
}
int wtimeout(WINDOW *win, int ms) {
    if (win) { win->_timeout = ms; win->_nodelay = (ms == 0); }
    return OK;
}
int timeout(int ms) { wtimeout(stdscr, ms); return OK; }
int halfdelay(int tenths) { wtimeout(stdscr, tenths * 100); return OK; }

/* ---- wgetch (uses asyncify) ---- */
int wgetch(WINDOW *win) {
    if (!win) return ERR;
    int ms = win->_nodelay ? 0 : win->_timeout;

    if (ms == 0) {
        /* non-blocking: pop immediately */
        return kq_pop();
    }
    if (ms > 0) {
        /* timed: sleep once, then pop */
        emscripten_sleep((unsigned)ms);
        return kq_pop();
    }
    /* blocking: poll every 10ms */
    while (1) {
        int k = kq_pop();
        if (k != ERR) return k;
        emscripten_sleep(10);
    }
}

int mvwgetch(WINDOW *win, int y, int x) {
    wmove(win, y, x);
    return wgetch(win);
}

int wgetstr(WINDOW *win, char *str) {
    if (!win || !str) return ERR;
    int i = 0, ch;
    while ((ch = wgetch(win)) != '\n' && ch != ERR) {
        if (ch == KEY_BACKSPACE || ch == '\b') {
            if (i > 0) { i--; waddstr(win, "\x08 \x08"); }
        } else if (ch >= 0x20 && ch < 0x7F) {
            str[i++] = (char)ch;
            waddch(win, (chtype)ch);
        }
    }
    str[i] = '\0';
    return OK;
}

int flushinp(void) {
    kq_head = kq_tail = 0;
    return OK;
}

/* ---- misc ---- */
int curs_set(int v) { tw(v ? "\x1B[?25h" : "\x1B[?25l"); return OK; }
int beep(void)  { return OK; }
int flash(void) { return OK; }
int napms(int ms) { emscripten_sleep((unsigned)ms); return OK; }
