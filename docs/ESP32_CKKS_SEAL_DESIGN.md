# ESP32-S3 CKKS Encryption-Only Client (SEAL-Compatible)

Date: 2026-05-11  
Target: ESP32-S3 CAM + Arduino  
Scope: CKKS **encode + encrypt + serialize + send** only (no keygen/decrypt/eval)

---

## 1) Goal and Non-Goals

### Goal
Build a lightweight embedded client that produces CKKS ciphertexts which deserialize and decrypt correctly with Microsoft SEAL on PC/server.

### Strict Non-Goals
- No secret/public key generation on ESP32
- No decryption on ESP32
- No homomorphic operations (add/mul/relinearize/rotate/rescale) on ESP32
- No attempt to port full SEAL API/runtime

---

## 2) What Was Studied in SEAL (Minimum Required Path)

Repository studied: `https://github.com/microsoft/SEAL` (cloned on 2026-05-11)  
Version in local build config: SEAL 4.3.2 (`SEAL_VERSION_MAJOR=4`, `MINOR=3`, `PATCH=2`)

### Core CKKS encode path
- `native/src/seal/ckks.h`
- `native/src/seal/ckks.cpp`

Key behavior:
- slot count = `N/2`
- vector encode path:
  1. construct conjugate-symmetric slot vector
  2. inverse FFT (`transform_from_rev`)
  3. scale/round
  4. RNS decomposition into each `q_i`
  5. forward NTT into plaintext NTT form
- plaintext for CKKS encryption **must be NTT form**

### Public-key encryption path
- `native/src/seal/encryptor.cpp` (`Encryptor::encrypt_internal`, `encrypt_zero_internal`)
- `native/src/seal/util/rlwe.cpp` (`encrypt_zero_asymmetric`, sampling)

Key behavior:
- CKKS encrypt = encrypt-zero + add plaintext into `c0`
- randomness and noise sampling done internally
- ciphertext output size for fresh encrypt is 2 polys (`c0`, `c1`)

### Serialization path (compatibility-critical)
- `native/src/seal/ciphertext.h/.cpp`
- `native/src/seal/serialization.h/.cpp`
- `native/src/seal/dynarray.h`
- `native/src/seal/publickey.h` (public key wraps `Ciphertext`)

Key behavior:
- SEAL binary object always starts with 16-byte `SEALHeader`
  - magic `0xA15E`
  - header size `16`
  - version major/minor
  - compression mode
  - total serialized size
- `Ciphertext::save(..., compr_mode_type::none)` produces deterministic binary layout compatible with PC `Ciphertext::load`.

### Context/parameter validation and precompute path
- `native/src/seal/context.h/.cpp`
- `native/src/seal/util/ntt.h/.cpp`
- `native/src/seal/util/rns.h/.cpp`

Key behavior:
- builds NTT tables, RNS tools, modulus chain
- this is one of the heaviest memory components

---

## 3) Feasibility on ESP32-S3

## Bottom line
**Feasible** for encryption-only CKKS at `N=2048`, but only with a minimal design and tight parameter policy.

### Hard constraints that drive design
1. Internal SRAM is limited; persistent HE objects must mostly live in PSRAM.
2. Full SEAL-style dynamic allocation patterns can fragment heap if used naively.
3. CKKS encode path (FFT + RNS + NTT) is nontrivial but still manageable for `N=2048`.
4. Compression (zlib/zstd) is unnecessary on device and adds code/memory complexity.

### Feasibility recommendation
- Start with **single-prime CKKS level** at `N=2048` (no modulus switching on encrypt path).
- Use `compr_mode_type::none` for serialization.
- Keep all large buffers in PSRAM with a fixed-pool allocator.

---

## 4) Memory Analysis

## 4.1 Exact ciphertext/key size formulas (SEAL layout)

