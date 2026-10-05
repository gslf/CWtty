# Convenience wrapper around CMake.
#
#   make            configure (if needed) and build a Release binary
#   make run        build and start cwtty
#   make test       build and run the test suite
#   make debug      build a Debug binary in build-debug/
#   make install    install (use PREFIX=/some/path to override the prefix)
#   make clean      clean build outputs
#   make distclean  remove all build directories

BUILD_DIR  ?= build
BUILD_TYPE ?= Release
CMAKE      ?= cmake
CTEST      ?= ctest
GENERATOR  ?=

ifneq ($(GENERATOR),)
CMAKE_GEN := -G "$(GENERATOR)"
endif

.PHONY: all build configure run test debug install clean distclean

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

clean:
	@if [ -d "$(BUILD_DIR)" ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf build build-debug
