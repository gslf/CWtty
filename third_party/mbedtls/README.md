# mbedTLS (vendored subset)

Mbed TLS 3.6.7 LTS, from the official release tarball
`mbedtls-3.6.7.tar.bz2` (SHA-256
`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`),
licensed under Apache-2.0 (see `LICENSE`).

CWtty uses it in two builds:

- the terminal app: a TLS 1.2 client (towards the primary MQTT broker and
  Filen) and the hashes, AES-GCM and base64 of the Filen client
  (`src/fcrypto.c`), built with `cwtty_mbedtls_config.h` (passed as
  `MBEDTLS_CONFIG_FILE`);
- the web app (WebAssembly): only the hashes, AES-GCM and base64, built with
  `cwtty_mbedtls_wasm_config.h` (see `web/Makefile`).

Kept here:

- `include/` as released;
- `library/*.h` as released;
- the `library/*.c` files that these configurations compile (the lists are
  in the `mbedtls` target of the top-level `CMakeLists.txt` and in
  `MBEDTLS` of `web/Makefile`).

Nothing is modified. To update: extract a newer 3.6.x tarball, copy `include/`,
`library/*.h` and the same `library/*.c` files over these, and build.
