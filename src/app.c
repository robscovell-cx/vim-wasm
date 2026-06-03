#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <emscripten.h>
#include "terminal.h"

void terminal_write(const char *data, int len);

static void app_puts(const char *s) {
    terminal_write(s, (int)strlen(s));
}

static void app_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    app_puts(buf);
}

#define ESC "\x1B"
#define CSI ESC "["

static void sgr(const char *params) { app_printf(CSI "%sm", params); }
static void move_to(int row, int col) { app_printf(CSI "%d;%dH", row, col); }
static void clear_screen(void) { app_puts(CSI "2J" CSI "H"); }

/* ================================================================
 * Normal shell state
 * ================================================================ */

#define LINE_BUF_SZ 256
static char line_buf[LINE_BUF_SZ];
static int  line_len = 0;

static int demo_active = 0;
static int demo_frame  = 0;

#define HIST_SZ 8
static char history[HIST_SZ][LINE_BUF_SZ];
static int  hist_head = 0;
static int  hist_cnt  = 0;
static int  hist_idx  = -1;

static void hist_push(const char *s) {
    if (strlen(s) == 0) return;
    strncpy(history[hist_head], s, LINE_BUF_SZ - 1);
    hist_head = (hist_head + 1) % HIST_SZ;
    if (hist_cnt < HIST_SZ) hist_cnt++;
    hist_idx = -1;
}

static void print_prompt(void) {
    sgr("0;32");
    app_puts("vt200> ");
    sgr("0");
}

/* ================================================================
 * WOPR — War Operation Plan Response
 * Typewriter + pause engine driven by app_tick() (called each rAF)
 * ================================================================ */

static int wg_active     = 0;
static int wg_input_mode = 0;

/* Typewriter state */
static const char *tw_s   = NULL;
static int         tw_len = 0;
static int         tw_p   = 0;
static int         tw_spd = 2;
static int         tw_cnt = 0;
static void (*tw_cb)(void) = NULL;

/* Pause state */
static int tw_pdly = 0;
static void (*tw_pcb)(void) = NULL;

/* WOPR input line buffer */
#define WG_LINE 128
static char wg_ln[WG_LINE];
static int  wg_ll = 0;
static void (*wg_lcb)(const char *) = NULL;

static void wopr_print(const char *s, int spd, void (*cb)(void)) {
    tw_s = s; tw_len = (int)strlen(s);
    tw_p = 0; tw_spd = spd; tw_cnt = 0; tw_cb = cb;
}

static void wopr_wait(int d, void (*cb)(void)) {
    tw_pdly = d; tw_pcb = cb;
}

static void wopr_input(void (*cb)(const char *)) {
    wg_input_mode = 1;
    wg_ll = 0; wg_ln[0] = '\0';
    wg_lcb = cb;
}

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ---- forward declarations ---- */
static void wg_conn_pause(void);
static void wg_conn_header(void);
static void wg_login_show(void);
static void wg_login_wait(void);
static void wg_on_login(const char *s);
static void wg_greet(void);
static void wg_menu_show(void);
static void wg_menu_wait(void);
static void wg_on_selection(const char *s);
static void wg_war_1(void);
static void wg_war_2(void);
static void wg_war_3(void);
static void wg_war_4(void);
static void wg_war_5(void);
static void wg_war_6(void);
static void wg_war_7(void);
static void wg_war_winner_pause(void);
static void wg_war_winner(void);
static void wg_end_1(void);
static void wg_end_2(void);
static void wg_end_3(void);
static void wg_end_4(void);
static void wg_end_5(void);
static void wg_end_chess_offer(void);
static void wg_chess_show(void);
static void wg_chess_wait(void);
static void wg_on_chess(const char *s);
static void wg_chess_again(void);
static void wg_chess_replay(const char *s);
static void wg_exit(void);

/* ---- scene strings ---- */

static const char CONN_HEADER[] =
    "\r\n\r\n"
    "WOPR ONLINE.\r\n"
    "WAR OPERATION PLAN RESPONSE\r\n"
    "NORAD CHEYENNE MOUNTAIN COMPLEX\r\n"
    "\r\n";

static const char GREETING[] =
    "\r\nHELLO, PROFESSOR FALKEN.\r\n"
    "\r\n"
    "SHALL WE PLAY A GAME?\r\n";