Raw polynomial storage:
- ciphertext raw bytes = `8 * N * K * T`
  - `N` = poly modulus degree
  - `K` = coeff modulus prime count at ciphertext level
  - `T` = ciphertext size (fresh encrypt => 2)
- public key raw bytes = `8 * N * K * 2` (public key is a size-2 ciphertext at key level)

For `N=2048`:
- if `K=1`: ciphertext raw = `8*2048*1*2 = 32768` bytes
- if key also has `K=1`: public key raw = `32768` bytes

## 4.2 Measured SEAL pool usage (host probe; indicative, not ESP absolute)

Measured with SEAL 4.3.2, compression disabled, CKKS encrypt-only flow:

Case A: `N=2048`, coeff mod bits `{50}` (single prime)
- after context init: **269,360 B**
- after `CKKSEncoder` init: **425,024 B**
- after keygen/public key: **425,024 B**
- after one encode+encrypt: **621,632 B**
- plaintext bytes: **16,384 B**
- ciphertext raw bytes: **32,768 B**
- ciphertext serialized size (`none`): **32,881 B**
- public key serialized size (`none`): **32,881 B**

Case B: `N=2048`, coeff mod bits `{27,27}` (two primes at key level)
- after context init: **901,088 B**
- after `CKKSEncoder` init: **925,680 B**
- after keygen/public key: **925,680 B**
- after one encode+encrypt: **1,155,056 B**
- ciphertext still at first data level (`K=1`) by SEAL chain rules
- public key serialized size (`none`): **65,649 B**

### Interpretation
- Two-prime chain at `N=2048` greatly increases precompute/state memory.
- For embedded encryption-only, single-prime configuration is dramatically lighter.

---

## 5) Security/Parameter Reality Check for `N=2048`

In SEAL security tables (`util/hestdparms.h`, `util/globals.cpp`):
- `N=2048`, 128-bit classical security upper bound total coeff modulus bits = **54**
- default 128-bit set for `N=2048` is a **single 54-bit prime**

Implication:
- `N=2048` can be secure only with a very small modulus budget.
- This is fine for encrypt-only ingestion (no deep HE circuit on device).

---

## 6) Architecture (Minimal Embedded Client)

## 6.1 PC/Server responsibilities (authoritative side)
- Generate parameters and keys using normal SEAL.
- Export device bundle:
  - SEAL version tag (major/minor)
  - CKKS params (`N`, coeff mod primes)
  - public key (serialized, `compr_mode_type::none`)
  - chosen scale policy
  - optional precomputed tables for faster boot

## 6.2 ESP32 responsibilities
- Load parameter bundle (flash or OTA-provisioned)
- Load public key
- Encode values
- Encrypt with public key
- Serialize ciphertext with SEAL-compatible binary layout
- Transmit ciphertext bytes to server

## 6.3 Recommended memory placement
- **Flash (const/provisioned):**
  - parameter bundle
  - serialized public key blob (or compact packed form)
  - static lookup tables (if precomputed offline)
- **PSRAM (long-lived):**
  - public key expanded polys
  - context/NTT/RNS tables
  - encoder state
  - reusable plaintext/ciphertext buffers
- **Internal SRAM (fast/small):**
  - control structs, short temporary scalars, network metadata

## 6.4 Allocation strategy
- Avoid frequent `new/delete`.
- Use arena/fixed-pool style allocations for HE buffers.
- Reuse:
  - one plaintext buffer
  - one ciphertext buffer
  - one encode scratch region
  - one encrypt scratch region

---

## 7) SEAL Compatibility Strategy

### Required invariants
1. Same scheme (`ckks`)
2. Same `poly_modulus_degree`
3. Same coeff modulus primes and order
4. Plaintext in correct NTT form for its `parms_id`
5. Ciphertext metadata consistent (`parms_id`, size, scale, NTT flag)
6. Serialized format uses SEAL header + member layout exactly

### Serialization policy (embedded)
- Force `compr_mode_type::none` for all outbound blobs.
- This avoids zlib/zstd dependencies and reduces risk.

