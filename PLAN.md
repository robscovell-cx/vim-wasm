# VT-200 Terminal Emulator in C + WebAssembly

## Context

Build a browser-based terminal emulator that looks and behaves like a DEC VT-220. No real shell — a built-in C "application" handles keyboard input and generates VT-200 escape sequences that drive the terminal. Implemented in C, compiled to WebAssembly via Emscripten, rendered to HTML5 Canvas in JavaScript. Zero npm/framework dependencies.

---

## File Structure

```
vim-wasm/
├── Makefile
├── src/
│   ├── terminal.h        # Public API + data structures
│   ├── terminal.c        # VT-200 state machine + screen buffer
│   └── app.c             # Built-in demo app (keyboard → escape sequences)
└── web/
    ├── index.html        # Canvas element, loads WASM glue + terminal.js
    ├── terminal.js       # JS: WASM init, canvas rendering, keyboard input
    ├── style.css         # Phosphor green theme, box glow, scanlines
    └── VT323-Regular.woff2   # Self-hosted font (OFL licensed, DEC look)
```

Build outputs land in `web/`: `terminal_wasm.js` and `terminal_wasm.wasm`.

---

## Core Data Structures (`terminal.h`)

```c
#define TERM_COLS 80
#define TERM_ROWS 24

#define ATTR_BOLD      0x01
#define ATTR_UNDERLINE 0x02
#define ATTR_REVERSE   0x04
#define ATTR_BLINK     0x08

typedef struct {
    uint32_t ch;    // Unicode codepoint
    uint8_t  fg;    // foreground colour index 0–7 (8 = default)
    uint8_t  bg;    // background colour index 0–7 (8 = default)
    uint8_t  attr;  // ATTR_* bit flags
    uint8_t  _pad;
} Cell;             // 8 bytes per cell

typedef enum {
    STATE_GROUND, STATE_ESCAPE, STATE_CSI_ENTRY,
    STATE_CSI_PARAM, STATE_CSI_IGNORE, STATE_DCS, STATE_OSC
} ParseState;

typedef struct {
    Cell       cells[TERM_ROWS][TERM_COLS];
    int        cx, cy;            // cursor position (0-based)
    int        saved_cx, saved_cy;
    uint8_t    cur_fg, cur_bg, cur_attr;  // current SGR state
    ParseState state;
    int        params[16];
    int        param_idx, param_cur;
    int        dirty;
} Terminal;
```

Cell buffer is 1920 × 8 = 15,360 bytes. JS reads it directly from WASM linear memory via typed array pointer — zero-copy.

---

## WASM Exported Functions

```c
EMSCRIPTEN_KEEPALIVE void      term_init(void);
EMSCRIPTEN_KEEPALIVE uintptr_t term_get_cells(void);   // pointer to cells[0][0]
EMSCRIPTEN_KEEPALIVE int       term_cell_size(void);   // returns sizeof(Cell) = 8
EMSCRIPTEN_KEEPALIVE int       term_is_dirty(void);
EMSCRIPTEN_KEEPALIVE void      term_clear_dirty(void);
EMSCRIPTEN_KEEPALIVE int       term_cursor_col(void);
EMSCRIPTEN_KEEPALIVE int       term_cursor_row(void);
EMSCRIPTEN_KEEPALIVE void      term_send_key(const char *key);  // JS key string
EMSCRIPTEN_KEEPALIVE void      app_tick(void);         // called every rAF
EMSCRIPTEN_KEEPALIVE void      ollama_receive(const char *text);
EMSCRIPTEN_KEEPALIVE void      ollama_ready(void);
EMSCRIPTEN_KEEPALIVE void      ollama_error(const char *msg);
```

`term_send_key` is called via `Module.ccall('term_send_key', null, ['string'], [e.key])` so Emscripten handles UTF-8 stack allocation.

---

## terminal.c — VT-200 Parser

State machine processes one byte at a time through `parse_byte()`. Key sequences:

| Sequence | Action |
|---|---|
| `ESC [ H` | Cursor home |
| `ESC [ row ; col H` | Cursor position (CUP) |
| `ESC [ A/B/C/D` | Cursor up/down/forward/back |
| `ESC [ 2 J` | Erase display |
| `ESC [ K` | Erase to end of line (variants 0/1/2) |
| `ESC [ ...m` | SGR: fg/bg colours (30–37, 40–47), bold, underline, reverse, reset |
| `ESC 7` / `ESC 8` | Save / restore cursor |
| `\r`, `\n`, `\x08` | CR, LF, backspace |
| `\x07` | BEL — ignore |

Scrolling: when `cy >= TERM_ROWS`, `memmove` rows up by one, blank last row, set `cy = TERM_ROWS - 1`.

`app.c` calls `terminal_write(const char *data, int len)` directly (same WASM module, single-threaded — no ring buffer needed).

---

## app.c — Application Layer

Startup: writes a VT-200 welcome banner using escape sequences. Maintains a static line input buffer (256 bytes). Modes:

### Shell mode (`vt200> ` prompt)

| Command | Action |
|---|---|
| `help` | Print command list |
| `clear` | `ESC[2J ESC[H` + redraw prompt |
| `colors` | Print 8 colour swatches |
| `demo` | Colour animation driven by `app_tick()` |
| `wargames` | Connect to WOPR (WarGames simulation) |
| `ollama [model]` | Chat with local Ollama; `/quit` to exit |
| `about` | Version/author banner |

### WOPR mode (`wargames` command)

