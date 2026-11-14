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

CORE   = util morse keyer engine voices dsp decoder wordlists demo settings stats statsbook training \
         mqtt link room over abbr brokers json fcrypto filen statsync
# Mbed TLS: the hashes, AES-GCM and base64 of the Filen client (fcrypto.c).
MBEDTLS = sha1 sha256 sha512 aes gcm block_cipher base64 constant_time
OBJS   = $(patsubst %,$(OBJ)/%.o,$(CORE) monocypher $(MBEDTLS:%=mbedtls_%) mbedtls_glue page worklet libc)
HDRS   = $(wildcard ../src/*.h wasm/libc/*.h ../third_party/monocypher/*.h)
MBEDTLS_FLAGS = -isystem ../third_party/mbedtls/include -isystem ../third_party/mbedtls \
          -DMBEDTLS_CONFIG_FILE='"cwtty_mbedtls_wasm_config.h"'
STATIC = index.html style.css app.js audio.js net.js cloud.js worklet.js install.js sw.js manifest.webmanifest \
         icon.svg icon-32.png icon-192.png icon-512.png apple-touch-icon.png

CFLAGS  = --target=wasm32 -std=c11 -O2 -ffreestanding -fno-builtin -nostdlib -fvisibility=hidden \
          -isystem wasm/libc -I../src -isystem ../third_party/monocypher $(MBEDTLS_FLAGS) \
          -Wall -Wextra -Wno-unused-parameter
# The stack goes below the data, so an overflow traps instead of corrupting it.
LDFLAGS = --no-entry --export-dynamic --strip-all --stack-first

vpath %.c ../src wasm wasm/libc ../third_party/monocypher

.PHONY: all serve clean

all: $(OUT)/cwtty.wasm $(STATIC:%=$(OUT)/%)

$(OUT)/cwtty.wasm: $(OBJS) | $(OUT)
	$(WASM_LD) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ)/%.o: %.c $(HDRS) | $(OBJ)
	$(CLANG) $(CFLAGS) -c -o $@ $<

# Vendored: built as it is, without our warnings.
$(OBJ)/monocypher.o: monocypher.c $(HDRS) | $(OBJ)
	$(CLANG) $(CFLAGS) -w -c -o $@ $<

$(OBJ)/mbedtls_%.o: ../third_party/mbedtls/library/%.c ../third_party/mbedtls/cwtty_mbedtls_wasm_config.h | $(OBJ)
	$(CLANG) $(CFLAGS) -I../third_party/mbedtls/library -w -c -o $@ $<

$(OUT)/%: % | $(OUT)
	cp $< $@

$(OUT) $(OBJ):
	mkdir -p $@

serve: all
	python3 -m http.server -d $(OUT) $(PORT)

clean:
	rm -rf $(OUT) $(OBJ)
