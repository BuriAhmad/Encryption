#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

namespace he_esp
{
    constexpr uint32_t kBundleFormatVersion = 1;
    constexpr uint16_t kSchemeCkks = 2;

    struct BundleHeader
    {
        char magic[8]; // HEBNDL1\0
        uint32_t format_version;
        uint16_t seal_major;
        uint16_t seal_minor;
        uint16_t scheme;
        uint16_t reserved;
        uint32_t poly_modulus_degree;
        uint32_t coeff_modulus_count;
        uint32_t scale_bits;
        uint32_t coeff_modulus_section_size;
        uint32_t params_section_size;
        uint32_t public_key_section_size;
        uint32_t flags;
        uint32_t payload_crc32;
    };

    struct BundleView
    {
        BundleHeader header{};
        const uint8_t *coeff_modulus_values{ nullptr };
        const uint8_t *params_blob{ nullptr };
        const uint8_t *public_key_blob{ nullptr };
        size_t coeff_modulus_values_size{ 0 };
        size_t params_blob_size{ 0 };
        size_t public_key_blob_size{ 0 };
    };

    struct ParseResult
    {
        bool ok{ false };
        BundleView view{};
        const char *error{ nullptr };
    };

    uint32_t crc32_ieee(const uint8_t *data, size_t size);
    ParseResult parse_bundle(const uint8_t *data, size_t size);
} // namespace he_esp
