# Mini Client Formats (Current v1)

Date: 2026-05-16

## 1) Host Bundle (`bundle.bin`)

Produced by:
- `he_tool export-bundle`

Purpose:
- Full host/interop artifact containing SEAL parameter blob + SEAL public key blob.

Layout:
- Fixed header (`HEBNDL1`) with version/scheme/sizes/CRC
- coeff modulus values section
- serialized `EncryptionParameters` (SEAL binary, `compr_mode=none`)
- serialized `PublicKey` (SEAL binary, `compr_mode=none`)

## 2) Embedded Package (`embedded_package.bin`)

Produced by:
- `he_tool export-embedded-package --bundle ...`

Purpose:
- Compact package for the mini embedded encryption core.
- Avoids implementing full SEAL object deserialization on-device.

Current constraints:
- CKKS only
- `coeff_modulus_count == 1`
- fresh public key only (`size=2`, key-level NTT form)

Layout:
- Header magic `HEPKG1\0\0`
- SEAL major/minor version
- `poly_modulus_degree`
- single coeff modulus `q`
- scale bits
- `parms_id` (4 x uint64)
- `pk_poly_size` (= N)
- payload CRC32
- payload:
  - `pk0` NTT coefficients (`N * uint64`)
  - `pk1` NTT coefficients (`N * uint64`)

## 3) Mini Ciphertext Output (`mini_cipher.bin`)

Produced by:
- `he_tool encrypt-mini`
- or on-device via `mini_ckks_serialize_ciphertext`

Purpose:
- SEAL-compatible ciphertext bytes for PC-side `Ciphertext::load`.

Properties:
- `compr_mode=none`
- ciphertext size = 2
- coeff modulus size = 1
- NTT form = true
- nested DynArray serialization follows SEAL binary layout

Validated by:
- `he_tool decrypt-check --bundle ... --secret ... --cipher mini_cipher.bin`
