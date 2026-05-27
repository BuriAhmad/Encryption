# PC Tools (SEAL Side)

This folder contains host-side tooling for the ESP32 CKKS encryption-only client workflow.

## Build

```bash
cmake -S pc_tools -B pc_tools/build
cmake --build pc_tools/build -j4
```

By default the build expects:
- SEAL source at `../SEAL`
- SEAL minimal build at `../SEAL/build-min`

You can override these:

```bash
cmake -S pc_tools -B pc_tools/build \
  -DSEAL_SOURCE_DIR=/path/to/SEAL \
  -DSEAL_BUILD_DIR=/path/to/SEAL/build
```

## Tool Commands

Binary: `pc_tools/build/he_tool`

### 1) Export bundle + secret key

```bash
pc_tools/build/he_tool export-bundle \
  --bundle-out pc_tools/test_vectors/bundle.bin \
  --secret-out pc_tools/test_vectors/secret.bin \
  --poly 4096 \
  --coeff-bits 50 \
  --scale-bits 20
```

### 1b) Export server-side evaluation keys

Relinearization and Galois keys are only needed on the PC/server when doing ciphertext-ciphertext multiplication or slot rotations. The ESP32 encryption client does not need these keys.

```bash
pc_tools/build/he_tool export-eval-keys \
  --bundle pc_tools/test_vectors/bundle.bin \
  --secret pc_tools/test_vectors/secret.bin \
  --relin-out pc_tools/test_vectors/relin_keys.bin \
  --galois-out pc_tools/test_vectors/galois_keys.bin \
  --galois-steps 1,2,4,8
```

Evaluation keys are for the PC/server only. The ESP32 encryption client never stores relin or Galois keys.

### 2) Encrypt from bundle

```bash
pc_tools/build/he_tool encrypt-from-bundle \
  --bundle pc_tools/test_vectors/bundle.bin \
  --out pc_tools/test_vectors/cipher.bin \
  --values "1.25,-2.5,3.75,4.5"
```

### 3) Export embedded package

```bash
pc_tools/build/he_tool export-embedded-package \
  --bundle pc_tools/test_vectors/bundle.bin \
  --out pc_tools/test_vectors/embedded_package.bin
```

### 4) Encrypt with mini embedded-compatible core

```bash
pc_tools/build/he_tool encrypt-mini \
  --package pc_tools/test_vectors/embedded_package.bin \
  --out pc_tools/test_vectors/mini_cipher.bin \
  --values "1.25,-2.5,3.75,4.5" \
  --rng-seed 12345
```

### 5) Decrypt and check

```bash
pc_tools/build/he_tool decrypt-check \
  --bundle pc_tools/test_vectors/bundle.bin \
  --secret pc_tools/test_vectors/secret.bin \
  --cipher pc_tools/test_vectors/cipher.bin \
  --expected "1.25,-2.5,3.75,4.5" \
  --max-abs-err 0.05 \
  --compute-a 1 \
  --compute-b 2 \
  --compute-c 3
```

## Deterministic Vector Generation

Run:

```bash
pc_tools/scripts/generate_test_vector.sh
```

This creates:
- `pc_tools/test_vectors/bundle.bin`
- `pc_tools/test_vectors/secret.bin`
- `pc_tools/test_vectors/cipher.bin`
- `pc_tools/test_vectors/embedded_package.bin`
- `pc_tools/test_vectors/mini_cipher.bin`
- `pc_tools/test_vectors/decrypt_report.txt`
- `pc_tools/test_vectors/vector_manifest.txt`

The script uses a fixed debug seed for deterministic outputs. Never use deterministic seeds in production.

## Capture Ciphertext From ESP32

After uploading `MiniEncryptDemo` to the ESP32-S3, capture a ciphertext over serial:

```bash
pc_tools/serial/capture_esp_ciphertext.py \
  --port /dev/tty.usbserial-10 \
  --values 1.5,2.25,-3.0,4.75 \
  --out pc_tools/test_vectors/cipher_from_esp.bin \
  --report pc_tools/test_vectors/encrypt_report.txt \
  --bundle pc_tools/test_vectors/bundle_8192_40x5.bin \
  --secret pc_tools/test_vectors/secret_8192_40x5.bin \
  --verify
```

With `--verify`, the script decrypts using normal SEAL and then runs a small PC-side encrypted computation check:

```text
((x + 1) + 2) * 3
```

To run timing and memory measurements without emitting a ciphertext:

```bash
pc_tools/serial/capture_esp_ciphertext.py \
  --port /dev/tty.usbserial-10 \
  --value 1.5 \
  --bench-runs 10 \
  --report pc_tools/test_vectors/bench_report.txt
```

## Convert Bundle to Arduino Header

```bash
pc_tools/scripts/bin_to_c_array.py \
  pc_tools/test_vectors/embedded_package.bin \
  arduino/MiniEncryptDemo/embedded_package_blob.h \
  --var kEmbeddedPackageBlob
```
