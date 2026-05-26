# Feasibility: Increasing CKKS Parameters To N=4096 Or N=8192

This evaluates whether the ESP32-S3 CAM can eventually support larger `poly_modulus_degree` values. This document does not change the active parameters.

## Current Measured Baseline

Measured on the ESP32-S3 CAM with OPI PSRAM:

```text
N=2048
coeff_modulus_count=1
scale_bits=20
PSRAM total=8388608 bytes
PSRAM free before encrypt=8350264 bytes
ciphertext bytes=32881
root table bytes=32768
temporary tracked HE peak delta=131185 bytes
encrypt avg=462 ms
serialize avg=1 ms
```

The temporary tracked peak is almost exactly:

```text
c0 + c1 + serialized ciphertext + encryption scratch
```

For the current implementation:

```text
c0 = N * 8
c1 = N * 8
serialized ciphertext ~= 2 * N * 8 + header
scratch = 4 * N * 8
temporary peak ~= 8 * N * 8 + header
```

At `N=2048`:

```text
8 * 2048 * 8 + header ~= 131 KB
```

which matches the measured `131185` bytes.

## Single-Prime Scaling Estimate

If we only increase `N` but keep `coeff_modulus_count=1`, memory scales roughly linearly with `N`.

| N | Root Tables | Temporary HE Peak | Ciphertext Bytes | Estimated Encrypt Time |
|---:|---:|---:|---:|---:|
| 2048 | 32 KB | 131 KB | 32.9 KB | 0.46 s measured |
| 4096 | 64 KB | 262 KB | 65.6 KB | about 1.0 s |
| 8192 | 128 KB | 524 KB | 131 KB | about 2.1-2.4 s |

Plain conclusion: single-prime `N=4096` and `N=8192` look feasible in RAM/PSRAM.

But single-prime CKKS is still limited for server-side computation because it cannot support key-switching/relinearization/rotations in SEAL.

## Multi-Prime Scaling Estimate

Real CKKS computation needs multiple coefficient modulus primes. Let:

```text
K = coeff_modulus_count
```

Approximate sizes:

```text
public key package ~= 2 * K * N * 8 bytes
root tables ~= 2 * K * N * 8 bytes
ciphertext ~= 2 * K * N * 8 bytes + header
```

Temporary encryption memory depends on implementation strategy.

Simple full-RNS scratch strategy:

```text
temporary peak ~= 8 * K * N * 8 bytes
```

More memory-careful per-prime scratch strategy:

```text
temporary peak ~= (4*K + 4) * N * 8 bytes
```

The second approach is what we should aim for.

## Example Estimates

### N=4096, K=2

```text
public key package ~= 128 KB
root tables ~= 128 KB
ciphertext ~= 128 KB
temporary peak, full scratch ~= 512 KB
temporary peak, per-prime scratch ~= 384 KB
```

Likely feasible.

### N=4096, K=3

```text
public key package ~= 192 KB
root tables ~= 192 KB
ciphertext ~= 192 KB
temporary peak, full scratch ~= 768 KB
temporary peak, per-prime scratch ~= 512 KB
```

Likely feasible in PSRAM, but serial transfer becomes slow.

### N=8192, K=3

```text
public key package ~= 384 KB
root tables ~= 384 KB
ciphertext ~= 384 KB
temporary peak, full scratch ~= 1.5 MB
temporary peak, per-prime scratch ~= 1.0 MB
```

Likely feasible in PSRAM, but runtime and transfer time become real bottlenecks.

### N=8192, K=5

```text
public key package ~= 640 KB
root tables ~= 640 KB
ciphertext ~= 640 KB
temporary peak, full scratch ~= 2.5 MB
temporary peak, per-prime scratch ~= 1.6 MB
```

Possible with 8 MB PSRAM, but now engineering risk is high:

- slower encryption
- much larger serial/Wi-Fi transfers
- more fragmentation risk
- larger flash assets
- more exact SEAL compatibility work

## Transmission Bottleneck

The current serial protocol sends ciphertext as hex text.

Hex doubles the transfer size:

```text
32 KB ciphertext -> about 64 KB serial text
384 KB ciphertext -> about 768 KB serial text
640 KB ciphertext -> about 1.25 MB serial text
```

At `115200` baud, this becomes slow. Before moving to `N=8192` with multiple primes, we should add:

- binary serial framing, or
- Wi-Fi transfer, or
- SD-card write + transfer, depending on the target deployment.

## Security And Computation Depth

Rough SEAL/HomomorphicEncryption.org 128-bit total coefficient modulus limits:

```text
N=2048  -> about 54 total coeff modulus bits
N=4096  -> about 109 total coeff modulus bits
N=8192  -> about 218 total coeff modulus bits
```

Meaning:

- `N=2048`: good for this proof-of-compatibility prototype, not good for complex CKKS computation.
- `N=4096`: may support shallow computation with a small modulus chain.
- `N=8192`: much more realistic for useful CKKS workloads and rescale/relinearization/rotation support.

For complex approximations, `N=8192` is the first parameter size that starts to feel practically useful.

## Feasibility Conclusion

`N=4096`:

- Feasible on ESP32-S3 with OPI PSRAM.
- Good next experimental step.
- Can probably support a small multi-prime chain.
- Transfer size starts to matter but remains manageable.

`N=8192`:

- Feasible in memory if we are careful.
- Should use per-prime scratch reuse.
- Requires better transport than hex-over-115200 serial for comfort.
- More realistic for meaningful CKKS server computations.

Do not jump straight to `N=8192`. Recommended path:

```text
1. finish multi-prime RNS at N=2048/4096 on host mini core
2. test N=4096, K=2 on ESP32
3. add binary ciphertext transport
4. test N=4096, K=3
5. only then test N=8192
```

The ESP32 should still remain encryption-only. Relin keys, Galois keys, rescaling, rotations, and complex approximation evaluation belong on the PC/server.
