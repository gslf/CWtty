# Builds the web app (a PWA) into ../build-web, ready to publish as it is:
# cwtty.wasm, compiled from the shared C sources, and the static files.
# Objects go to ../build-wasm.
#
#   make          build
#   make serve    build and serve on http://localhost:8000
#   make clean    remove both directories
#
# Needs clang with the wasm32 target and wasm-ld (LLVM lld). No Emscripten,
# no libc: wasm/libc has the few functions the sources use.

CLANG   ?= clang
WASM_LD ?= wasm-ld
OUT     ?= ../build-web
OBJ     ?= ../build-wasm
PORT    ?= 8000

CORE   = util morse keyer engine dsp decoder wordlists demo settings training
OBJS   = $(patsubst %,$(OBJ)/%.o,$(CORE) page worklet libc)
HDRS   = $(wildcard ../src/*.h wasm/libc/*.h)
STATIC = index.html style.css app.js audio.js worklet.js install.js sw.js manifest.webmanifest \
         icon.svg icon-32.png icon-192.png icon-512.png apple-touch-icon.png

CFLAGS  = --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib -fvisibility=hidden \
          -isystem wasm/libc -I../src -Wall -Wextra -Wno-unused-parameter
LDFLAGS = --no-entry --export-dynamic --strip-all

vpath %.c ../src wasm wasm/libc

.PHONY: all serve clean

all: $(OUT)/cwtty.wasm $(STATIC:%=$(OUT)/%)

$(OUT)/cwtty.wasm: $(OBJS) | $(OUT)
	$(WASM_LD) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ)/%.o: %.c $(HDRS) | $(OBJ)
	$(CLANG) $(CFLAGS) -c -o $@ $<

$(OUT)/%: % | $(OUT)
	cp $< $@

$(OUT) $(OBJ):
	mkdir -p $@

serve: all
	python3 -m http.server -d $(OUT) $(PORT)

clean:
	rm -rf $(OUT) $(OBJ)
