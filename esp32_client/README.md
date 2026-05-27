# ESP32 CKKS Client Core

This folder contains the reusable embedded C++ core used by the Arduino sketch.

## Current Files

- `src/he_allocator.*`
  - PSRAM-preferred allocation wrappers for large HE buffers.
  - Tracks current/peak allocation, PSRAM/internal split, largest allocation, and failures.
- `src/he_embedded_package.*`
  - Parser for the compact embedded package exported by the PC tool.
  - Package v2 stores coefficient modulus values plus raw public-key polynomials in NTT form.
- `src/mini_ckks_client.*`
  - Minimal CKKS real-vector encoder.
  - Asymmetric public-key encryption.
  - SEAL-compatible ciphertext serialization.

## Current Active Target

The current validated Arduino package is:

```text
N = 8192
coeff bits = {40,40,40,40,40}
scale_bits = 20
key-level modulus count = 5
fresh ciphertext modulus count = 4
```

The ESP32 does not generate keys, decrypt, evaluate, relinearize, rotate, or rescale. It only:

```text
load embedded public-key package -> encode real vector -> encrypt -> serialize ciphertext
```

## Removed Scaffolding

Older bundle-inspection examples and full host-bundle parsers were removed because the active embedded flow uses the compact package only. The canonical Arduino sketch lives in:

```text
arduino/MiniEncryptDemo/
```
