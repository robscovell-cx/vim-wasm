#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <emscripten.h>
#include "terminal.h"

/* forward declarations */
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

/* ---- colour helpers ---- */
#define ESC "\x1B"
#define CSI ESC "["

static void sgr(const char *params) {
    app_printf(CSI "%sm", params);
}

static void move_to(int row, int col) { /* 1-based */
    app_printf(CSI "%d;%dH", row, col);
}

static void clear_screen(void) {
    app_puts(CSI "2J" CSI "H");
}

/* ---- line input buffer ---- */
#define LINE_BUF_SZ 256
static char line_buf[LINE_BUF_SZ];
static int  line_len = 0;

/* ---- demo state ---- */
static int demo_active = 0;
static int demo_frame  = 0;

/* ---- command history ---- */
#define HIST_SZ 8
static char history[HIST_SZ][LINE_BUF_SZ];
static int  hist_head = 0;   /* next write slot */
static int  hist_cnt  = 0;   /* number of valid entries */
static int  hist_idx  = -1;  /* -1 = not browsing */

static void hist_push(const char *s) {
    if (strlen(s) == 0) return;
    strncpy(history[hist_head], s, LINE_BUF_SZ - 1);
    hist_head = (hist_head + 1) % HIST_SZ;
    if (hist_cnt < HIST_SZ) hist_cnt++;
    hist_idx = -1;
}

/* ---- prompt ---- */
static void print_prompt(void) {
    sgr("0;32");
    app_puts("vt200> ");
    sgr("0");
}

/* ---- command implementations ---- */

static void cmd_help(void) {
    app_puts("\r\n");
    sgr("1;32");
    app_puts("Available commands:\r\n");
    sgr("0;32");
    app_puts("  help    - Show this help\r\n");
    app_puts("  clear   - Clear the screen\r\n");
    app_puts("  colors  - Show colour palette\r\n");
    app_puts("  demo    - Start colour animation (press any key to stop)\r\n");
    app_puts("  about   - About this terminal\r\n");
    sgr("0");
}

static void cmd_clear(void) {
    clear_screen();
}

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
    /* trim leading whitespace */
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "help") == 0)   { cmd_help();   return; }
    if (strcmp(cmd, "clear") == 0)  { cmd_clear();  return; }
    if (strcmp(cmd, "colors") == 0) { cmd_colors(); return; }
    if (strcmp(cmd, "about") == 0)  { cmd_about();  return; }
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

/* ---- startup banner ---- */

void app_init(void) {
    clear_screen();

    /* Box */
    sgr("1;32");
    app_puts("+" );
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

/* ---- app_tick (called from JS rAF loop) ---- */

EMSCRIPTEN_KEEPALIVE void app_tick(void) {
    if (!demo_active) return;
    demo_frame++;

    /* animate colour bars across the full screen */
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
}

/* ---- keyboard input ---- */

static void erase_line_display(void) {
    /* erase typed characters on screen: backspace over each */
    for (int i = 0; i < line_len; i++)
        app_puts("\x08 \x08");
}

void app_handle_key(const char *key) {
    if (demo_active) {
        demo_active = 0;
        /* restore prompt */
        clear_screen();
        app_init();
        /* reset line buf */
        line_len = 0;
        return;
    }

    hist_idx = -1;  /* any new key resets history browsing */

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
        if (line_len > 0) {
            line_len--;
            app_puts("\x08 \x08");
        }
        return;
    }

    if (strcmp(key, "ArrowUp") == 0) {
        if (hist_cnt == 0) return;
        if (hist_idx == -1) hist_idx = 0;
        else if (hist_idx < hist_cnt - 1) hist_idx++;
        /* compute actual ring slot */
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
            line_len = 0;
            hist_idx = -1;
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

    /* ignore non-printable multi-char key names */
    if (strlen(key) != 1) return;

    char c = key[0];
    if (c >= 0x20 && c < 0x7F && line_len < LINE_BUF_SZ - 1) {
        line_buf[line_len++] = c;
        /* echo the character */
        char echo[2] = { c, '\0' };
        app_puts(echo);
    }
}
