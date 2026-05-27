# CKKS Chain Growth Log

Date: 2026-05-27

Goal: keep `N=8192` fixed and increase the 40-bit coefficient modulus chain one step at a time until the ESP32-S3 fails, SEAL rejects the parameters, or the chain reaches 6 primes.

Verification rule for each working step:

- ESP32 encrypts `1.5,2.25,-3.0,4.75`.
- PC SEAL decrypts the ciphertext.
- PC SEAL runs `((x + 1) + 2) * 3`.
- The multiplication plaintext is encoded at the same scale as the ciphertext for this chain-growth test.
- Measurements are recorded from the ESP32 serial benchmark.

## Baseline: `N=8192`, coeff bits `{40,40}`

- Status: PASS with stable-scale multiply-by-constant; this is the previous checkpoint.
- Note: same-scale multiply was not used for the baseline because the fresh ciphertext has only one data prime after the special/key prime is dropped.
- Package size: 262240 bytes.
- Ciphertext size: 131185 bytes.
- Persistent tracked HE allocation: 557072 bytes.
- Peak temporary tracked HE allocation delta: 942193 bytes.
- Average encryption time: about 4460 ms.
- Average total time: about 4467 ms.

## Step 1: `N=8192`, coeff bits `{40,40,40}`

- Status: PASS.
- Same-scale multiply test: PASS for `((x + 1) + 2) * 3`.
- Package size: 393320 bytes.
- Ciphertext size: 262257 bytes.
- Key-level coefficient modulus count: 3.
- Fresh ciphertext coefficient modulus count: 2.
- Persistent tracked HE allocation: 688152 bytes.
- Peak temporary tracked HE allocation delta: 1400945 bytes.
- Peak tracked HE allocation during encryption: 2089097 bytes.
- Average encryption time over 5 runs: about 6316 ms.
- Average serialization time over 5 runs: about 12 ms.
- Average total time over 5 runs: about 6328 ms.
- Internal tracked HE allocation: 0 bytes.
- Failed allocations: 0.
- Sketch flash after compile: 715967 bytes.
- Result: safe to try the next larger chain.

## Step 2: `N=8192`, coeff bits `{40,40,40,40}`

- Status: PASS.
- Same-scale multiply test: PASS for `((x + 1) + 2) * 3`.
- Package size: 524400 bytes.
- Ciphertext size: 393329 bytes.
- Key-level coefficient modulus count: 4.
- Fresh ciphertext coefficient modulus count: 3.
- Persistent tracked HE allocation: 819232 bytes.
- Peak temporary tracked HE allocation delta: 1859697 bytes.
- Peak tracked HE allocation during encryption: 2678929 bytes.
- Average encryption time over 5 runs: about 8167 ms.
- Average serialization time over 5 runs: about 17 ms.
- Average total time over 5 runs: about 8185 ms.
- Internal tracked HE allocation: 0 bytes.
- Failed allocations: 0.
- Sketch flash after compile: 847039 bytes.
- Result: safe to try the next larger chain.

## Step 3: `N=8192`, coeff bits `{40,40,40,40,40}`

- Status: PASS.
- Same-scale multiply test: PASS for `((x + 1) + 2) * 3`.
- Package size: 655480 bytes.
- Ciphertext size: 524401 bytes.
- Key-level coefficient modulus count: 5.
- Fresh ciphertext coefficient modulus count: 4.
- Persistent tracked HE allocation: 950312 bytes.
- Peak temporary tracked HE allocation delta: 2318449 bytes.
- Peak tracked HE allocation during encryption: 3268761 bytes.
- Average encryption time over 5 runs: about 10033 ms.
- Average serialization time over 5 runs: about 23 ms.
- Average total time over 5 runs: about 10056 ms.
- Internal tracked HE allocation: 0 bytes.
- Failed allocations: 0.
- Sketch flash after compile: 978127 bytes.
- Result: safe to attempt the 6-prime limit, but SEAL security limits may reject it for `N=8192`.
