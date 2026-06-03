'use strict';

/* Phosphor green palette — 9 entries (0-7 = ANSI, 8 = default) */
const FG_PALETTE = [
  '#1a3a1a',  /* 0 black → dark shadow green */
  '#cc4422',  /* 1 red */
  '#33ff33',  /* 2 green = primary phosphor */
  '#ccff44',  /* 3 yellow */
  '#44aa66',  /* 4 blue → deep teal-green */
  '#44ddaa',  /* 5 magenta → cyan-green */
  '#88ffcc',  /* 6 cyan → bright cyan-green */
  '#ccffcc',  /* 7 white → bright phosphor */
  '#33ff33',  /* 8 default fg */
];

const BG_PALETTE = [
  '#001100',  /* 0 black */
  '#1a0500',  /* 1 dark red */
  '#002200',  /* 2 dark green */
  '#111a00',  /* 3 dark yellow */
  '#001100',  /* 4 dark blue → near-black */
  '#001a0d',  /* 5 dark magenta */
  '#001a11',  /* 6 dark cyan */
  '#0a1a0a',  /* 7 dark white */
  '#001100',  /* 8 default bg */
];

/* high-intensity variants (bold) */
const FG_BOLD_PALETTE = [
  '#2a4a2a',
  '#ff6644',
  '#88ff88',
  '#ffff66',
  '#66cc88',
  '#66ffcc',
  '#aaffee',
  '#eeffee',
  '#88ff88',
];

const TERM_COLS = 80;
const TERM_ROWS = 24;
const CELL_SIZE = 8;  /* sizeof(Cell) in bytes */

let canvas, ctx;
let cellBufPtr;
let CELL_W = 8;
let CELL_H = 16;
let cursorVisible = true;
let rafId = null;

function initTerminal() {
  canvas = document.getElementById('terminal');
  ctx    = canvas.getContext('2d', { alpha: false });

  /* Wait for VT323 font to load before measuring */
  document.fonts.ready.then(function() {
    measureAndInit();
  });
}

function measureAndInit() {
  /* Measure character cell dimensions with VT323 at 16px */
  ctx.font = '16px "VT323", monospace';
  const m = ctx.measureText('M');
  CELL_W = Math.ceil(m.width);
  CELL_H = 16;

  canvas.width  = CELL_W * TERM_COLS;
  canvas.height = CELL_H * TERM_ROWS;

  /* Init the WASM terminal */
  Module._term_init();
  cellBufPtr = Module._term_get_cells();

  document.addEventListener('keydown', handleKey);

  /* Cursor blink ~1.9 Hz (matches DEC hardware) */
  setInterval(function() {
    cursorVisible = !cursorVisible;
    render();
  }, 530);

  /* Start render loop */
  renderLoop();
}

function renderLoop() {
  if (Module._term_is_dirty()) {
    render();
    Module._term_clear_dirty();
  }
  Module._app_tick();
  rafId = requestAnimationFrame(renderLoop);
}

function render() {
  const heap8  = Module.HEAPU8;
  const heap32 = Module.HEAP32;

  ctx.textBaseline = 'top';

  /* Full screen background */
  ctx.fillStyle = BG_PALETTE[8];
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  let lastFg   = null;
  let lastBg   = null;
  let lastBold = null;

  for (let row = 0; row < TERM_ROWS; row++) {
    const rowY = row * CELL_H;
    for (let col = 0; col < TERM_COLS; col++) {
      const base = cellBufPtr + (row * TERM_COLS + col) * CELL_SIZE;

      /* Read cell fields from WASM linear memory */
      const ch   = heap32[base >> 2];            /* uint32_t at offset 0 */
      const fg   = heap8[base + 4];              /* uint8_t at offset 4 */
      const bg   = heap8[base + 5];              /* uint8_t at offset 5 */
      const attr = heap8[base + 6];              /* uint8_t at offset 6 */

      const reversed = (attr & 0x04) !== 0;
      const bold     = (attr & 0x01) !== 0;

      const fgIdx = fg <= 8 ? fg : 8;
      const bgIdx = bg <= 8 ? bg : 8;

      const cellFgColor = reversed ? BG_PALETTE[bgIdx] : (bold ? FG_BOLD_PALETTE[fgIdx] : FG_PALETTE[fgIdx]);
      const cellBgColor = reversed ? FG_PALETTE[fgIdx] : BG_PALETTE[bgIdx];

      const x = col * CELL_W;
      const y = rowY;

      /* Background cell */
      if (cellBgColor !== BG_PALETTE[8]) {
        ctx.fillStyle = cellBgColor;
        ctx.fillRect(x, y, CELL_W, CELL_H);
      }

      /* Character glyph */
      if (ch > 0x20) {
        const fontStr = bold ? ('bold 16px "VT323", monospace') : ('16px "VT323", monospace');
        if (fontStr !== lastBold) {
          ctx.font = fontStr;
          lastBold = fontStr;
        }
        if (cellFgColor !== lastFg) {
          ctx.fillStyle = cellFgColor;
          lastFg = cellFgColor;
        }
        ctx.fillText(String.fromCodePoint(ch), x, y);
      }

      /* Underline */
      if (attr & 0x02) {
        ctx.fillStyle = cellFgColor;
        ctx.fillRect(x, y + CELL_H - 2, CELL_W, 2);
      }
    }
  }

  /* Cursor */
  drawCursor();

  /* CRT scanlines */
  drawScanlines();
}

