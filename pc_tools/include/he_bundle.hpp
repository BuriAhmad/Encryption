#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace he
{
    constexpr std::uint32_t kBundleFormatVersion = 1;
    constexpr std::uint16_t kSchemeCkks = 2;

    struct BundleHeader
    {
        std::array<char, 8> magic{}; // "HEBNDL1\0"
        std::uint32_t format_version{ kBundleFormatVersion };
        std::uint16_t seal_major{ 0 };
        std::uint16_t seal_minor{ 0 };
        std::uint16_t scheme{ kSchemeCkks };
        std::uint16_t reserved{ 0 };
        std::uint32_t poly_modulus_degree{ 0 };
        std::uint32_t coeff_modulus_count{ 0 };
        std::uint32_t scale_bits{ 0 };
        std::uint32_t coeff_modulus_section_size{ 0 }; // count * uint64_t
        std::uint32_t params_section_size{ 0 }; // SEAL serialized EncryptionParameters (none)
        std::uint32_t public_key_section_size{ 0 }; // SEAL serialized PublicKey (none)
        std::uint32_t flags{ 0 };
        std::uint32_t payload_crc32{ 0 };
    };

    static_assert(sizeof(BundleHeader) == 52, "BundleHeader size mismatch");

    struct BundlePayload
    {
        std::vector<std::uint64_t> coeff_modulus_values;
        std::vector<std::uint8_t> params_blob;
        std::vector<std::uint8_t> public_key_blob;
    };

    struct BundleData
    {
        BundleHeader header;
        BundlePayload payload;
    };

    struct BundleView
    {
        BundleHeader header{};
        const std::uint8_t *coeff_modulus_values{ nullptr };
        const std::uint8_t *params_blob{ nullptr };
        const std::uint8_t *public_key_blob{ nullptr };
        std::size_t coeff_modulus_values_size{ 0 };
        std::size_t params_blob_size{ 0 };
        std::size_t public_key_blob_size{ 0 };
    };

    std::vector<std::uint8_t> serialize_bundle(const BundleData &bundle);
    BundleData parse_bundle(const std::vector<std::uint8_t> &bytes);
    BundleView parse_bundle_view(const std::uint8_t *data, std::size_t size);

    std::uint32_t crc32(const std::uint8_t *data, std::size_t size);

    std::vector<std::uint8_t> read_file_binary(const std::string &path);
    void write_file_binary(const std::string &path, const std::vector<std::uint8_t> &data);

    std::vector<double> parse_csv_doubles(std::string_view csv);
    std::string join_doubles(const std::vector<double> &values);
} // namespace he
