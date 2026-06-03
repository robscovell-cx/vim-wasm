#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "curses.h"

/* Field dimensions (includes border) */
#define FW  78   /* cols — fits inside 80-wide screen with 1-col margin each side */
#define FH  22   /* rows — fits inside 24-row screen with 1-row margin each side */

/* Playfield interior (inside the box border) */
#define PW  (FW - 2)
#define PH  (FH - 2)  /* bottom interior row reserved for score line */

#define MAX_SNAKE (PW * PH)

#define CP_BORDER 1
#define CP_SNAKE  2
#define CP_HEAD   3
#define CP_FOOD   4
#define CP_STATUS 5
#define CP_TITLE  6
#define CP_DEAD   7

typedef struct { int x, y; } Pt;

static Pt  snake[MAX_SNAKE];
static int slen;
static int dx, dy;
static Pt  food;
static int score;

static void place_food(void) {
    for (int tries = 0; tries < 2000; tries++) {
        int fx = 1 + rand() % (PW - 1);
        int fy = 1 + rand() % (PH - 2);   /* avoid score row */
        bool hit = false;
        for (int i = 0; i < slen; i++)
            if (snake[i].x == fx && snake[i].y == fy) { hit = true; break; }
        if (!hit) { food.x = fx; food.y = fy; return; }
    }
}

static void draw_frame(WINDOW *w) {
    /* border */
    wattron(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
    box(w, '|', '-');
    /* title centered on top border row */
    wattroff(w, COLOR_PAIR(CP_BORDER) | A_BOLD);
    wattron(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(w, 0, (FW - 9) / 2, " SNAKE ");
    wattroff(w, COLOR_PAIR(CP_TITLE) | A_BOLD);
}

static void draw_score(WINDOW *w) {
    wattron(w, COLOR_PAIR(CP_STATUS));
    mvwprintw(w, FH - 1, 1, " Score: %-5d  Arrow: move  Q: quit  R: restart ", score);
    wattroff(w, COLOR_PAIR(CP_STATUS));
}

static void draw_food(WINDOW *w) {
    wattron(w, COLOR_PAIR(CP_FOOD) | A_BOLD);
    /* +1 offsets for border */
    mvwaddch(w, food.y + 1, food.x + 1, '*');
    wattroff(w, COLOR_PAIR(CP_FOOD) | A_BOLD);
}

static void draw_snake(WINDOW *w) {
    wattron(w, COLOR_PAIR(CP_HEAD) | A_BOLD);
    mvwaddch(w, snake[0].y + 1, snake[0].x + 1, 'O');
    wattroff(w, COLOR_PAIR(CP_HEAD) | A_BOLD);
    wattron(w, COLOR_PAIR(CP_SNAKE));
    for (int i = 1; i < slen; i++)
        mvwaddch(w, snake[i].y + 1, snake[i].x + 1, 'o');
    wattroff(w, COLOR_PAIR(CP_SNAKE));
}

void snake_main(void) {
    srand((unsigned)time(NULL));

    initscr();
    start_color();
    noecho();
    cbreak();
    curs_set(0);

    init_pair(CP_BORDER, COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_SNAKE,  COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_HEAD,   COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_FOOD,   COLOR_RED,     COLOR_BLACK);
    init_pair(CP_STATUS, COLOR_YELLOW,  COLOR_BLACK);
    init_pair(CP_TITLE,  COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_DEAD,   COLOR_RED,     COLOR_BLACK);

    /* center field on 80x24 screen */
    int top  = (LINES - FH) / 2;
    int left = (COLS  - FW) / 2;
    if (top  < 0) top  = 0;
    if (left < 0) left = 0;

    WINDOW *w = newwin(FH, FW, top, left);
    keypad(w, true);
    wtimeout(w, 180);   /* game tick: 180ms; ERR returned if no key */

restart:
    slen = 5;
    for (int i = 0; i < slen; i++) {
        snake[i].x = PW / 2 - i;
        snake[i].y = PH / 2;
    }
    dx = 1; dy = 0;
    score = 0;
    place_food();

    bool game_over = false;
    while (!game_over) {
        /* draw everything into field window only */
        wclear(w);
        draw_frame(w);
        draw_food(w);
        draw_snake(w);
        draw_score(w);
        wrefresh(w);   /* single wrefresh — no stdscr involved */

        int ch = wgetch(w);   /* returns ERR on 180ms timeout = tick */
        switch (ch) {
        case KEY_UP:    if (dy != 1)  { dx = 0;  dy = -1; } break;
        case KEY_DOWN:  if (dy != -1) { dx = 0;  dy =  1; } break;
        case KEY_LEFT:  if (dx != 1)  { dx = -1; dy =  0; } break;
        case KEY_RIGHT: if (dx != -1) { dx =  1; dy =  0; } break;
        case 'q': case 'Q': game_over = true; continue;
        case 'r': case 'R': goto restart;
        /* ERR = timeout: fall through and advance game */
        default: break;
        }

        /* advance snake */
        Pt head = { snake[0].x + dx, snake[0].y + dy };

        /* wall collision */
        if (head.x < 0 || head.x >= PW || head.y < 0 || head.y >= PH - 1) {
            game_over = true; break;
        }
        /* self collision */
        for (int i = 1; i < slen - 1; i++) {
            if (snake[i].x == head.x && snake[i].y == head.y) {
                game_over = true; break;
            }
        }
        if (game_over) break;

        bool ate = (head.x == food.x && head.y == food.y);
        if (!ate && slen > 1) slen--;
        if (slen < MAX_SNAKE - 1) {
            memmove(&snake[1], &snake[0], slen * sizeof(Pt));
            slen++;
        }
        snake[0] = head;
        if (ate) { score += 10; place_food(); }
    }

    /* game over */
    wclear(w);
    draw_frame(w);
    wattron(w, COLOR_PAIR(CP_DEAD) | A_BOLD);
    mvwprintw(w, FH / 2 - 1, (FW - 13) / 2, "  GAME OVER!  ");
    mvwprintw(w, FH / 2,     (FW - 18) / 2, "  Score: %-5d       ", score);
    mvwprintw(w, FH / 2 + 1, (FW - 22) / 2, "  R = restart   Q = quit  ");
    wattroff(w, COLOR_PAIR(CP_DEAD) | A_BOLD);
    wrefresh(w);

    wtimeout(w, -1);   /* block until R or Q */
    while (1) {
        int ch = wgetch(w);
        if (ch == 'r' || ch == 'R') goto restart;
        if (ch == 'q' || ch == 'Q') break;
    }

    delwin(w);
    endwin();
}
