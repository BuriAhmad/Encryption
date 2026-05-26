# Implementation Progress

Date: 2026-05-26

## Completed in this increment

- Added host-side tooling to export CKKS bundle and test keys:
  - `pc_tools/src/he_tool.cpp`
  - `pc_tools/src/he_bundle.cpp`
  - `pc_tools/include/he_bundle.hpp`
- Added deterministic interoperability workflow:
  - `pc_tools/scripts/generate_test_vector.sh`
  - `pc_tools/scripts/bin_to_c_array.py`
- Added embedded-side bundle parser/memory scaffolding:
  - `esp32_client/src/he_bundle_format.h`
  - `esp32_client/src/he_bundle_format.cpp`
  - `esp32_client/src/he_memory_plan.h`
  - `esp32_client/src/he_memory_plan.cpp`
  - `esp32_client/src/he_allocator.h`
  - `esp32_client/src/he_allocator.cpp`
  - `esp32_client/examples/BundleInspector/BundleInspector.ino`
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

## Validated flow

1. `export-bundle` creates bundle + secret key.
2. `encrypt-from-bundle` creates ciphertext using only bundle public key + params.
3. `decrypt-check` decrypts and reports max absolute error.
4. Deterministic script (`generate_test_vector.sh`) produces repeatable artifacts.
5. Arduino sketch compiles and uploads to ESP32-S3 with `PSRAM=opi` and `UploadSpeed=115200`.
6. ESP32-generated ciphertext was captured over serial and decrypted successfully on PC with SEAL.
7. After decrypt verification, PC-side SEAL computation test runs `((x + 1) + 2) * 3`.

## Current status against milestones

- Milestone 1 (parameter loading + public key loading): **in progress / scaffolded**
  - Host bundle format is implemented and validated with CRC.
  - Embedded parser exists and validates bundle structure.
- Milestone 4 (encryption): **initial single-prime implementation**
  - Scalar encode + asymmetric encrypt path implemented in `mini_ckks_client`.
- Milestone 5 (serialization): **initial implementation**
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

## Not implemented yet

- Full CKKS vector encode path on ESP32
- Multi-prime (`K>1`) RNS path
- Seeded ciphertext/public-key serialization paths
- Runtime memory benchmarking on target board
- Larger/more realistic memory benchmarking before increasing parameter sizes.
- External-tool energy measurement is intentionally deferred.
