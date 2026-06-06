CC      := emcc
CFLAGS  := -O2 -Wall -Wextra -std=gnu11
SRCS    := src/terminal.c src/app.c src/curses.c src/snake.c src/forth.c \
           src/tint/tint.c src/tint/io.c src/tint/engine.c src/tint/utils.c
TARGET  := web/terminal_wasm.js

# tint: redirect <curses.h> to our implementation, stub score file,
# suppress macro-redefinition warnings from tint's io.h redefining COLOR_*
TINT_FLAGS := -I src/ -DUSE_RAND -DSCOREFILE='"/tint.scores"' -Wno-macro-redefined

EXPORTS := -sEXPORTED_FUNCTIONS='["_term_init","_term_get_cells","_term_cell_size","_term_is_dirty","_term_clear_dirty","_term_cursor_col","_term_cursor_row","_term_send_key","_app_tick","_ollama_receive","_ollama_ready","_ollama_error","_curses_push_key","_snake_main","_tint_main","_malloc","_free"]'
RUNTIME := -sEXPORTED_RUNTIME_METHODS='["ccall","HEAPU8","HEAP32"]'
MEM     := -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=2MB
ASYNC   := -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536

.PHONY: all clean serve kill

all: $(TARGET)

$(TARGET): $(SRCS) src/terminal.h src/curses.h
	$(CC) $(CFLAGS) $(TINT_FLAGS) $(EXPORTS) $(RUNTIME) $(MEM) $(ASYNC) \
	    -sENVIRONMENT=web \
	    --no-entry \
	    -o $(TARGET) $(SRCS)

clean:
	rm -f web/terminal_wasm.js web/terminal_wasm.wasm

serve:
	python3 -m http.server 8080 --directory web

kill:
	@pkill -f "python3 -m http.server 8080" || true
