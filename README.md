# Encryption

Minimal CKKS public-key encryption client for an ESP32-S3 CAM board, designed to produce ciphertexts that are compatible with Microsoft SEAL.

## What This Repo Does

The goal is not to port all of SEAL to the ESP32.

The goal is to keep the embedded side small and focused:

- load pre-generated CKKS parameters
- load a pre-generated public key
- encode a real vector
- encrypt it on the ESP32
- serialize the ciphertext in a SEAL-compatible format
- send the ciphertext back to a PC/server

The PC/server side does the heavy work:

- key generation
- evaluation-key generation
- decrypt/verify
- future homomorphic computation

## Current Status

Working end-to-end flow is implemented and tested:

- PC generates CKKS parameters and keys with Microsoft SEAL
- PC exports a compact embedded package
- ESP32-S3 loads that package and encrypts a real CKKS vector
- PC captures the ciphertext over serial
- PC deserializes and decrypts it with normal SEAL
- PC runs a shallow post-encryption compute test: `((x + 1) + 2) * 3`

Current maximum validated configuration:

- `N = 8192`
- coeff bits = `{40,40,40,40,40}`
- `scale_bits = 20`

This five-prime chain is the largest tested chain in this repo that is both:

- accepted by SEAL parameter validation
- successfully run on the ESP32-S3 CAM

`N=8192` with six 40-bit primes is rejected by SEAL before ESP32 upload.

## Current Resource Picture

For the active validated package `N=8192,{40,40,40,40,40}`:

- sketch flash: about `978 KB`
- global RAM: about `20.4 KB`
- persistent tracked HE memory: about `928 KB`
- temporary tracked HE peak during encryption: about `2.21 MB`
- total tracked HE peak during encryption: about `3.12 MB`
- average encryption time: about `10.0 s`

All tracked HE working buffers are going to PSRAM, not internal RAM.

## Repo Layout

- `arduino/`
  - Arduino-ready ESP32 sketch used for compile/upload
- `esp32_client/`
  - reusable embedded-side C++ core
- `pc_tools/`
  - host-side SEAL tools for bundle export, verification, and test capture
- `docs/`
  - design notes, test flow, format notes, and chain-growth measurements
- `SEAL/`
  - local Microsoft SEAL checkout used by the host tools

## Main Flow

```text
PC/SEAL: generate params + keys
PC/SEAL: export compact embedded package
Arduino: compile/upload sketch with embedded package blob
ESP32: load package -> encode -> encrypt -> serialize
PC: capture ciphertext over serial
PC/SEAL: decrypt and verify
PC/SEAL: run small compute test
```

## Quick Start

Clone Microsoft SEAL into this repo first. The host tools expect a local checkout at `./SEAL`.

```bash
git clone --depth 1 https://github.com/microsoft/SEAL.git
cmake -S SEAL -B SEAL/build-min \
  -DSEAL_BUILD_DEPS=ON \
  -DSEAL_USE_MSGSL=OFF \
  -DSEAL_USE_ZLIB=ON \
  -DSEAL_USE_ZSTD=ON
cmake --build SEAL/build-min -j4
```

Build the PC tool:

```bash
cmake -S pc_tools -B pc_tools/build
cmake --build pc_tools/build -j4
```

Compile the Arduino sketch:

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,FlashSize=4M,UploadSpeed=115200 \
  ~/Documents/Arduino/MiniEncryptDemo
```

Upload to the board:

```bash
# Find your port first (it may differ):
arduino-cli board list

arduino-cli upload \
  -p /dev/tty.usbserial-XXX \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,FlashSize=4M,UploadSpeed=115200 \
  ~/Documents/Arduino/MiniEncryptDemo
```

Capture, decrypt, and verify:

```bash
pc_tools/serial/capture_esp_ciphertext.py \
  --port /dev/tty.usbserial-XXX \
  --values 1.5,2.25,-3.0,4.75 \
  --out pc_tools/test_vectors/cipher_from_esp.bin \
  --bundle pc_tools/test_vectors/bundle_8192_40x5.bin \
  --secret pc_tools/test_vectors/secret_8192_40x5.bin \
  --verify
```

## Important Constraints

- real-valued vector CKKS encode only
- public-key encryption only
- no decryption on the ESP32
- no evaluation on the ESP32
- no relin or Galois keys on the ESP32
- serial hex transport is still simple and slow

## Recommended Reading

- `docs/HARDWARE_TEST_FLOW.md`
- `docs/CHAIN_GROWTH_LOG.md`
- `docs/MINI_CLIENT_FORMATS.md`
- `docs/ESP32_CKKS_SEAL_DESIGN.md`
