# Arduino Upload Flow

This folder contains the Arduino-ready sketch for the ESP32-S3 CAM:

- `MiniEncryptDemo/MiniEncryptDemo.ino`
- `MiniEncryptDemo/embedded_package_blob.h`
- `MiniEncryptDemo/he_allocator.*`
- `MiniEncryptDemo/he_embedded_package.*`
- `MiniEncryptDemo/mini_ckks_client.*`

The same folder is also copied to the Arduino sketchbook here:

```text
/Users/burhanahmadkhan/Documents/Arduino/MiniEncryptDemo
```

That copy is useful because the Arduino IDE expects each sketch to live in a folder with the same name as the `.ino` file.

## What Runs On The ESP32

The ESP32 sketch does not generate keys and does not decrypt. It only:

1. Starts Serial at `115200`.
2. Loads the embedded public-key package from `embedded_package_blob.h`.
3. Waits for a command from the PC.
4. Encrypts one scalar value when it receives `ENCRYPT <value>`.
5. Prints a SEAL-compatible ciphertext as hex between markers.

Supported commands:

```text
PING
INFO
MEM
ENCRYPT 1.25
BENCH 1.25 10
```

## Compile

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,FlashSize=4M,UploadSpeed=115200 \
  /Users/burhanahmadkhan/Documents/Arduino/MiniEncryptDemo
```

## Upload

First find the port:

```bash
arduino-cli board list
```

Then upload. The current tested port was `/dev/tty.usbserial-10`, but it can change.

```bash
arduino-cli upload \
  -p /dev/tty.usbserial-10 \
  --fqbn esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app,FlashSize=4M,UploadSpeed=115200 \
  /Users/burhanahmadkhan/Documents/Arduino/MiniEncryptDemo
```

`UploadSpeed=115200` is intentional. The board failed at the faster default upload speed but uploaded correctly at 115200.
`PSRAM=opi` is also intentional for this ESP32-S3 CAM board. With `PSRAM=enabled` the sketch still ran, but runtime PSRAM reported `0`. With `PSRAM=opi`, runtime PSRAM reported about 8 MB.

## Manual Serial Monitor

You can manually talk to the board with:

```bash
arduino-cli monitor -p /dev/tty.usbserial-10 -c baudrate=115200
```

Then type:

```text
INFO
MEM
ENCRYPT 1.25
BENCH 1.25 10
```

This is good for human checking, but it is not the best way to save ciphertext files because the ciphertext is a long hex block.

## Automatic Capture And Verification

Use the capture script instead:

```bash
pc_tools/serial/capture_esp_ciphertext.py \
  --port /dev/tty.usbserial-10 \
  --value 1.25 \
  --out pc_tools/test_vectors/cipher_from_esp.bin \
  --verify
```

This script:

1. Opens the serial port.
2. Sends `ENCRYPT 1.25`.
3. Reads the ciphertext hex block.
4. Saves it as a binary `.bin` file.
5. Runs PC-side SEAL decrypt verification.
6. If decrypt passes, runs a basic encrypted computation check:

```text
((x + 1) + 2) * 3
```

For `x = 1.25`, the expected computed result is `12.75`.

## Benchmark And Memory Report

Run a benchmark without printing a huge ciphertext:

```bash
pc_tools/serial/capture_esp_ciphertext.py \
  --port /dev/tty.usbserial-10 \
  --value 1.5 \
  --bench-runs 10 \
  --report pc_tools/test_vectors/bench_report.txt
```

The benchmark report includes:

- CKKS parameters: `N`, scale bits, coefficient modulus, ciphertext size.
- Timing: min/avg/max encryption time, serialization time, total time.
- Heap/PSRAM: free memory before, lowest seen during the run, and after.
- Tracked allocations: persistent root-table memory, peak encryption memory, PSRAM vs internal RAM usage.

The most important line for future scaling is:

```text
TRACKED_PEAK_DELTA_MAX
```

That is the extra tracked HE memory needed while encryption is running, not counting the persistent root tables already loaded at boot.

## Important Serial Notes

- Close Arduino IDE Serial Monitor before upload or capture, otherwise the port may be busy.
- Close `arduino-cli monitor` before running the capture script.
- If the port changes, rerun `arduino-cli board list`.
- If upload fails at high speed, keep `UploadSpeed=115200`.
