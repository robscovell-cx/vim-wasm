# VT-200 Terminal Emulator

A DEC VT-220 terminal emulator running in the browser, built with C and WebAssembly.

## Overview

The terminal emulator is implemented as a VT-200 state machine in C, compiled to WebAssembly via Emscripten, and rendered to an HTML5 Canvas. There is no real shell — a built-in application written in C handles keyboard input and produces VT-200 escape sequences that drive the terminal.

## Features

- VT-200/ANSI escape sequence parser (cursor movement, SGR colours, erase, scrolling)
- 80×24 character grid with per-cell attributes (colour, bold, underline, reverse video)
- Phosphor green aesthetic with CRT scanline overlay and bezel glow
- Blinking block cursor at 1.9 Hz (matches DEC hardware)
- VT323 bitmap font for authentic DEC terminal output
- Command history (arrow keys), backspace editing
- A small ncurses-compatible layer (`src/curses.c`) lets real curses programs run against the VT-200 screen buffer
- Two curses apps ship in-browser: Snake and a Tetris clone (Tint), launched with `run <app>`
- A full interactive Forth interpreter (`forth` command)
- A WarGames/WOPR-style typewriter easter egg (`wargames`) and Ollama chat client (`ollama [model]`)
- Zero npm / framework dependencies

## Commands

| Command       | Description                                  |
|---------------|-----------------------------------------------|
| `help`        | List available commands                      |
| `clear`       | Clear the screen                             |
| `colors`      | Display the 8-colour palette                 |
| `demo`        | Animated colour bar sweep (any key to stop)   |
| `wargames`    | Connect to WOPR (WarGames-style typewriter)   |
| `run snake`   | Launch Snake (ncurses)                       |
| `run tint`    | Launch Tint, a Tetris clone (ncurses)         |
| `forth`       | Interactive Forth interpreter (`bye` to exit) |
| `ollama [model]` | Chat with a local Ollama server (`/quit` to exit) |
| `about`       | About this terminal                          |

## Requirements

- [Emscripten](https://emscripten.org/) (`emcc` in PATH)
- Python 3 (for the development server)

## Build

```sh
make
```

Produces `web/terminal_wasm.js` and `web/terminal_wasm.wasm`.

## Run

```sh
make serve
```

Opens a Python HTTP server on `http://localhost:8080`. A server is required — browsers block WASM loaded from `file://` URIs.

## Project Structure

```
src/
  terminal.h     — Cell struct, Terminal state machine types, public API
  terminal.c     — VT-200 parser and screen buffer management
  app.c          — Built-in application (banner, prompt, command dispatch, WOPR/Ollama modes)
  curses.h       — Minimal ncurses-compatible API (windows, colour pairs, key codes)
  curses.c       — curses implementation on top of terminal_write()/Terminal.cells
  snake.c        — Snake, written against the curses layer
  forth.c        — Standalone Forth interpreter (line-buffered shell mode)
  tint/          — Vendored "Tint" Tetris clone (curses game), patched for non-interactive WASM use
web/
  index.html     — Canvas element, loads WASM glue and terminal.js
  terminal.js    — WASM initialisation, canvas renderer, keyboard input, curses app launcher, Ollama fetch client
  style.css      — Phosphor green theme, CRT bezel styling
  VT323-Regular.woff2 — Self-hosted font (OFL licensed)
Makefile         — Emscripten build
```

### The `tint` directory

`src/tint/` is a copy of the upstream TINT ("Tint Is Not Tetris") clone (`tint.c`, `engine.c`, `io.c`, `utils.c`), built against the local `curses.h` instead of a system ncurses. Its own `getname()`/`choose_level()` prompts (which read from a real stdin) are stubbed out for the browser, and `main()` is renamed to `tint_main()` so it can be an Emscripten export invoked on demand rather than the module entry point.

## Architecture

```
Keyboard event (JS)
      │
      ▼
 term_send_key()  ─── WASM export
      │
      ▼
 app_handle_key() ─── C: line editing, command dispatch
      │
      ▼
 terminal_write() ─── C: feeds bytes into VT-200 parser
      │
      ▼
 Terminal.cells[] ─── 80×24 Cell array in WASM linear memory
      │
      ▼
 render() (JS)    ─── reads cells via HEAPU8/HEAP32, draws to Canvas
```

The cell buffer is exposed to JavaScript as a raw pointer (`term_get_cells()`), allowing direct zero-copy reads via typed arrays.

### Curses apps (Snake, Tint)

`run snake` / `run tint` hand keyboard control to a real ncurses-style event loop instead of the line-based shell:

```
"run snake" / "run tint"
      │
      ▼
Module.launchCursesApp(name)         ─── JS: async, sets cursesActive = true
      │
      ▼
ccall("snake_main"/"tint_main", {async: true})   ─── blocks inside WASM via ASYNCIFY
      │
      ▼
curses.c getch()/refresh() loop      ─── C: reads curses_push_key() queue, writes via terminal_write()
      │
      ▼
JS handleKey() ─── while cursesActive, translates keys to curses codes, calls curses_push_key()
```

Because the games run a blocking `while (running) { getch(); ... }` loop in C, the build uses Emscripten's `ASYNCIFY` (see Makefile) so that loop can yield to the browser event loop between key presses instead of freezing the tab. `tint_main`/`snake_main` return (or throw Emscripten's `ExitStatus` on `exit()`, which JS swallows) back to the normal shell prompt when the game ends.
