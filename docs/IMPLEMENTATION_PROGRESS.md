# Implementation Progress

Date: 2026-05-27

## Completed in this increment

- Added host-side tooling to export CKKS bundle and test keys:
  - `pc_tools/src/he_tool.cpp`
  - `pc_tools/src/he_bundle.cpp`
  - `pc_tools/include/he_bundle.hpp`
- Added deterministic interoperability workflow:
  - `pc_tools/scripts/generate_test_vector.sh`
  - `pc_tools/scripts/bin_to_c_array.py`
- Added embedded-side allocation and package scaffolding:
  - `esp32_client/src/he_allocator.h`
  - `esp32_client/src/he_allocator.cpp`
  - `esp32_client/src/he_embedded_package.h`
  - `esp32_client/src/he_embedded_package.cpp`
- Added compact embedded package + mini encryption core:
  - `esp32_client/src/he_embedded_package.h`
  - `esp32_client/src/he_embedded_package.cpp`
  - `esp32_client/src/mini_ckks_client.h`
  - `esp32_client/src/mini_ckks_client.cpp`
  - `pc_tools/src/he_tool.cpp` (`export-embedded-package`, `encrypt-mini`)
- Added Arduino-ready sketch and serial capture workflow:
  - `arduino/MiniEncryptDemo/MiniEncryptDemo.ino`
  - `arduino/README.md`
  - `pc_tools/serial/capture_esp_ciphertext.py`
  - `docs/HARDWARE_TEST_FLOW.md`
- Added measurement mode:
  - `MEM` command for immediate heap/PSRAM/tracked allocation state
  - `BENCH <value> <runs>` command for repeated encryption timing/memory measurements
  - PSRAM-preferred allocation tracker with current/peak/internal/PSRAM counters
- Added PC/server evaluation-key scaffolding:
  - `docs/PARAMETER_FEASIBILITY_4K_8K.md`
  - `pc_tools/build/he_tool export-eval-keys` command for future multi-prime bundles
- Upgraded the active embedded prototype from scalar `N=2048` to vector CKKS encoding at `N=4096`:
  - `ENCRYPT` now accepts comma-separated vector values, e.g. `ENCRYPT 1.5,2.25,-3.0,4.75`.
  - Embedded vector encoder mirrors SEAL's CKKS matrix index map and inverse complex DWT flow.
  - Embedded NTT root selection was corrected to SEAL's minimal primitive root convention.
- Added first multi-prime public-key/package support for `N=4096`, coeff bits `{40,40}`:
  - Embedded package version 2 stores `q_values[K]`, `pk0[K][N]`, and `pk1[K][N]`.
  - Encryption runs at the SEAL key level and then drops the special/key prime for fresh CKKS ciphertexts.
  - Ciphertext serialization writes the ciphertext data-level modulus count rather than assuming the key-level count.
  - Host verification supports both stable-scale and same-scale multiply-by-constant tests.

## Validated flow

1. `export-bundle` creates bundle + secret key.
2. `encrypt-from-bundle` creates ciphertext using only bundle public key + params.
3. `decrypt-check` decrypts and reports max absolute error.
4. Deterministic script (`generate_test_vector.sh`) produces repeatable artifacts.
5. Arduino sketch compiles and uploads to ESP32-S3 with `PSRAM=opi` and `UploadSpeed=115200`.
6. ESP32-generated ciphertext was captured over serial and decrypted successfully on PC with SEAL.
7. After decrypt verification, PC-side SEAL computation test runs `((x + 1) + 2) * 3`.

## Current status against milestones

- Milestone 1 (parameter loading + public key loading): **complete for compact embedded packages**
  - Host bundle format is implemented and validated with CRC.
  - Embedded package parser validates magic, version, sizing, and CRC.
- Milestone 4 (encryption): **multi-prime key-level implementation**
  - Scalar/vector encode + asymmetric public-key encrypt path implemented in `mini_ckks_client`.
  - Supports one SEAL-style key-prime drop for `{data, special}` chains such as `{40,40}`.
