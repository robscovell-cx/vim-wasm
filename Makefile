CC      := emcc
CFLAGS  := -O2 -Wall -Wextra -std=c11
SRCS    := src/terminal.c src/app.c
TARGET  := web/terminal_wasm.js

EXPORTS := -sEXPORTED_FUNCTIONS='["_term_init","_term_get_cells","_term_cell_size","_term_is_dirty","_term_clear_dirty","_term_cursor_col","_term_cursor_row","_term_send_key","_app_tick","_malloc","_free"]'
RUNTIME := -sEXPORTED_RUNTIME_METHODS='["ccall","HEAPU8","HEAP32"]'
MEM     := -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=1MB

.PHONY: all clean serve

all: $(TARGET)

$(TARGET): $(SRCS) src/terminal.h
	$(CC) $(CFLAGS) $(EXPORTS) $(RUNTIME) $(MEM) \
	    -sENVIRONMENT=web \
	    --no-entry \
	    -o $(TARGET) $(SRCS)

clean:
	rm -f web/terminal_wasm.js web/terminal_wasm.wasm

serve:
	python3 -m http.server 8080 --directory web
