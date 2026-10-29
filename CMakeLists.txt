cmake_minimum_required(VERSION 3.16)

project(cwtty
    VERSION 1.0.0
    DESCRIPTION "A terminal user interface for CW (Morse) telegraphy"
    LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # POSIX/BSD APIs (termios, SIGWINCH, clock_gettime)

option(CWTTY_BUILD_TESTS "Build the test suite" ON)

find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# miniaudio: vendored, and the only third-party dependency.
# -----------------------------------------------------------------------------
add_library(miniaudio STATIC third_party/miniaudio/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC third_party/miniaudio)
target_compile_definitions(miniaudio PUBLIC
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER
    MA_NO_NODE_GRAPH
    MA_NO_ENGINE)
if(MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
else()
    target_compile_options(miniaudio PRIVATE -w)
endif()
target_link_libraries(miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX)
    target_link_libraries(miniaudio PUBLIC m)
endif()
if(APPLE)
    target_link_libraries(miniaudio PUBLIC
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework AudioToolbox")
endif()

# -----------------------------------------------------------------------------
# Core library: everything except main(), shared by the app and the tests.
# -----------------------------------------------------------------------------
add_library(cwtty_core STATIC
    src/util.c
    src/platform.c
    src/term.c
    src/input.c
    src/render.c
    src/ui.c
    src/bigfont.c
    src/morse.c
    src/wordlists.c
    src/keyer.c
    src/engine.c
    src/audio.c
    src/dsp.c
    src/decoder.c
    src/demo.c
    src/settings.c
    src/transcript.c
    src/training.c
    src/app.c
    src/mode_home.c
    src/mode_tx.c
    src/mode_rx.c
    src/mode_trx.c
    src/mode_train.c
    src/modals.c)
target_include_directories(cwtty_core PUBLIC src)
target_compile_definitions(cwtty_core PUBLIC CWTTY_VERSION="${PROJECT_VERSION}")
target_link_libraries(cwtty_core PUBLIC miniaudio)

if(MSVC)
    target_compile_options(cwtty_core PRIVATE /W4 /wd4100 /wd4996)
    target_compile_definitions(cwtty_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(cwtty_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(cwtty src/main.c)
target_link_libraries(cwtty PRIVATE cwtty_core)
if(NOT MSVC)
    target_compile_options(cwtty PRIVATE -Wall -Wextra)
endif()

install(TARGETS cwtty RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if(CWTTY_BUILD_TESTS)
    enable_testing()
    add_executable(test_cwtty tests/test_cwtty.c)
    target_link_libraries(test_cwtty PRIVATE cwtty_core)
    add_test(NAME cwtty COMMAND test_cwtty)
    add_executable(test_audio tests/test_audio.c src/util.c src/keyer.c src/engine.c src/morse.c)
    target_include_directories(test_audio PRIVATE src)
    target_link_libraries(test_audio PRIVATE miniaudio)
    add_test(NAME audio COMMAND test_audio)
endif()