- Milestone 5 (serialization): **initial SEAL-compatible implementation**
  - Mini core serializes SEAL-compatible ciphertext binary (`compr_mode=none`).
- Milestone 7 test artifact requirement (deterministic vectors): **initial implementation complete**
- Hardware upload/capture smoke test: **initial implementation complete**
  - Tested serial command path: `ENCRYPT 1.25`
  - Captured ciphertext size: 32881 bytes
  - Runtime PSRAM with `PSRAM=opi`: detected, about 8 MB total
  - Decrypt check: passed
  - Compute-after-decrypt-check: passed for `((x + 1) + 2) * 3`
- Benchmark mode: **initial implementation complete**
  - Tested command path: `BENCH 1.5 5`
  - Average encryption time: about 462 ms
  - Average serialization time: about 1 ms
  - Temporary tracked HE allocation peak delta: about 131185 bytes
  - Temporary tracked HE allocations used PSRAM, not internal RAM
- Evaluation-key export: **PC/server helper available**
  - Relin and Galois keys are exported for server-side computation only.
  - The ESP32 encryption client does not store evaluation keys.
- Vector/N=4096 hardware test: **initial implementation complete**
  - Tested serial command path: `ENCRYPT 1.5,2.25,-3.0,4.75`
  - Captured ciphertext size: 65649 bytes
  - Decrypt check: passed with max absolute error about 0.014
  - Compute-after-decrypt-check: passed for `((x + 1) + 2) * 3`
  - Average vector encryption time at `N=4096`: about 1196 ms
  - Temporary tracked HE allocation peak delta at `N=4096`: about 262257 bytes
- Multi-prime `N=4096,{40,40}` hardware test: **encryption/decrypt complete**
  - Tested serial command path: `ENCRYPT 1.5,2.25,-3.0,4.75`
  - Public-key package size: 131168 bytes
  - Key-level coefficient modulus count: 2
  - Fresh ciphertext coefficient modulus count: 1
  - Captured ciphertext size: 65649 bytes
  - Decrypt check: passed with max absolute error about 0.00292
  - Compute-after-decrypt-check passed for `((x + 1) + 2) * 3` using the stable-scale multiply-by-constant mode.
  - Average encryption time over 5 runs: about 2057 ms
  - Average total time over 5 runs: about 2060 ms
  - Persistent tracked HE allocation after setup: about 278544 bytes
  - Temporary tracked HE allocation peak delta: about 471153 bytes
  - Tracked HE allocations used PSRAM; internal tracked HE allocation stayed at 0 bytes.
- Multi-prime `N=8192,{40,40}` hardware test: **encryption/decrypt/compute complete**
  - Tested serial command path: `ENCRYPT 1.5,2.25,-3.0,4.75`
  - Public-key package size: 262240 bytes
  - Key-level coefficient modulus count: 2
  - Fresh ciphertext coefficient modulus count: 1
  - Captured ciphertext size: 131185 bytes
  - Decrypt check: passed with max absolute error about 0.000893
  - Compute-after-decrypt-check passed for `((x + 1) + 2) * 3` with max absolute error about 0.00268
  - Average encryption time over 5 runs: about 4460 ms
  - Average total time over 5 runs: about 4467 ms
  - Persistent tracked HE allocation after setup: about 557072 bytes
  - Temporary tracked HE allocation peak delta: about 942193 bytes
  - Tracked HE allocations used PSRAM; internal tracked HE allocation stayed at 0 bytes.
- Chain growth to `N=8192,{40,40,40,40,40}`: **complete**
  - Same-scale compute test passed for 3, 4, and 5 total 40-bit primes.
  - Maximum SEAL-valid tested chain at `N=8192` was five 40-bit primes.
  - Six 40-bit primes failed SEAL parameter validation before ESP32 upload.
  - Detailed measurements are in `docs/CHAIN_GROWTH_LOG.md`.

## Not implemented yet

- Seeded ciphertext/public-key serialization paths
- Binary serial/Wi-Fi transport for larger ciphertexts
- Ciphertext-ciphertext multiplication tests with relin/rescale on the PC/server.
- External-tool energy measurement is intentionally deferred.
