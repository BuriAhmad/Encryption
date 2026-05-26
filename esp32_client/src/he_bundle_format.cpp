#include "he_bundle_format.h"

#include <cstring>

namespace he_esp
{
    namespace
    {
        constexpr char kMagic[8] = { 'H', 'E', 'B', 'N', 'D', 'L', '1', '\0' };

        ParseResult fail(const char *error)
        {
            ParseResult r;
            r.ok = false;
            r.error = error;
            return r;
        }
    } // namespace

    uint32_t crc32_ieee(const uint8_t *data, size_t size)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i)
        {
            crc ^= static_cast<uint32_t>(data[i]);
            for (int j = 0; j < 8; ++j)
            {
                uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
                crc = (crc >> 1u) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }

    ParseResult parse_bundle(const uint8_t *data, size_t size)
    {
        if (!data || size < sizeof(BundleHeader))
        {
            return fail("bundle too small");
        }

        BundleHeader hdr{};
        std::memcpy(&hdr, data, sizeof(BundleHeader));

        if (std::memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0)
        {
            return fail("bad bundle magic");
        }
        if (hdr.format_version != kBundleFormatVersion)
        {
            return fail("unsupported bundle version");
        }
        if (hdr.scheme != kSchemeCkks)
        {
            return fail("unsupported HE scheme");
        }

        const size_t expected_coeff_bytes = static_cast<size_t>(hdr.coeff_modulus_count) * sizeof(uint64_t);
        if (static_cast<size_t>(hdr.coeff_modulus_section_size) != expected_coeff_bytes)
        {
            return fail("coeff section mismatch");
        }

        const size_t section_total = static_cast<size_t>(hdr.coeff_modulus_section_size) +
                                     static_cast<size_t>(hdr.params_section_size) +
                                     static_cast<size_t>(hdr.public_key_section_size);

        if (sizeof(BundleHeader) + section_total != size)
        {
            return fail("bundle size mismatch");
        }

        const uint8_t *payload = data + sizeof(BundleHeader);
        const uint32_t got_crc = crc32_ieee(payload, section_total);
        if (got_crc != hdr.payload_crc32)
        {
            return fail("bundle crc mismatch");
        }

        BundleView view{};
        view.header = hdr;
        view.coeff_modulus_values = payload;
        view.coeff_modulus_values_size = hdr.coeff_modulus_section_size;

        view.params_blob = view.coeff_modulus_values + view.coeff_modulus_values_size;
        view.params_blob_size = hdr.params_section_size;

        view.public_key_blob = view.params_blob + view.params_blob_size;
        view.public_key_blob_size = hdr.public_key_section_size;

        ParseResult ok{};
        ok.ok = true;
        ok.view = view;
        ok.error = nullptr;
        return ok;
    }
} // namespace he_esp