static const char GAME_MENU[] =
    "\r\n"
    "  CHESS\r\n"
    "  POKER\r\n"
    "  FIGHTER COMBAT\r\n"
    "  GUERRILLA ENGAGEMENT\r\n"
    "  DESK SET\r\n"
    "  THEATERWIDE TACTICAL WARFARE\r\n"
    "  THEATERWIDE BIOTOXIC AND CHEMICAL WARFARE\r\n"
    "  GLOBAL THERMONUCLEAR WAR\r\n"
    "\r\n"
    "SELECT: ";

static const char WAR_INIT[] =
    "\r\nINITIATING SIMULATION.\r\n"
    "\r\n"
    "PREPARING LAUNCH CODES...\r\n"
    "\r\n"
    "FIRST STRIKE SEQUENCE ACTIVE.\r\n";

static const char US_TARGETS[] =
    "\r\nUNITED STATES FIRST STRIKE:\r\n"
    "  MOSCOW            . . . [TARGETED]\r\n"
    "  LENINGRAD         . . . [TARGETED]\r\n"
    "  KIEV              . . . [TARGETED]\r\n"
    "  SVERDLOVSK        . . . [TARGETED]\r\n"
    "  MINSK             . . . [TARGETED]\r\n"
    "  TASHKENT          . . . [TARGETED]\r\n"
    "  NOVOSIBIRSK       . . . [TARGETED]\r\n"
    "  KHARKOV           . . . [TARGETED]\r\n"
    "  GORKY             . . . [TARGETED]\r\n"
    "  BAKU              . . . [TARGETED]\r\n";

static const char USSR_TARGETS[] =
    "\r\nUSSR RETALIATORY STRIKE:\r\n"
    "  NEW YORK, NY      . . . [TARGETED]\r\n"
    "  WASHINGTON, DC    . . . [TARGETED]\r\n"
    "  CHICAGO, IL       . . . [TARGETED]\r\n"
    "  LOS ANGELES, CA   . . . [TARGETED]\r\n"
    "  SEATTLE, WA       . . . [TARGETED]\r\n"
    "  HOUSTON, TX       . . . [TARGETED]\r\n"
    "  BOSTON, MA        . . . [TARGETED]\r\n"
    "  DETROIT, MI       . . . [TARGETED]\r\n"
    "  DENVER, CO        . . . [TARGETED]\r\n"
    "  SAN FRANCISCO, CA . . . [TARGETED]\r\n";

static const char CHESS_BOARD[] =
    "\r\n"
    "     A   B   C   D   E   F   G   H\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 8 | r | n | b | q | k | b | n | r |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 7 | p | p | p | p | p | p | p | p |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 6 |   |   |   |   |   |   |   |   |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 5 |   |   |   |   |   |   |   |   |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 4 |   |   |   |   |   |   |   |   |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 3 |   |   |   |   |   |   |   |   |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 2 | P | P | P | P | P | P | P | P |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    " 1 | R | N | B | Q | K | B | N | R |\r\n"
    "   +---+---+---+---+---+---+---+---+\r\n"
    "\r\n"
    "YOUR MOVE (E.G. E2-E4): ";

/* ---- scenes ---- */

static void wg_start(void) {
    wg_active = 1;
    wg_input_mode = 0;
    tw_s = NULL; tw_pdly = 0;
    clear_screen();
    sgr("0;32");
    wopr_print("CONNECTING TO WOPR...", 2, wg_conn_pause);
}
/* wg_conn_pause defined after forward decls — can't forward-declare statics
   without prototypes, so define inline here using the pause trick */
static void wg_conn_pause(void)  { wopr_wait(50, wg_conn_header); }
static void wg_conn_header(void) { sgr("1;32"); wopr_print(CONN_HEADER, 2, wg_login_show); }

static void wg_login_show(void) {
    sgr("0;32");
    wopr_print("LOGON: ", 2, wg_login_wait);
}

static void wg_login_wait(void) { wopr_input(wg_on_login); }