### Wire format recommendation
- Application frame:
  - magic/version for your protocol
  - payload type (`SEAL_CIPHERTEXT`)
  - payload length
  - raw SEAL ciphertext bytes
  - CRC32 (transport-level integrity)
- Keep SEAL payload untouched inside your frame.

---

## 8) Key Engineering Tradeoffs (Decisions)

## Decision A: Full SEAL port vs minimal rewrite
- **Recommended:** minimal rewrite of CKKS encrypt-only core with SEAL-compatible serialization.
- Reason: full SEAL pulls in large STL/exception/feature surface not needed on ESP32.

## Decision B: Single-prime (`K=1`) vs multi-prime
- **Recommended first target:** single-prime at `N=2048` (e.g., 50–54-bit prime).
- Reason: far lower memory footprint and simpler path (no key-level/data-level chain complications).

## Decision C: Vector encode vs scalar-only first
- If sensor pipeline sends one value at a time, scalar encode-first is much lighter operationally.
- Full vector CKKS encode should be milestone-gated.

## Decision D: Key storage
- Store public key in flash as serialized blob; load/expand once at boot into PSRAM.
- Do not regenerate anything key-related on device.

---

## 9) Risks and Limits

1. **Memory fragmentation risk** with uncontrolled dynamic allocations  
   Mitigation: fixed buffers + pool allocator + boot-time allocation only.

2. **Cross-version serialization mismatch**  
   Mitigation: pin SEAL major/minor version in bundle; reject mismatch.

3. **RNG quality and portability**  
   Mitigation: tie RNG to ESP32 hardware randomness + deterministic PRNG expansion strategy compatible with implementation.

4. **Performance at larger N/chains**  
   Mitigation: freeze `N=2048`, single-prime for v1.

5. **Hidden complexity in vector encode path**  
   Mitigation: stage rollout (scalar encode first, vector second).

---

## 10) Incremental Implementation Roadmap

## Milestone 0: Golden PC reference
- Build PC harness with SEAL:
  - fixed params/key
  - encode/encrypt/save
  - decrypt/verify
- Emit golden vectors/artifacts for embedded tests.

## Milestone 1: Parameter + key loader on ESP32
- Parse bundle from flash.
- Validate parameter IDs and sizes.
- Instantiate internal structures.

## Milestone 2: Math primitives
- Modular arithmetic helpers
- RNS poly container
- NTT forward/inverse for required moduli

## Milestone 3: CKKS encoding
- Scalar encode path first
- Vector encode path second
- Match SEAL output numerically for controlled test vectors

## Milestone 4: Public-key encryption
- Implement encrypt-zero-asymmetric path + add plaintext into `c0`
- Match SEAL ciphertext structural invariants

## Milestone 5: Serialization
- Implement SEAL-compatible ciphertext serialization (`none`)
- Byte-for-byte header validity checks with PC

## Milestone 6: Interoperability tests
- ESP ciphertext -> PC `Ciphertext::load` -> decrypt -> compare expected values
- Regression suite over multiple values/scales

## Milestone 7: Bench + memory telemetry
- Peak heap/PSRAM usage
- encode/encrypt latency
- throughput at target send interval

---

## 11) Test Completion Criteria (Must Pass)

1. PC can `load` ESP ciphertext with standard SEAL APIs.
2. Decrypted result matches expected CKKS value tolerance.
3. No runtime allocation failures under stress.
4. Memory usage recorded (peak internal RAM + PSRAM).
5. Deterministic test vectors available for regression.

---

## 12) Practical v1 Recommendation

For first working end-to-end:
- `N = 2048`
- coeff modulus count = **1**
- total coeff modulus bits <= 54
- `compr_mode = none`
- single ciphertext at a time, reused buffers
- PC handles all heavy HE lifecycle

This gives the highest chance of reliable ESP32 operation while preserving SEAL compatibility.

