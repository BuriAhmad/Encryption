#include "he_memory_plan.h"

namespace he_esp
{
    MemoryEstimate estimate_memory(size_t poly_modulus_degree, size_t coeff_modulus_count)
    {
        MemoryEstimate m{};

        // CKKS plaintext in RNS-NTT form: N * K * 8 bytes.
        m.plaintext_bytes = poly_modulus_degree * coeff_modulus_count * sizeof(std::uint64_t);

        // Fresh ciphertext size is 2 polys: 2 * N * K * 8 bytes.
        m.ciphertext_bytes = 2 * m.plaintext_bytes;

        // Public key is also size-2 ciphertext at key level.
        m.public_key_bytes = 2 * m.plaintext_bytes;

        m.coeff_modulus_bytes = coeff_modulus_count * sizeof(std::uint64_t);

        // Conservative working set for v1: plain + cipher + pk + extra plain scratch.
        m.working_set_bytes = m.plaintext_bytes + m.ciphertext_bytes + m.public_key_bytes + m.plaintext_bytes;
        return m;
    }
} // namespace he_esp