Typewriter + pause engine driven by `app_tick()`. Scenes are chained static C callbacks (`wg_*`). Accepts login password `JOSHUA` or `FALKEN`. Game selections: `GLOBAL THERMONUCLEAR WAR` (full simulation → "A STRANGE GAME.") or `CHESS`. Escape key aborts at any point.

### Ollama mode (`ollama [model]` command)

`cmd_ollama()` calls `EM_ASM({ Module.ollamaInit(...) })` to trigger JS fetch. Tokens stream back via `ollama_receive()` WASM callback. `/quit` exits. Conversation history maintained in JS (`ollamaHistory[]`).

### Demo mode (`demo` command)

`app_tick()` fills all 80×24 cells with cycling background colours on each rAF frame. Any keypress exits.

---

## Makefile

```makefile
CC     := emcc
CFLAGS := -O2 -Wall -Wextra -std=gnu11   # gnu11 required for EM_ASM
SRCS   := src/terminal.c src/app.c
TARGET := web/terminal_wasm.js

EXPORTS := -sEXPORTED_FUNCTIONS='[...]'
RUNTIME := -sEXPORTED_RUNTIME_METHODS='["ccall","HEAPU8","HEAP32"]'
MEM     := -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=1MB

all: $(TARGET)
clean:
    rm -f web/terminal_wasm.js web/terminal_wasm.wasm
serve:
    python3 -m http.server 8080 --directory web
```

`--no-entry`: no `main()`, JS drives init via `onRuntimeInitialized`. Fixed 1 MB — sufficient for all buffers. `-std=gnu11` required because `EM_ASM` uses GCC statement-expression extensions.

---

## JavaScript Rendering (terminal.js)

```javascript
var Module = { onRuntimeInitialized: initTerminal };

function measureAndInit() {
    // canvas.width/height assignment resets 2D context — set textBaseline INSIDE render()
    ctx.font = '16px "VT323", monospace';
    CELL_W = Math.ceil(ctx.measureText('M').width);
    canvas.width  = CELL_W * 80;
    canvas.height = 16 * 24;
    Module._term_init();
    cellBufPtr = Module._term_get_cells();
}

function render() {
    ctx.textBaseline = 'top';   // must be set here — resets with canvas resize
    // Full 80×24 redraw via direct WASM memory read:
    base = cellBufPtr + (row*80+col)*8;
    ch   = Module.HEAP32[base >> 2];
    fg   = Module.HEAPU8[base + 4];
    bg   = Module.HEAPU8[base + 5];
    attr = Module.HEAPU8[base + 6];
    // draw bg rect → fillText → underline → cursor (difference blend) → scanlines
}
```

---

## Ollama Integration (terminal.js)

```javascript
Module.ollamaInit = async (model) => {
    // GET /api/tags → resolve model name → ollama_receive("Connected...") → ollama_ready()
};

Module.ollamaSend = async (userMsg) => {
    // POST /api/chat {model, messages, stream:true}
    // stream NDJSON → parse token → ollama_receive(token.replace(/\n/g,'\r\n'))
    // on done:true → ollamaHistory.push(assistant msg) → ollama_ready()
};
```

Requires Ollama running at `http://localhost:11434`. Conversation history kept in JS `ollamaHistory[]` array for multi-turn context.

---

## Colour Palette (Phosphor Green)

```javascript
const FG_PALETTE      = ['#1a3a1a','#cc4422','#33ff33','#ccff44','#44aa66','#44ddaa','#88ffcc','#ccffcc','#33ff33'];
const BG_PALETTE      = ['#001100','#1a0500','#002200','#111a00','#001100','#001a0d','#001a11','#0a1a0a','#001100'];
const FG_BOLD_PALETTE = ['#2a4a2a','#ff6644','#88ff88','#ffff66','#66cc88','#66ffcc','#aaffee','#eeffee','#88ff88'];
```

Default fg = phosphor green (`#33ff33`), default bg = near-black (`#001100`).

---

## Styling (style.css)

- Body: `background: #0a0a0a`, flex-centered
- `#bezel`: dark grey border with inset shadow
- `#screen`: multi-layer `box-shadow` for phosphor bloom glow; `::after` glass reflection overlay
- `canvas`: `cursor: none`, `image-rendering: pixelated`
- Font: VT323 self-hosted via `@font-face` from `web/VT323-Regular.woff2`

---

## Known Gotchas

- `canvas.width = ...` resets the entire 2D context state (font, textBaseline, etc.) — always set `ctx.textBaseline = 'top'` at the top of `render()`, not during init.
- `EM_ASM` requires `-std=gnu11`; the macro uses GCC statement-expression extensions incompatible with strict `-std=c11`.
- WASM served over HTTP only — browsers block `.wasm` loaded from `file://` URIs.
- Ollama CORS: requests from `localhost:8080` to `localhost:11434` are allowed by default on recent Ollama versions.

---

## Verification

1. `emcc --version` — confirm Emscripten ≥ 5.x
2. `make all` — builds `web/terminal_wasm.js` + `.wasm`
3. `make serve` — Python3 HTTP server on `localhost:8080`
4. Open browser and check:
   - Welcome banner renders in green phosphor on black
   - `help` → command list; `clear` → screen clears; `colors` → swatches
   - `demo` → animated colour bars; any key exits
   - `wargames` → WOPR connection sequence; login with `JOSHUA`; select `GLOBAL THERMONUCLEAR WAR`
   - `ollama` → connects to local Ollama; chat works; `/quit` returns to prompt
   - Cursor blinks at ~1.9 Hz, aligned with text on the same row
   - Arrow keys don't scroll the page; CRT scanlines visible
