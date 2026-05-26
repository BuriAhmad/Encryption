# Next Phase: Vectors, Multi-Prime RNS, and Evaluation Keys

This note explains what needs to change after the current scalar, single-prime prototype.

## Current Prototype

The ESP32 currently does:

```text
single double value -> scalar CKKS-style plaintext -> public-key encrypt -> SEAL ciphertext bytes
```

Current supported embedded shape:

```text
poly_modulus_degree = 2048
coeff_modulus_count = 1
scale_bits = 20
ciphertext size = 32881 bytes
```

This is enough to prove:

- ESP32 package loading works.
- ESP32 encryption works.
- ESP32 serialization is SEAL-compatible.
- PC-side SEAL can deserialize and decrypt the ESP32 ciphertext.

It is not yet enough for real CKKS vector workloads or deep encrypted computation.

## What The PC Computation Test Currently Uses

The current PC-side computation test is:

```text
((x + 1) + 2) * 3
```

In SEAL terms, it uses:

```text
Evaluator::add_plain_inplace
Evaluator::multiply_plain_inplace
```

It does not use:

```text
RelinKeys
GaloisKeys
Evaluator::multiply_inplace(ciphertext, ciphertext)
Evaluator::relinearize_inplace
Evaluator::rotate_vector_inplace
Evaluator::rescale_to_next_inplace
```

Why: the current test only multiplies by a plaintext constant, so ciphertext size does not grow and relinearization is not needed. There are also no slot rotations, so Galois keys are not needed.

## Why Current Parameters Cannot Use Relin/Galois Keys

The current ESP32 prototype uses one coefficient modulus prime:

```text
coeff_modulus_count = 1
```

Microsoft SEAL does not support key-switching with this one-prime setup. That means these are not available under the current test parameters:

```text
RelinKeys
GaloisKeys
rotations
ciphertext-ciphertext multiplication followed by relinearization
```

For serious PC/server computation, we need a multi-prime coefficient modulus chain.

## New PC Tool Support

The PC tool now includes an evaluation-key export command:

```bash
pc_tools/build/he_tool export-eval-keys \
  --bundle pc_tools/test_vectors/bundle.bin \
  --secret pc_tools/test_vectors/secret.bin \
  --relin-out pc_tools/test_vectors/relin_keys.bin \
  --galois-out pc_tools/test_vectors/galois_keys.bin \
  --galois-steps 1,2,4,8
```

For the current `N=2048`, single-prime prototype, this intentionally fails with a clear message because SEAL key-switching is unavailable.

For a multi-prime bundle, it works. A smoke test with `N=4096` and two 36-bit primes produced:

```text
bundle bytes = 131358
secret bytes = 65624
relin keys bytes = 131249
galois keys bytes for steps 1,2,4,8 = 557564
```

Important: these evaluation keys are for the PC/server only. The ESP32 encryption-only client should not store them.

## Vector CKKS Encoding On ESP32

True CKKS vector encoding is much heavier than the current scalar path.

Current scalar path:

```text
value -> repeated-slot plaintext shortcut
```

Full vector path:

```text
vector<double/complex> -> inverse canonical embedding -> coefficient polynomial -> RNS/NTT plaintext
```

This requires more machinery:

- complex FFT-like transform over CKKS slots
- bit-reversal/permutation logic matching SEAL
- floating-point rounding into coefficient modulus limbs
- RNS handling for every coefficient modulus prime
- exact SEAL-compatible plaintext layout

Recommended implementation order:

1. Keep the current scalar path stable.
2. Add PC-generated deterministic vector test cases.
3. Implement vector encode on the PC-side mini core first.
4. Compare mini plaintext/ciphertext behavior against SEAL.
5. Port the vector encoder to ESP32 only after the host version matches.

## Multi-Prime RNS On ESP32

The current embedded package stores:

```text
one q prime
pk0[N]
pk1[N]
```

Multi-prime support needs:

```text
q_count = K
q_values[K]
pk0[K][N]
pk1[K][N]
root tables[K][N]
c0[K][N]
c1[K][N]
```

The encryption math must run per modulus prime:

```text
for each q_i:
    sample/transform u under q_i
    compute c0_i = pk0_i * u_i + e0_i + plain_i
    compute c1_i = pk1_i * u_i + e1_i
```

To save RAM, scratch buffers should be reused one modulus at a time. Do not allocate all scratch buffers for all primes unless the benchmark says it is safe.

## Noise/Scale Management For Later

For larger computations on the PC/server, the important concepts are:

- `RelinKeys`: needed after ciphertext-ciphertext multiplication to shrink ciphertext size back down.
- `GaloisKeys`: needed for slot rotations and conjugation.
- `rescale_to_next`: needed after CKKS multiplication to reduce scale and move down the modulus chain.
- `mod_switch_to_inplace`: needed to align ciphertext/plaintext levels before addition.
- coefficient modulus chain: determines how many multiplications/rescales can happen.

The ESP32 only creates fresh ciphertexts. Noise management mainly happens on the PC/server after encryption.

## Practical Recommendation

Do not try to make the ESP32 perform evaluation. Keep it as:

```text
load params + public key -> encode -> encrypt -> serialize/send
```

Let the PC/server hold:

```text
secret key
relin keys
galois keys
evaluation code
rescale/mod-switch logic
```

That split keeps the embedded client small and makes complex computation feasible later.
