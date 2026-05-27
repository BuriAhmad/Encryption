#include "he_embedded_package.h"

#include <cstring>

namespace he_esp
{
    static_assert(sizeof(EmbeddedPackageHeader) % alignof(uint64_t) == 0, "package payload must be uint64-aligned");

    namespace
    {
        constexpr char kMagic[8] = { 'H', 'E', 'P', 'K', 'G', '1', '\0', '\0' };

        uint32_t crc32_local(const uint8_t *data, size_t size)
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

        EmbeddedPackageParseResult fail(const char *error)
        {
            EmbeddedPackageParseResult r;
            r.ok = false;
            r.error = error;
            return r;
        }
    } // namespace

    EmbeddedPackageParseResult parse_embedded_package(const uint8_t *data, size_t size)
    {
        if (!data || size < sizeof(EmbeddedPackageHeader))
        {
            return fail("embedded package too small");
        }

        EmbeddedPackageHeader header{};
        std::memcpy(&header, data, sizeof(EmbeddedPackageHeader));

        if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0)
        {
            return fail("bad embedded package magic");
        }
        if (header.version != 1 && header.version != kEmbeddedPackageVersion)
        {
            return fail("unsupported embedded package version");
        }
        if (header.poly_modulus_degree == 0 || (header.poly_modulus_degree & (header.poly_modulus_degree - 1)) != 0)
        {
            return fail("invalid poly_modulus_degree");
        }
        if (header.pk_poly_size != header.poly_modulus_degree)
        {
            return fail("pk poly size mismatch");
        }

        const uint32_t coeff_modulus_count =
            (header.version == 1) ? 1u : header.coeff_modulus_count;
        if (coeff_modulus_count == 0)
        {
            return fail("invalid coeff_modulus_count");
        }

        const size_t coeff_moduli_bytes =
            (header.version == 1) ? 0 : static_cast<size_t>(coeff_modulus_count) * sizeof(uint64_t);
        const size_t pk_bytes =
            static_cast<size_t>(header.pk_poly_size) * static_cast<size_t>(coeff_modulus_count) * sizeof(uint64_t);
        const size_t payload_bytes = coeff_moduli_bytes + (pk_bytes * 2);
        if (sizeof(EmbeddedPackageHeader) + payload_bytes != size)
        {
            return fail("embedded package size mismatch");
        }

        const uint8_t *payload = data + sizeof(EmbeddedPackageHeader);
        if (crc32_local(payload, payload_bytes) != header.payload_crc32)
        {
            return fail("embedded package crc mismatch");
        }

        EmbeddedPackageParseResult ok{};
        ok.ok = true;
        ok.error = nullptr;
        ok.view.header = header;
        ok.view.header.coeff_modulus_count = coeff_modulus_count;
        ok.view.coeff_moduli = (header.version == 1) ? nullptr : reinterpret_cast<const uint64_t *>(payload);
        ok.view.pk0 = reinterpret_cast<const uint64_t *>(payload + coeff_moduli_bytes);
        ok.view.pk1 = reinterpret_cast<const uint64_t *>(payload + coeff_moduli_bytes + pk_bytes);
        return ok;
    }
} // namespace he_esp
