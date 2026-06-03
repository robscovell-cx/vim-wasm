#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "terminal.h"

/* ---- types ---- */
typedef uint32_t chtype;
typedef uint32_t attr_t;

/* ---- attribute flags (match terminal.h ATTR_* values) ---- */
#define A_NORMAL    0x00000000u
#define A_BOLD      0x00000001u
#define A_UNDERLINE 0x00000002u
#define A_REVERSE   0x00000004u
#define A_BLINK     0x00000008u
#define A_DIM       0x00000010u
#define A_INVIS     0x00000020u
#define A_STANDOUT  A_REVERSE

/* color pair packed into upper bits: bits 8-15 */
#define COLOR_PAIR(n)   (((chtype)(n)) << 8)
#define PAIR_NUMBER(a)  (((a) >> 8) & 0xFF)

/* ---- color constants (guarded so they don't conflict with app headers) ---- */
#ifndef COLOR_BLACK
#define COLOR_BLACK   0
#endif
#ifndef COLOR_RED
#define COLOR_RED     1
#endif
#ifndef COLOR_GREEN
#define COLOR_GREEN   2
#endif
#ifndef COLOR_YELLOW
#define COLOR_YELLOW  3
#endif
#ifndef COLOR_BLUE
#define COLOR_BLUE    4
#endif
#ifndef COLOR_MAGENTA
#define COLOR_MAGENTA 5
#endif
#ifndef COLOR_CYAN
#define COLOR_CYAN    6
#endif
#ifndef COLOR_WHITE
#define COLOR_WHITE   7
#endif
/* 64 pairs: tint uses pairs 0–63 (8 bg × 8 fg) */
#define COLOR_PAIRS   64

/* ---- key constants (> 255 so they fit in int, not char) ---- */
#define KEY_CODE_YES  0x100
#define KEY_UP        0x103
#define KEY_DOWN      0x102
#define KEY_LEFT      0x104
#define KEY_RIGHT     0x105
#define KEY_BACKSPACE 0x107
#define KEY_DC        0x14A   /* delete */
#define KEY_HOME      0x106
#define KEY_END       0x168
#define KEY_PPAGE     0x153   /* page up */
#define KEY_NPAGE     0x152   /* page down */
#define KEY_ENTER     0x157
#define KEY_F(n)      (0x108 + (n))
#define KEY_RESIZE    0x19A

#ifndef ERR
#define ERR  (-1)
#endif
#ifndef OK
#define OK   (0)
#endif
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* ---- WINDOW struct ---- */
struct _win_st {
    int   _begy, _begx;
    int   _maxy, _maxx;
    int   _cury, _curx;
    attr_t _attrs;
    bool  _keypad;
    bool  _nodelay;
    int   _timeout;       /* ms: -1=block, 0=nonblock, >0=timed */
    bool  _scroll;
    /* per-cell virtual buffer: only allocated up to _maxy*_maxx */
    Cell  *_cells;        /* heap-allocated, _maxy * _maxx cells */
};
typedef struct _win_st WINDOW;

extern WINDOW *stdscr;
extern WINDOW *curscr;
extern int     LINES;
extern int     COLS;

/* ---- init/teardown ---- */
WINDOW *initscr(void);
int     endwin(void);

/* ---- window management ---- */
WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
int     delwin(WINDOW *win);
int     mvwin(WINDOW *win, int y, int x);

/* ---- output ---- */
int  wrefresh(WINDOW *win);
int  wnoutrefresh(WINDOW *win);
int  doupdate(void);
int  wclear(WINDOW *win);
int  werase(WINDOW *win);
int  wclrtoeol(WINDOW *win);
int  wclrtobot(WINDOW *win);
int  wmove(WINDOW *win, int y, int x);
int  waddch(WINDOW *win, chtype ch);
int  waddnstr(WINDOW *win, const char *str, int n);
int  waddstr(WINDOW *win, const char *str);
int  wprintw(WINDOW *win, const char *fmt, ...);
int  mvwaddch(WINDOW *win, int y, int x, chtype ch);
int  mvwaddstr(WINDOW *win, int y, int x, const char *str);
int  mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int  wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs,
             chtype tl, chtype tr, chtype bl, chtype br);
int  box(WINDOW *win, chtype verch, chtype horch);
int  whline(WINDOW *win, chtype ch, int n);
int  wvline(WINDOW *win, chtype ch, int n);

/* ---- attributes / color ---- */
int  wattron(WINDOW *win, int attrs);
int  wattroff(WINDOW *win, int attrs);
int  wattrset(WINDOW *win, int attrs);
int  wcolor_set(WINDOW *win, short pair, void *opts);
int  start_color(void);
bool has_colors(void);
int  init_pair(short pair, short fg, short bg);
int  color_content(short color, short *r, short *g, short *b);
int  pair_content(short pair, short *fg, short *bg);

/* ---- input ---- */
int  wgetch(WINDOW *win);
int  wgetstr(WINDOW *win, char *str);
int  mvwgetch(WINDOW *win, int y, int x);

/* ---- input modes ---- */
int  noecho(void);
int  echo(void);
int  cbreak(void);
int  nocbreak(void);
int  raw(void);
int  noraw(void);
int  keypad(WINDOW *win, bool bf);
int  nodelay(WINDOW *win, bool bf);
int  wtimeout(WINDOW *win, int ms);
int  timeout(int ms);           /* sets stdscr timeout */
int  halfdelay(int tenths);     /* timeout = tenths * 100ms */
int  notimeout(WINDOW *win, bool bf);
int  scrollok(WINDOW *win, bool bf);
int  idlok(WINDOW *win, bool bf);
int  idcok(WINDOW *win, bool bf);
int  clearok(WINDOW *win, bool bf);

/* ---- misc ---- */
int  curs_set(int visibility);
int  beep(void);
int  flash(void);
int  napms(int ms);

/* ---- va_list output ---- */
int  vw_printw(WINDOW *win, const char *fmt, va_list ap);

/* ---- flush input queue ---- */
int  flushinp(void);

/* ---- curses key input queue (called from JS) ---- */
void curses_push_key(int key);

/* ---- convenience macros ---- */
#define refresh()          wrefresh(stdscr)
#define clear()            wclear(stdscr)
#define erase()            werase(stdscr)
#define clrtoeol()         wclrtoeol(stdscr)
#define clrtobot()         wclrtobot(stdscr)
#define move(y,x)          wmove(stdscr,(y),(x))
#define addch(c)           waddch(stdscr,(c))
#define addstr(s)          waddstr(stdscr,(s))
#define addnstr(s,n)       waddnstr(stdscr,(s),(n))
#define getch()            wgetch(stdscr)
#define getstr(s)          wgetstr(stdscr,(s))
#define attron(a)          wattron(stdscr,(a))
#define attroff(a)         wattroff(stdscr,(a))
#define attrset(a)         wattrset(stdscr,(a))
#define color_set(p,o)     wcolor_set(stdscr,(p),(o))
#define attrset(a)         wattrset(stdscr,(a))

#define mvaddch(y,x,c)     mvwaddch(stdscr,(y),(x),(c))
#define mvaddstr(y,x,s)    mvwaddstr(stdscr,(y),(x),(s))
#define mvgetch(y,x)       mvwgetch(stdscr,(y),(x))

/* printw/mvprintw need varargs — implemented as inline wrappers */
static inline int printw(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return waddstr(stdscr, buf);
}
static inline int mvprintw(int y, int x, const char *fmt, ...) {
    wmove(stdscr, y, x);
    va_list ap; va_start(ap, fmt);
    char buf[512]; vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return waddstr(stdscr, buf);
}