function drawCursor() {
  if (!cursorVisible) return;
  const col = Module._term_cursor_col();
  const row = Module._term_cursor_row();
  const x   = col * CELL_W;
  const y   = row * CELL_H;

  /* Draw a block cursor using XOR-like difference blend */
  const prev = ctx.globalCompositeOperation;
  ctx.globalCompositeOperation = 'difference';
  ctx.fillStyle = '#33ff33';
  ctx.fillRect(x, y, CELL_W, CELL_H);
  ctx.globalCompositeOperation = prev;
}

function drawScanlines() {
  ctx.fillStyle = 'rgba(0,0,0,0.15)';
  for (let y = 1; y < canvas.height; y += 2) {
    ctx.fillRect(0, y, canvas.width, 1);
  }
}

/* ================================================================
 * Ollama chat — fetch + streaming, calls back into WASM
 * ================================================================ */

let ollamaHistory = [];
let ollamaModel   = '';

Module.ollamaInit = async function(requestedModel) {
  try {
    const resp = await fetch('http://localhost:11434/api/tags');
    if (!resp.ok) throw new Error('Ollama returned HTTP ' + resp.status);
    const data   = await resp.json();
    const models = (data.models || []).map(m => m.name);

    if (models.length === 0) {
      Module.ccall('ollama_error', null, ['string'],
        ['No models installed. Run: ollama pull llama3.2']);
      return;
    }

    /* Use requested model if available, otherwise first installed */
    ollamaModel = (requestedModel && models.find(n => n.startsWith(requestedModel)))
                  || models[0];
    ollamaHistory = [];

    Module.ccall('ollama_receive', null, ['string'],
      ['Connected.  Model: ' + ollamaModel + '\r\n']);
    Module.ccall('ollama_ready', null, [], []);
  } catch (e) {
    Module.ccall('ollama_error', null, ['string'],
      [e.message + ' — is Ollama running? (ollama serve)']);
  }
};

Module.ollamaSend = async function(userMsg) {
  ollamaHistory.push({role: 'user', content: userMsg});

  try {
    const resp = await fetch('http://localhost:11434/api/chat', {
      method:  'POST',
      headers: {'Content-Type': 'application/json'},
      body:    JSON.stringify({
        model:    ollamaModel,
        messages: ollamaHistory,
        stream:   true,
      }),
    });

    if (!resp.ok) throw new Error('HTTP ' + resp.status);

    Module.ccall('ollama_receive', null, ['string'], ['\r\nAssistant: ']);

    const reader  = resp.body.getReader();
    const decoder = new TextDecoder();
    let   full    = '';
    let   partial = '';

    while (true) {
      const {done, value} = await reader.read();
      if (done) break;

      partial += decoder.decode(value, {stream: true});
      const lines = partial.split('\n');
      partial = lines.pop();        /* keep incomplete last line */

      for (const line of lines) {
        if (!line.trim()) continue;
        try {
          const obj   = JSON.parse(line);
          const token = obj.message?.content ?? '';
          if (token) {
            full += token;
            /* Convert bare \n → \r\n for the VT-200 terminal */
            Module.ccall('ollama_receive', null, ['string'],
              [token.replace(/\n/g, '\r\n')]);
          }
          if (obj.done) {
            ollamaHistory.push({role: 'assistant', content: full});
            Module.ccall('ollama_ready', null, [], []);
            return;
          }
        } catch (_) { /* malformed JSON line — skip */ }
      }
    }
    /* Stream ended without a done:true packet */
    ollamaHistory.push({role: 'assistant', content: full});
    Module.ccall('ollama_ready', null, [], []);

  } catch (e) {
    Module.ccall('ollama_error', null, ['string'], [e.message]);
  }
};

/* Capture keys the browser would otherwise consume */
const CAPTURED_KEYS = new Set([
  'Tab', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
  'F1','F2','F3','F4','F5','F6','F7','F8','F9','F10','F11','F12',
  'Backspace', 'Escape', 'Home', 'End', 'PageUp', 'PageDown',
]);

function handleKey(e) {
  if (CAPTURED_KEYS.has(e.key) || (e.ctrlKey && !e.metaKey)) {
    e.preventDefault();
  }

  /* Don't pass modifier-only keys */
  if (['Control','Alt','Shift','Meta','CapsLock'].includes(e.key)) return;

  Module.ccall(
    'term_send_key',
    null,
    ['string'],
    [e.key]
  );

  /* Render immediately for snappy response */
  Module._term_clear_dirty();
  render();
}