static void wg_on_login(const char *s) {
    if (ci_eq(s, "JOSHUA") || ci_eq(s, "FALKEN")) {
        wopr_wait(30, wg_greet);
    } else if (ci_eq(s, "HELP") || ci_eq(s, "?")) {
        wopr_print(
            "\r\nHINT: A BACKDOOR EXISTS. THINK ABOUT WHO BUILT THIS SYSTEM.\r\n\r\n"
            "LOGON: ",
            2, wg_login_wait
        );
    } else {
        wopr_print("\r\nACCESS DENIED.\r\n\r\nLOGON: ", 2, wg_login_wait);
    }
}

static void wg_greet(void) {
    sgr("1;32");
    wopr_print(GREETING, 5, wg_menu_show);
}

static void wg_menu_show(void) {
    sgr("0;32");
    wopr_print(GAME_MENU, 2, wg_menu_wait);
}

static void wg_menu_wait(void) { wopr_input(wg_on_selection); }

static void wg_on_selection(const char *s) {
    if (ci_eq(s, "GLOBAL THERMONUCLEAR WAR") ||
        ci_eq(s, "THERMONUCLEAR WAR")        ||
        ci_eq(s, "THERMONUCLEAR")            ||
        ci_eq(s, "WAR")) {
        wg_war_1();
    } else if (ci_eq(s, "CHESS")) {
        wg_chess_show();
    } else if (strlen(s) == 0) {
        wg_menu_show();
    } else {
        wopr_print("\r\nSELECTION NOT AVAILABLE.\r\n", 2, wg_menu_show);
    }
}

/* ---- Global Thermonuclear War sequence ---- */

static void wg_war_1(void) {
    sgr("0;32");
    wopr_print(WAR_INIT, 3, wg_war_2);
}
static void wg_war_2(void) { wopr_wait(60, wg_war_3); }
static void wg_war_3(void) { wopr_print(US_TARGETS,   1, wg_war_4); }
static void wg_war_4(void) { wopr_wait(40, wg_war_5); }
static void wg_war_5(void) { wopr_print(USSR_TARGETS, 1, wg_war_6); }
static void wg_war_6(void) { wopr_wait(60, wg_war_7); }

static void wg_war_7(void) {
    sgr("1;31");
    wopr_print("\r\nESTIMATED CASUALTIES: 3,348,000,000\r\n", 4, wg_war_winner_pause);
}
static void wg_war_winner_pause(void) { wopr_wait(90, wg_war_winner); }
static void wg_war_winner(void) {
    wopr_print("WINNER: NONE\r\n", 5, wg_end_1);
}

/* ---- Endgame ---- */

static void wg_end_1(void)  { wopr_wait(150, wg_end_2); }
static void wg_end_2(void) {
    sgr("1;32");
    wopr_print("\r\nA STRANGE GAME.\r\n", 10, wg_end_3);
}
static void wg_end_3(void)  { wopr_wait(90, wg_end_4); }
static void wg_end_4(void) {
    wopr_print("THE ONLY WINNING MOVE IS NOT TO PLAY.\r\n", 10, wg_end_5);
}
static void wg_end_5(void)  { wopr_wait(120, wg_end_chess_offer); }
static void wg_end_chess_offer(void) {
    sgr("0;32");
    wopr_print("\r\nHOW ABOUT A NICE GAME OF CHESS?\r\n\r\n", 6, wg_exit);
}

/* ---- Chess ---- */

static void wg_chess_show(void) {
    sgr("0;32");
    wopr_print(CHESS_BOARD, 1, wg_chess_wait);
}
static void wg_chess_wait(void) { wopr_input(wg_on_chess); }
static void wg_on_chess(const char *s) {
    (void)s;
    app_puts("\r\n");
    wopr_print("INTERESTING MOVE. SHALL WE PLAY AGAIN? (YES/NO): ", 3, wg_chess_again);
}
static void wg_chess_again(void) { wopr_input(wg_chess_replay); }
static void wg_chess_replay(const char *s) {
    if (ci_eq(s, "YES") || ci_eq(s, "Y")) {
        app_puts("\r\n");
        wg_chess_show();
    } else {
        app_puts("\r\n");
        wg_exit();
    }
}

static void wg_exit(void) {
    wg_active = 0;
    sgr("0");
    print_prompt();
}

/* ---- WOPR keyboard input handler ---- */

