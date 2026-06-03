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
- Zero npm / framework dependencies

## Commands

| Command  | Description                        |
|----------|------------------------------------|
| `help`   | List available commands            |
| `clear`  | Clear the screen                   |
| `colors` | Display the 8-colour palette       |
| `demo`   | Animated colour bar sweep (any key to stop) |
| `about`  | About this terminal                |

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
  app.c          — Built-in application (banner, prompt, command dispatch)
web/
  index.html     — Canvas element, loads WASM glue and terminal.js
  terminal.js    — WASM initialisation, canvas renderer, keyboard input
  style.css      — Phosphor green theme, CRT bezel styling
  VT323-Regular.woff2 — Self-hosted font (OFL licensed)
Makefile         — Emscripten build
```

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
