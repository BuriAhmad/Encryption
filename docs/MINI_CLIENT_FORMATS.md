# Mini Client Formats

Date: 2026-05-27

## 1) Host Bundle

Produced by:

```text
pc_tools/build/he_tool export-bundle
```

Purpose:

- PC-side artifact containing SEAL encryption parameters and SEAL public key.
- Also used with the secret key on the PC for decrypt/compute verification.

Contents:

- Fixed header (`HEBNDL1`) with sizes and CRCs.
- Coefficient modulus values.
- Serialized SEAL `EncryptionParameters` (`compr_mode=none`).
- Serialized SEAL `PublicKey` (`compr_mode=none`).

This file is not loaded directly on the ESP32.

## 2) Embedded Package v2

Produced by:

```text
pc_tools/build/he_tool export-embedded-package
```

Purpose:

- Compact ESP32 package.
- Avoids implementing full SEAL object deserialization on-device.
- Stores only the values needed for encode/encrypt/serialize.

Current constraints:

- CKKS only.
- Public-key encryption only.
- Public key must have two polynomials.
- Supports multi-prime key-level RNS packages.
- Fresh ciphertexts are serialized at the first data level, after dropping the special/key prime.

Layout:

```text
EmbeddedPackageHeader
q_values[K]
pk0[K][N]
pk1[K][N]
```

Important fields:

- `poly_modulus_degree`: `N`.
- `coeff_modulus_count`: key-level RNS count `K`.
- `coeff_modulus`: first modulus, retained for v1 compatibility.
- `parms_id`: SEAL first-context `parms_id`, matching the fresh ciphertext level.
- `payload_crc32`: CRC over `q_values`, `pk0`, and `pk1`.

## 3) Mini Ciphertext Output

Produced by:

- `pc_tools/build/he_tool encrypt-mini`
- ESP32 serial command `ENCRYPT ...`

Purpose:

- SEAL-compatible ciphertext bytes for PC-side `Ciphertext::load`.

Properties:

- `compr_mode=none`.
- Ciphertext size = 2.
- NTT form = true.
- `coeff_modulus_size = coeff_modulus_count - 1` for multi-prime packages.
- Nested DynArray serialization follows SEAL binary layout.

Validated by:

```text
pc_tools/build/he_tool decrypt-check
```