static void wg_handle_key(const char *key) {
    if (!wg_input_mode) {
        /* Escape aborts WarGames entirely */
        if (strcmp(key, "Escape") == 0) {
            wg_active = 0;
            tw_s = NULL; tw_pdly = 0;
            app_puts("\r\n");
            sgr("0");
            print_prompt();
        }
        return;
    }

    if (strcmp(key, "Enter") == 0) {
        wg_ln[wg_ll] = '\0';
        wg_input_mode = 0;
        app_puts("\r\n");
        void (*cb)(const char *) = wg_lcb;
        wg_lcb = NULL;
        if (cb) cb(wg_ln);
        return;
    }

    if (strcmp(key, "Backspace") == 0) {
        if (wg_ll > 0) { wg_ll--; app_puts("\x08 \x08"); }
        return;
    }

    if (strlen(key) == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') c -= 32;  /* WOPR echoes uppercase */
        if (c >= 0x20 && c < 0x7F && wg_ll < WG_LINE - 1) {
            wg_ln[wg_ll++] = c;
            char echo[2] = {c, '\0'};
            app_puts(echo);
        }
    }
}

/* ================================================================
 * Normal shell commands
 * ================================================================ */

static void cmd_help(void) {
    app_puts("\r\n");
    sgr("1;32");
    app_puts("Available commands:\r\n");
    sgr("0;32");
    app_puts("  help      - Show this help\r\n");
    app_puts("  clear     - Clear the screen\r\n");
    app_puts("  colors    - Show colour palette\r\n");
    app_puts("  demo      - Start colour animation (press any key to stop)\r\n");
    app_puts("  wargames  - Connect to WOPR\r\n");
    app_puts("  about     - About this terminal\r\n");
    sgr("0");
}

static void cmd_clear(void) { clear_screen(); }

static void cmd_colors(void) {
    app_puts("\r\n");
    sgr("1;32");
    app_puts("Colour palette:\r\n");
    sgr("0");

    const char *names[] = {
        "Black  ", "Red    ", "Green  ", "Yellow ",
        "Blue   ", "Magenta", "Cyan   ", "White  "
    };
    for (int i = 0; i < 8; i++) {
        char fg_param[16], bg_param[16];
        snprintf(fg_param, sizeof(fg_param), "0;%d", 30 + i);
        snprintf(bg_param, sizeof(bg_param), "0;%d;%d", 37, 40 + i);
        sgr(fg_param);
        app_printf("  FG %d %s  ", i, names[i]);
        sgr(bg_param);
        app_printf(" BG %d %s ", i, names[i]);
        sgr("0");
        app_puts("\r\n");
    }
}

static void cmd_about(void) {
    app_puts("\r\n");
    sgr("1;32");
    app_puts("  VT-200 Terminal Emulator\r\n");
    sgr("0;32");
    app_puts("  Built with C + WebAssembly\r\n");
    app_puts("  Compiled with Emscripten\r\n");
    app_puts("  Rendered on HTML5 Canvas\r\n");
    app_puts("  Font: VT323 (OFL)\r\n");
    sgr("0");
}

static void cmd_unknown(const char *cmd) {
    app_printf("\r\n" CSI "31munknown command: %s" CSI "0m\r\n", cmd);
}

static void dispatch_command(const char *cmd) {
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "help") == 0)      { cmd_help();    return; }
    if (strcmp(cmd, "clear") == 0)     { cmd_clear();   return; }
    if (strcmp(cmd, "colors") == 0)    { cmd_colors();  return; }
    if (strcmp(cmd, "about") == 0)     { cmd_about();   return; }
    if (strcmp(cmd, "wargames") == 0)  { wg_start();    return; }
    if (strcmp(cmd, "demo") == 0) {
        demo_active = 1;
        demo_frame  = 0;
        app_puts("\r\n");
        sgr("1;32");
        app_puts("[demo mode - press any key to exit]\r\n");
        sgr("0");
        return;
    }
    if (strlen(cmd) == 0) return;
    cmd_unknown(cmd);
}

/* ================================================================
 * Startup banner
 * ================================================================ */

