#include "he_bundle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace he
{
    namespace
    {
        constexpr std::array<char, 8> kMagic{ 'H', 'E', 'B', 'N', 'D', 'L', '1', '\0' };

        template <typename T>
        void append_pod(std::vector<std::uint8_t> &dst, const T &value)
        {
            const auto *ptr = reinterpret_cast<const std::uint8_t *>(&value);
            dst.insert(dst.end(), ptr, ptr + sizeof(T));
        }

        template <typename T>
        T read_pod(const std::uint8_t *data, std::size_t size, std::size_t &offset)
        {
            if (offset + sizeof(T) > size)
            {
                throw std::runtime_error("bundle parse error: truncated POD field");
            }
            T out{};
            std::memcpy(&out, data + offset, sizeof(T));
            offset += sizeof(T);
            return out;
        }

        std::string trim(std::string_view sv)
        {
            std::size_t b = 0;
            std::size_t e = sv.size();
            while (b < e && std::isspace(static_cast<unsigned char>(sv[b])))
            {
                b++;
            }
            while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1])))
            {
                e--;
            }
            return std::string(sv.substr(b, e - b));
        }
    } // namespace

    std::uint32_t crc32(const std::uint8_t *data, std::size_t size)
    {
        // Standard IEEE CRC-32 (polynomial 0xEDB88320).
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < size; ++i)
        {
            crc ^= static_cast<std::uint32_t>(data[i]);
            for (int j = 0; j < 8; ++j)
            {
                const std::uint32_t mask = static_cast<std::uint32_t>(-(crc & 1u));
                crc = (crc >> 1u) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }

    std::vector<std::uint8_t> serialize_bundle(const BundleData &bundle)
    {
        BundleHeader hdr = bundle.header;
        hdr.magic = kMagic;
        hdr.format_version = kBundleFormatVersion;
        hdr.scheme = kSchemeCkks;

        hdr.coeff_modulus_section_size =
            static_cast<std::uint32_t>(bundle.payload.coeff_modulus_values.size() * sizeof(std::uint64_t));
        hdr.params_section_size = static_cast<std::uint32_t>(bundle.payload.params_blob.size());
        hdr.public_key_section_size = static_cast<std::uint32_t>(bundle.payload.public_key_blob.size());

        std::vector<std::uint8_t> out;
        out.reserve(sizeof(BundleHeader) + hdr.coeff_modulus_section_size + hdr.params_section_size +
                    hdr.public_key_section_size);

        append_pod(out, hdr);

        for (auto q : bundle.payload.coeff_modulus_values)
        {
            append_pod(out, q);
        }

        out.insert(out.end(), bundle.payload.params_blob.begin(), bundle.payload.params_blob.end());
        out.insert(out.end(), bundle.payload.public_key_blob.begin(), bundle.payload.public_key_blob.end());

        const std::size_t payload_offset = sizeof(BundleHeader);
        const auto payload_crc = crc32(out.data() + payload_offset, out.size() - payload_offset);

        hdr.payload_crc32 = payload_crc;
        std::memcpy(out.data(), &hdr, sizeof(BundleHeader));
        return out;
    }

    BundleView parse_bundle_view(const std::uint8_t *data, std::size_t size)
    {
        if (!data || size < sizeof(BundleHeader))
        {
            throw std::runtime_error("bundle parse error: too small");
        }

        std::size_t offset = 0;
        BundleView view;
        view.header = read_pod<BundleHeader>(data, size, offset);

        if (view.header.magic != kMagic)
        {
            throw std::runtime_error("bundle parse error: bad magic");
        }
        if (view.header.format_version != kBundleFormatVersion)
        {
            throw std::runtime_error("bundle parse error: unsupported format version");
        }
        if (view.header.scheme != kSchemeCkks)
        {
            throw std::runtime_error("bundle parse error: unsupported scheme");
        }

        const std::size_t expected_coeff_bytes =
            static_cast<std::size_t>(view.header.coeff_modulus_count) * sizeof(std::uint64_t);
        if (view.header.coeff_modulus_section_size != expected_coeff_bytes)
        {
            throw std::runtime_error("bundle parse error: coeff section size mismatch");
        }

        const std::size_t total_sections =
            static_cast<std::size_t>(view.header.coeff_modulus_section_size) +
            static_cast<std::size_t>(view.header.params_section_size) +
            static_cast<std::size_t>(view.header.public_key_section_size);

        if (sizeof(BundleHeader) + total_sections != size)
        {
            throw std::runtime_error("bundle parse error: file size mismatch");
        }

        const std::uint8_t *cursor = data + sizeof(BundleHeader);
        view.coeff_modulus_values = cursor;
        view.coeff_modulus_values_size = view.header.coeff_modulus_section_size;
        cursor += view.coeff_modulus_values_size;

        view.params_blob = cursor;
        view.params_blob_size = view.header.params_section_size;
        cursor += view.params_blob_size;

        view.public_key_blob = cursor;
        view.public_key_blob_size = view.header.public_key_section_size;

        const auto computed_crc = crc32(data + sizeof(BundleHeader), total_sections);
        if (computed_crc != view.header.payload_crc32)
        {
            throw std::runtime_error("bundle parse error: CRC mismatch");
        }

        return view;
    }

    BundleData parse_bundle(const std::vector<std::uint8_t> &bytes)
    {
        const auto view = parse_bundle_view(bytes.data(), bytes.size());

        BundleData out;
        out.header = view.header;

        out.payload.coeff_modulus_values.resize(view.header.coeff_modulus_count);
        std::memcpy(
            out.payload.coeff_modulus_values.data(), view.coeff_modulus_values,
            out.payload.coeff_modulus_values.size() * sizeof(std::uint64_t));

        out.payload.params_blob.assign(view.params_blob, view.params_blob + view.params_blob_size);
        out.payload.public_key_blob.assign(view.public_key_blob, view.public_key_blob + view.public_key_blob_size);

        return out;
    }

    std::vector<std::uint8_t> read_file_binary(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            throw std::runtime_error("failed to open file for read: " + path);
        }
        in.seekg(0, std::ios::end);
        const auto size = static_cast<std::size_t>(in.tellg());
        in.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> data(size);
        if (size && !in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(size)))
        {
            throw std::runtime_error("failed to read file: " + path);
        }
        return data;
    }

    void write_file_binary(const std::string &path, const std::vector<std::uint8_t> &data)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            throw std::runtime_error("failed to open file for write: " + path);
        }
        if (!data.empty())
        {
            out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        if (!out)
        {
            throw std::runtime_error("failed to write file: " + path);
        }
    }

    std::vector<double> parse_csv_doubles(std::string_view csv)
    {
        std::vector<double> out;
        std::size_t start = 0;
        while (start < csv.size())
        {
            auto comma = csv.find(',', start);
            if (comma == std::string_view::npos)
            {
                comma = csv.size();
            }
            const auto token = trim(csv.substr(start, comma - start));
            if (!token.empty())
            {
                out.push_back(std::stod(token));
            }
            start = comma + 1;
        }
        if (out.empty())
        {
            throw std::runtime_error("values list is empty");
        }
        return out;
    }

    std::string join_doubles(const std::vector<double> &values)
    {
        std::ostringstream oss;
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            if (i)
            {
                oss << ',';
            }
            oss << values[i];
        }
        return oss.str();
    }
} // namespace he
