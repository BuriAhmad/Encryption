#pragma once

#include <cstddef>
#include <cstdint>

namespace he_esp
{
    struct MemoryEstimate
    {
        size_t plaintext_bytes{ 0 };
        size_t ciphertext_bytes{ 0 };
        size_t public_key_bytes{ 0 };
        size_t coeff_modulus_bytes{ 0 };
        size_t working_set_bytes{ 0 };
    };

    // Quick static estimate for planning PSRAM budgets.
    MemoryEstimate estimate_memory(size_t poly_modulus_degree, size_t coeff_modulus_count);
} // namespace he_esp
