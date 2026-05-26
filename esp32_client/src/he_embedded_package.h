#pragma once

#include <cstddef>
#include <cstdint>

namespace he_esp
{
    constexpr uint32_t kEmbeddedPackageVersion = 1;

    struct EmbeddedPackageHeader
    {
        char magic[8]; // "HEPKG1\0"
        uint32_t version;
        uint16_t seal_major;
        uint16_t seal_minor;
        uint32_t poly_modulus_degree;
        uint64_t coeff_modulus;
        uint32_t scale_bits;
        uint32_t reserved;
        uint64_t parms_id[4];
        uint32_t pk_poly_size;
        uint32_t payload_crc32;
    };

    struct EmbeddedPackageView
    {
        EmbeddedPackageHeader header{};
        const uint64_t *pk0{ nullptr };
        const uint64_t *pk1{ nullptr };
    };

    struct EmbeddedPackageParseResult
    {
        bool ok{ false };
        EmbeddedPackageView view{};
        const char *error{ nullptr };
    };

    EmbeddedPackageParseResult parse_embedded_package(const uint8_t *data, size_t size);
} // namespace he_esp