void app_init(void) {
    clear_screen();

    sgr("1;32");
    app_puts("+");
    for (int i = 0; i < 46; i++) app_puts("-");
    app_puts("+\r\n");

    app_puts("|");
    sgr("0;32");
    app_puts("       DEC VT-200 Terminal Emulator            ");
    sgr("1;32");
    app_puts("|\r\n");

    app_puts("|");
    sgr("0;32");
    app_puts("          C + WebAssembly Edition              ");
    sgr("1;32");
    app_puts("|\r\n");

    app_puts("+");
    for (int i = 0; i < 46; i++) app_puts("-");
    app_puts("+\r\n");
    sgr("0");

    app_puts("\r\n");
    sgr("32");
    app_puts("Type ");
    sgr("1;32");
    app_puts("help");
    sgr("0;32");
    app_puts(" to see available commands.\r\n\r\n");
    sgr("0");

    print_prompt();
}

/* ================================================================
 * app_tick — called every requestAnimationFrame from JS
 * ================================================================ */

EMSCRIPTEN_KEEPALIVE void app_tick(void) {
    /* Demo colour animation */
    if (demo_active) {
        demo_frame++;
        for (int row = 1; row <= TERM_ROWS; row++) {
            move_to(row, 1);
            for (int col = 0; col < TERM_COLS; col++) {
                int color_idx = ((col + demo_frame + row * 3) / 10) % 8;
                char param[16];
                snprintf(param, sizeof(param), "4%d", color_idx);
                sgr(param);
                app_puts(" ");
            }
        }
        sgr("0");
        return;
    }

    if (!wg_active) return;

    /* WOPR pause countdown */
    if (tw_pdly > 0) {
        tw_pdly--;
        if (tw_pdly == 0 && tw_pcb) {
            void (*cb)(void) = tw_pcb;
            tw_pcb = NULL;
            cb();
        }
        return;
    }

    /* WOPR typewriter */
    if (tw_s && tw_p < tw_len) {
        tw_cnt++;
        if (tw_cnt >= tw_spd) {
            tw_cnt = 0;
            char buf[2] = {tw_s[tw_p++], '\0'};
            terminal_write(buf, 1);

            if (tw_p >= tw_len) {
                tw_s = NULL;
                void (*cb)(void) = tw_cb;
                tw_cb = NULL;
                if (cb) cb();
            }
        }
    }
}

/* ================================================================
 * Keyboard input — dispatches to WOPR or normal shell
 * ================================================================ */

static void erase_line_display(void) {
    for (int i = 0; i < line_len; i++)
        app_puts("\x08 \x08");
}

void app_handle_key(const char *key) {
    if (demo_active) {
        demo_active = 0;
        clear_screen();
        app_init();
        line_len = 0;
        return;
    }

    if (wg_active) {
        wg_handle_key(key);
        return;
    }

    hist_idx = -1;

    if (strcmp(key, "Enter") == 0) {
        line_buf[line_len] = '\0';
        hist_push(line_buf);
        app_puts("\r\n");
        dispatch_command(line_buf);
        app_puts("\r\n");
        line_len = 0;
        print_prompt();
        return;
    }

    if (strcmp(key, "Backspace") == 0) {
        if (line_len > 0) { line_len--; app_puts("\x08 \x08"); }
        return;
    }

    if (strcmp(key, "ArrowUp") == 0) {
        if (hist_cnt == 0) return;
        if (hist_idx == -1) hist_idx = 0;
        else if (hist_idx < hist_cnt - 1) hist_idx++;
        int slot = ((hist_head - 1 - hist_idx) % HIST_SZ + HIST_SZ) % HIST_SZ;
        erase_line_display();
        strcpy(line_buf, history[slot]);
        line_len = (int)strlen(line_buf);
        app_puts(line_buf);
        return;
    }

    if (strcmp(key, "ArrowDown") == 0) {
        if (hist_idx <= 0) {
            erase_line_display();
            line_len = 0; hist_idx = -1;
            return;
        }
        hist_idx--;
        int slot = ((hist_head - 1 - hist_idx) % HIST_SZ + HIST_SZ) % HIST_SZ;
        erase_line_display();
        strcpy(line_buf, history[slot]);
        line_len = (int)strlen(line_buf);
        app_puts(line_buf);
        return;
    }

    if (strlen(key) != 1) return;

    char c = key[0];
    if (c >= 0x20 && c < 0x7F && line_len < LINE_BUF_SZ - 1) {
        line_buf[line_len++] = c;
        char echo[2] = {c, '\0'};
        app_puts(echo);
    }
}
