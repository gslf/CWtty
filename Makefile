# Convenience wrapper around CMake.
#
#   make            configure (if needed) and build a Release binary
#   make run        build and start cwtty
#   make test       build and run the test suite
#   make debug      build a Debug binary in build-debug/
#   make install    install (use PREFIX=/some/path to override the prefix)
#   make web        build the web app (PWA) into build-web/, see web/Makefile
#   make web-serve  build it and serve it on http://localhost:8000
#   make release    run the tests, tag vVERSION (from CMakeLists.txt) and push
#                   it: GitHub Actions builds the binaries and publishes them
#   make clean      clean build outputs
#   make distclean  remove all build directories

BUILD_DIR  ?= build
BUILD_TYPE ?= Release
CMAKE      ?= cmake
CTEST      ?= ctest
GENERATOR  ?=
VERSION    := $(shell sed -n 's/^ *VERSION \([0-9.]*\)$$/\1/p' CMakeLists.txt | head -n 1)

ifneq ($(GENERATOR),)
CMAKE_GEN := -G "$(GENERATOR)"
endif

.PHONY: all build configure run test debug install web web-serve release clean distclean

all: build

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_GEN) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel

run: build
	./$(BUILD_DIR)/cwtty

test: build
	cd $(BUILD_DIR) && $(CTEST) --output-on-failure

debug:
	$(MAKE) BUILD_DIR=build-debug BUILD_TYPE=Debug build

install: build
	$(CMAKE) --install $(BUILD_DIR) $(if $(PREFIX),--prefix "$(PREFIX)",)

web:
	$(MAKE) -C web

web-serve:
	$(MAKE) -C web serve

release: test
	@test -z "$$(git status --porcelain)" || { echo "release: commit or stash your changes first"; exit 1; }
	@! git rev-parse -q --verify "refs/tags/v$(VERSION)" >/dev/null || \
		{ echo "release: v$(VERSION) already exists, bump VERSION in CMakeLists.txt"; exit 1; }
	git tag -a "v$(VERSION)" -m "CWtty $(VERSION)"
	git push origin HEAD "v$(VERSION)"

clean:
	@if [ -d "$(BUILD_DIR)" ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf build build-debug build-web build-wasm
