#pragma once

#include "he_embedded_package.h"

#include <cstddef>
#include <cstdint>

namespace he_esp
{
    using RandomFillFn = void (*)(void *ctx, uint8_t *dst, size_t size);

    struct MiniCkksContext
    {
        uint16_t seal_major{ 0 };
        uint16_t seal_minor{ 0 };
        uint32_t poly_modulus_degree{ 0 };
        uint64_t coeff_modulus{ 0 };
        uint32_t scale_bits{ 0 };
        uint64_t parms_id[4]{};
        const uint64_t *pk0{ nullptr };
        const uint64_t *pk1{ nullptr };

        // Allocated tables/buffers.
        uint64_t *root_powers{ nullptr };
        uint64_t *inv_root_powers{ nullptr };
        uint64_t n_inv{ 0 };
    };

    bool mini_ckks_init_from_package(const EmbeddedPackageView &pkg, MiniCkksContext &ctx, const char **error);
    void mini_ckks_release(MiniCkksContext &ctx);

    // Encrypt a scalar value (repeated slot semantics) with CKKS-compatible scale.
    // out_c0/out_c1 must each have N uint64_t entries.
    bool mini_ckks_encrypt_scalar(
        const MiniCkksContext &ctx, double value, uint32_t scale_bits, RandomFillFn rand_fill, void *rand_ctx,
        uint64_t *out_c0, uint64_t *out_c1, const char **error);

    // Serialize fresh ciphertext (size=2, coeff_modulus_size=1, NTT form) to SEAL-compatible bytes.
    // Returns bytes written, or 0 on failure.
    size_t mini_ckks_serialize_ciphertext(
        const MiniCkksContext &ctx, double scale, const uint64_t *c0, const uint64_t *c1, uint8_t *out,
        size_t out_size);

    // Exact size needed by mini_ckks_serialize_ciphertext for this context.
    size_t mini_ckks_ciphertext_serialized_size(const MiniCkksContext &ctx);
} // namespace he_esp
