cmake_minimum_required(VERSION 3.16)

project(cwtty
    VERSION 1.2.0
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
# miniaudio: vendored, the audio backend.
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
# Monocypher: vendored, the cryptography of the private rooms (shared with
# the web app).
# -----------------------------------------------------------------------------
add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher SYSTEM PUBLIC third_party/monocypher)
if(MSVC)
    target_compile_options(monocypher PRIVATE /W0)
else()
    target_compile_options(monocypher PRIVATE -w)
endif()

# -----------------------------------------------------------------------------
# mbedTLS: vendored (a subset of the 3.6 LTS sources), the TLS client that
# connects to the primary MQTT broker and to Filen, and the hashes and
# AES-GCM of the Filen client. See third_party/mbedtls/README.md.
# -----------------------------------------------------------------------------
add_library(mbedtls STATIC
    third_party/mbedtls/library/aes.c
    third_party/mbedtls/library/asn1parse.c
    third_party/mbedtls/library/asn1write.c
    third_party/mbedtls/library/base64.c
    third_party/mbedtls/library/bignum.c
    third_party/mbedtls/library/bignum_core.c
    third_party/mbedtls/library/bignum_mod.c
    third_party/mbedtls/library/bignum_mod_raw.c
    third_party/mbedtls/library/block_cipher.c
    third_party/mbedtls/library/chacha20.c
    third_party/mbedtls/library/chachapoly.c
    third_party/mbedtls/library/cipher.c
    third_party/mbedtls/library/cipher_wrap.c
    third_party/mbedtls/library/constant_time.c
    third_party/mbedtls/library/ctr_drbg.c
    third_party/mbedtls/library/ecdh.c
    third_party/mbedtls/library/ecdsa.c
    third_party/mbedtls/library/ecp.c
    third_party/mbedtls/library/ecp_curves.c
    third_party/mbedtls/library/ecp_curves_new.c
    third_party/mbedtls/library/entropy.c
    third_party/mbedtls/library/entropy_poll.c
    third_party/mbedtls/library/error.c
    third_party/mbedtls/library/gcm.c
    third_party/mbedtls/library/hmac_drbg.c
    third_party/mbedtls/library/md.c
    third_party/mbedtls/library/oid.c
    third_party/mbedtls/library/pem.c
    third_party/mbedtls/library/pk.c
    third_party/mbedtls/library/pk_ecc.c
    third_party/mbedtls/library/pk_wrap.c
    third_party/mbedtls/library/pkparse.c
    third_party/mbedtls/library/platform.c
    third_party/mbedtls/library/platform_util.c
    third_party/mbedtls/library/poly1305.c
    third_party/mbedtls/library/psa_util.c
    third_party/mbedtls/library/rsa.c
    third_party/mbedtls/library/rsa_alt_helpers.c
    third_party/mbedtls/library/sha1.c
    third_party/mbedtls/library/sha256.c
    third_party/mbedtls/library/sha512.c
    third_party/mbedtls/library/ssl_ciphersuites.c
    third_party/mbedtls/library/ssl_client.c
    third_party/mbedtls/library/ssl_debug_helpers_generated.c
    third_party/mbedtls/library/ssl_msg.c
    third_party/mbedtls/library/ssl_tls.c
    third_party/mbedtls/library/ssl_tls12_client.c
    third_party/mbedtls/library/x509.c
    third_party/mbedtls/library/x509_crt.c)
target_include_directories(mbedtls SYSTEM PUBLIC third_party/mbedtls/include third_party/mbedtls)
target_include_directories(mbedtls PRIVATE third_party/mbedtls/library)
target_compile_definitions(mbedtls PUBLIC MBEDTLS_CONFIG_FILE="cwtty_mbedtls_config.h")
if(MSVC)
    target_compile_options(mbedtls PRIVATE /W0)
else()
    target_compile_options(mbedtls PRIVATE -w)
endif()
if(WIN32)
    target_link_libraries(mbedtls PUBLIC bcrypt)  # BCryptGenRandom
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
    src/voices.c
    src/audio.c
    src/mqtt.c
    src/room.c
    src/over.c
    src/abbr.c
    src/link.c
    src/brokers.c
    src/netio.c
    src/net.c
    src/net_cli.c
    src/dsp.c
    src/decoder.c
    src/demo.c
    src/settings.c
    src/stats.c
    src/statsbook.c
    src/json.c
    src/fcrypto.c
    src/filen.c
    src/statsync.c
    src/https.c
    src/cloud.c
    src/transcript.c
    src/training.c
    src/app.c
    src/mode_home.c
    src/mode_tx.c
    src/mode_rx.c
    src/mode_trx.c
    src/mode_train.c
    src/mode_stats.c
    src/mode_net.c
    src/modals.c)
target_include_directories(cwtty_core PUBLIC src)
target_compile_definitions(cwtty_core PUBLIC CWTTY_VERSION="${PROJECT_VERSION}")
target_link_libraries(cwtty_core PUBLIC miniaudio monocypher mbedtls)
if(WIN32)
    target_link_libraries(cwtty_core PUBLIC ws2_32 bcrypt crypt32)
endif()

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
    add_executable(test_audio tests/test_audio.c src/util.c src/keyer.c src/engine.c src/voices.c src/morse.c)
    target_include_directories(test_audio PRIVATE src)
    target_link_libraries(test_audio PRIVATE miniaudio)
    add_test(NAME audio COMMAND test_audio)
    add_executable(test_net tests/test_net.c)
    target_link_libraries(test_net PRIVATE cwtty_core)
    if(NOT MSVC)
        target_compile_options(test_net PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    add_test(NAME net COMMAND test_net)
    add_executable(test_filen tests/test_filen.c)
    target_link_libraries(test_filen PRIVATE cwtty_core)
    if(NOT MSVC)
        target_compile_options(test_filen PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    add_test(NAME filen COMMAND test_filen)
endif()
