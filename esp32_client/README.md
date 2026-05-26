# ESP32 Client Scaffolding

This folder contains initial embedded-side scaffolding for bundle parsing and memory planning.

## Files

- `src/he_bundle_format.h` and `src/he_bundle_format.cpp`:
  - Parses and validates the host-generated bundle format.
  - Validates magic/version/scheme/size/CRC.
- `src/he_memory_plan.h` and `src/he_memory_plan.cpp`:
  - Fast sizing estimates for plaintext/ciphertext/public-key buffers.
- `src/he_allocator.h` and `src/he_allocator.cpp`:
  - PSRAM-preferred allocation wrappers for large HE buffers.
- `src/he_embedded_package.h` and `src/he_embedded_package.cpp`:
  - Parser for compact single-prime embedded package (public key in raw NTT form).
- `src/mini_ckks_client.h` and `src/mini_ckks_client.cpp`:
  - Minimal CKKS real-vector encode + asymmetric encrypt + SEAL ciphertext serializer.

## Arduino Example

`examples/BundleInspector/BundleInspector.ino` demonstrates:
1. Loading bundle bytes (currently placeholder blob header)
2. Parsing and validating bundle
3. Printing HE metadata and memory estimates

To use it:
1. Generate a real `bundle.bin` with `pc_tools/he_tool export-bundle`.
2. Convert `bundle.bin` into a C byte array and paste into `examples/BundleInspector/bundle_blob_example.h`.
3. Build/upload the example and inspect serial output.

`examples/MiniEncryptDemo/MiniEncryptDemo.ino` demonstrates:
1. Loading compact embedded package bytes (`embedded_package.bin` converted to C array).
2. Running minimal real-vector CKKS encryption.
3. Serializing a SEAL-compatible ciphertext buffer and printing metadata.
4. Printing timing + heap/PSRAM counters for quick board-side monitoring.

## Current Scope

This now includes a first single-prime (`coeff_modulus_count=1`) encryption core:
- scalar CKKS encode (repeated slot semantics),
- real-vector CKKS encode using SEAL-style matrix mapping and inverse complex DWT,
- RLWE public-key encryption path for fresh ciphertexts,
- SEAL-compatible ciphertext binary serialization (`compr_mode=none`).

Multi-prime RNS chains are not yet implemented.
