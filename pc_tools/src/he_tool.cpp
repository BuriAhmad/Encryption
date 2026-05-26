#include "he_bundle.hpp"
#include "../../esp32_client/src/he_embedded_package.h"
#include "../../esp32_client/src/mini_ckks_client.h"

#include <seal/seal.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace seal;

namespace
{
    std::map<std::string, std::string> parse_kv_args(int argc, char **argv, int start_index)
    {
        std::map<std::string, std::string> out;
        for (int i = start_index; i < argc; ++i)
        {
            const std::string key = argv[i];
            if (key.rfind("--", 0) != 0)
            {
                throw std::runtime_error("invalid argument: " + key);
            }
            if (i + 1 >= argc)
            {
                throw std::runtime_error("missing value for argument: " + key);
            }
            out[key] = argv[++i];
        }
        return out;
    }

    std::string require_arg(const std::map<std::string, std::string> &args, const std::string &key)
    {
        auto it = args.find(key);
        if (it == args.end())
        {
            throw std::runtime_error("missing required argument " + key);
        }
        return it->second;
    }

    std::optional<std::string> optional_arg(const std::map<std::string, std::string> &args, const std::string &key)
    {
        auto it = args.find(key);
        if (it == args.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::vector<int> parse_csv_ints(const std::string &csv)
    {
        std::vector<int> out;
        std::size_t start = 0;
        while (start < csv.size())
        {
            auto comma = csv.find(',', start);
            if (comma == std::string::npos)
            {
                comma = csv.size();
            }
            auto token = csv.substr(start, comma - start);
            if (!token.empty())
            {
                out.push_back(std::stoi(token));
            }
            start = comma + 1;
        }
        if (out.empty())
        {
            throw std::runtime_error("coeff bits list is empty");
        }
        return out;
    }

    std::optional<prng_seed_type> parse_seed_hex(const std::optional<std::string> &seed_hex_opt)
    {
        if (!seed_hex_opt.has_value())
        {
            return std::nullopt;
        }

        std::string hex = *seed_hex_opt;
        if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0)
        {
            hex = hex.substr(2);
        }

        if (hex.size() != 128)
        {
            throw std::runtime_error("seed hex must be exactly 128 hex chars (64 bytes)");
        }

        std::array<std::uint8_t, 64> bytes{};
        for (std::size_t i = 0; i < 64; ++i)
        {
            const auto byte_str = hex.substr(i * 2, 2);
            bytes[i] = static_cast<std::uint8_t>(std::stoul(byte_str, nullptr, 16));
        }

        prng_seed_type seed{};
        for (std::size_t i = 0; i < seed.size(); ++i)
        {
            std::uint64_t w = 0;
            for (int b = 0; b < 8; ++b)
            {
                w |= static_cast<std::uint64_t>(bytes[i * 8 + b]) << (8 * b);
            }
            seed[i] = w;
        }
        return seed;
    }

    std::uint64_t parse_u64_or_default(const std::map<std::string, std::string> &args, const std::string &key, std::uint64_t dflt)
    {
        if (!args.count(key))
        {
            return dflt;
        }
        return static_cast<std::uint64_t>(std::stoull(args.at(key)));
    }

    std::string seed_to_hex(const prng_seed_type &seed)
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (auto w : seed)
        {
            for (int i = 0; i < 8; ++i)
            {
                const auto byte = static_cast<std::uint8_t>((w >> (8 * i)) & 0xFFu);
                oss << std::setw(2) << static_cast<unsigned int>(byte);
            }
        }
        return oss.str();
    }

    std::shared_ptr<UniformRandomGeneratorFactory> make_rng_factory(const std::optional<prng_seed_type> &seed)
    {
        if (!seed.has_value())
        {
            return UniformRandomGeneratorFactory::DefaultFactory();
        }
        return std::make_shared<Blake2xbPRNGFactory>(*seed);
    }

    template <typename T>
    std::vector<std::uint8_t> save_to_none_bytes(const T &obj)
    {
        const auto expected = static_cast<std::size_t>(obj.save_size(compr_mode_type::none));
        std::vector<std::uint8_t> out(expected);
        auto *out_ptr = reinterpret_cast<seal_byte *>(out.data());
        const auto wrote = static_cast<std::size_t>(obj.save(out_ptr, out.size(), compr_mode_type::none));
        out.resize(wrote);
        return out;
    }

    EncryptionParameters load_params_from_blob(const std::vector<std::uint8_t> &blob)
    {
        EncryptionParameters parms;
        const auto *in_ptr = reinterpret_cast<const seal_byte *>(blob.data());
        parms.load(in_ptr, blob.size());
        return parms;
    }

    PublicKey load_pk_from_blob(const SEALContext &context, const std::vector<std::uint8_t> &blob)
    {
        PublicKey pk;
        const auto *in_ptr = reinterpret_cast<const seal_byte *>(blob.data());
        pk.load(context, in_ptr, blob.size());
        return pk;
    }

    SecretKey load_sk_from_file(const SEALContext &context, const std::string &path)
    {
        SecretKey sk;
        auto bytes = he::read_file_binary(path);
        const auto *in_ptr = reinterpret_cast<const seal_byte *>(bytes.data());
        sk.load(context, in_ptr, bytes.size());
        return sk;
    }

    Ciphertext load_ct_from_file(const SEALContext &context, const std::string &path)
    {
        Ciphertext ct;
        auto bytes = he::read_file_binary(path);
        const auto *in_ptr = reinterpret_cast<const seal_byte *>(bytes.data());
        ct.load(context, in_ptr, bytes.size());
        return ct;
    }

    void print_usage()
    {
        std::cout << "he_tool commands:\n"
                  << "  export-bundle --bundle-out <path> --secret-out <path> [--poly 2048] [--coeff-bits 50] [--scale-bits 20] [--seed-hex <128hex>]\n"
                  << "  export-embedded-package --bundle <path> --out <path>\n"
                  << "  encrypt-from-bundle --bundle <path> --out <path> --values <csv> [--scale-bits <n>] [--seed-hex <128hex>]\n"
                  << "  encrypt-mini --package <path> --out <path> --value <double> [--scale-bits <n>] [--rng-seed <u64>]\n"
                  << "  decrypt-check --bundle <path> --secret <path> --cipher <path> [--expected <csv>] [--print-slots <n>] [--max-abs-err <e>]\n"
                  << "               [--compute-after-pass 1] [--compute-a 1.0] [--compute-b 2.0] [--compute-c 3.0] [--compute-max-abs-err 0.2]\n";
    }

    int cmd_export_bundle(const std::map<std::string, std::string> &args)
    {
        const auto bundle_out = require_arg(args, "--bundle-out");
        const auto secret_out = require_arg(args, "--secret-out");
        const auto poly = static_cast<std::size_t>(std::stoul(args.count("--poly") ? args.at("--poly") : "2048"));
        const auto coeff_bits = parse_csv_ints(args.count("--coeff-bits") ? args.at("--coeff-bits") : "50");
        const auto scale_bits = static_cast<std::uint32_t>(
            std::stoul(args.count("--scale-bits") ? args.at("--scale-bits") : "20"));

        const auto seed = parse_seed_hex(optional_arg(args, "--seed-hex"));

        EncryptionParameters parms(scheme_type::ckks);
        parms.set_random_generator(make_rng_factory(seed));
        parms.set_poly_modulus_degree(poly);
        parms.set_coeff_modulus(CoeffModulus::Create(poly, coeff_bits));

        SEALContext context(parms, true, sec_level_type::tc128);
        if (!context.parameters_set())
        {
            throw std::runtime_error("SEALContext parameters_set=false");
        }

        KeyGenerator keygen(context);
        PublicKey pk;
        keygen.create_public_key(pk);
        SecretKey sk = keygen.secret_key();

        he::BundleData bundle;
        bundle.header.seal_major = SEAL_VERSION_MAJOR;
        bundle.header.seal_minor = SEAL_VERSION_MINOR;
        bundle.header.poly_modulus_degree = static_cast<std::uint32_t>(poly);
        bundle.header.coeff_modulus_count = static_cast<std::uint32_t>(parms.coeff_modulus().size());
        bundle.header.scale_bits = scale_bits;
        bundle.header.flags = seed.has_value() ? 1u : 0u;

        for (const auto &q : parms.coeff_modulus())
        {
            bundle.payload.coeff_modulus_values.push_back(q.value());
        }

        bundle.payload.params_blob = save_to_none_bytes(parms);
        bundle.payload.public_key_blob = save_to_none_bytes(pk);

        auto bundle_bytes = he::serialize_bundle(bundle);
        he::write_file_binary(bundle_out, bundle_bytes);

        auto sk_bytes = save_to_none_bytes(sk);
        he::write_file_binary(secret_out, sk_bytes);

        std::cout << "bundle_out=" << bundle_out << " bytes=" << bundle_bytes.size() << "\n";
        std::cout << "secret_out=" << secret_out << " bytes=" << sk_bytes.size() << "\n";
        std::cout << "poly=" << poly << " coeff_count=" << coeff_bits.size() << " scale_bits=" << scale_bits << "\n";
        if (seed.has_value())
        {
            std::cout << "seed_hex=" << seed_to_hex(*seed) << "\n";
        }
        return 0;
    }

    int cmd_encrypt_from_bundle(const std::map<std::string, std::string> &args)
    {
        const auto bundle_path = require_arg(args, "--bundle");
        const auto out_path = require_arg(args, "--out");
        const auto values_csv = require_arg(args, "--values");
        auto values = he::parse_csv_doubles(values_csv);

        const auto seed = parse_seed_hex(optional_arg(args, "--seed-hex"));

        auto bundle_bytes = he::read_file_binary(bundle_path);
        auto bundle = he::parse_bundle(bundle_bytes);

        auto parms = load_params_from_blob(bundle.payload.params_blob);
        parms.set_random_generator(make_rng_factory(seed));

        SEALContext context(parms, true, sec_level_type::tc128);
        if (!context.parameters_set())
        {
            throw std::runtime_error("SEALContext parameters_set=false from bundle params");
        }

        PublicKey pk = load_pk_from_blob(context, bundle.payload.public_key_blob);
        CKKSEncoder encoder(context);
        Encryptor encryptor(context, pk);

        const std::uint32_t scale_bits = args.count("--scale-bits")
                                             ? static_cast<std::uint32_t>(std::stoul(args.at("--scale-bits")))
                                             : bundle.header.scale_bits;
        const double scale = std::pow(2.0, static_cast<int>(scale_bits));

        Plaintext plain;
        encoder.encode(values, scale, plain);

        Ciphertext ct;
        encryptor.encrypt(plain, ct);
        auto ct_bytes = save_to_none_bytes(ct);
        he::write_file_binary(out_path, ct_bytes);

        std::cout << "cipher_out=" << out_path << " bytes=" << ct_bytes.size() << "\n";
        std::cout << "values=" << he::join_doubles(values) << "\n";
        std::cout << "scale_bits=" << scale_bits << "\n";
        if (seed.has_value())
        {
            std::cout << "seed_hex=" << seed_to_hex(*seed) << "\n";
        }

        return 0;
    }

    int cmd_export_embedded_package(const std::map<std::string, std::string> &args)
    {
        const auto bundle_path = require_arg(args, "--bundle");
        const auto out_path = require_arg(args, "--out");

        auto bundle_bytes = he::read_file_binary(bundle_path);
        auto bundle = he::parse_bundle(bundle_bytes);

        auto parms = load_params_from_blob(bundle.payload.params_blob);
        SEALContext context(parms, true, sec_level_type::tc128);
        if (!context.parameters_set())
        {
            throw std::runtime_error("SEALContext parameters_set=false from bundle params");
        }
        if (parms.coeff_modulus().size() != 1)
        {
            throw std::runtime_error("embedded package currently supports coeff_modulus_count == 1 only");
        }

        PublicKey pk = load_pk_from_blob(context, bundle.payload.public_key_blob);
        if (pk.data().size() != 2 || pk.data().coeff_modulus_size() != 1)
        {
            throw std::runtime_error("public key is not size-2 with coeff_modulus_size=1");
        }

        he_esp::EmbeddedPackageHeader hdr{};
        const char magic[8] = { 'H', 'E', 'P', 'K', 'G', '1', '\0', '\0' };
        std::memcpy(hdr.magic, magic, sizeof(magic));
        hdr.version = he_esp::kEmbeddedPackageVersion;
        hdr.seal_major = bundle.header.seal_major;
        hdr.seal_minor = bundle.header.seal_minor;
        hdr.poly_modulus_degree = static_cast<uint32_t>(parms.poly_modulus_degree());
        hdr.coeff_modulus = parms.coeff_modulus()[0].value();
        hdr.scale_bits = bundle.header.scale_bits;
        hdr.pk_poly_size = hdr.poly_modulus_degree;
        std::memcpy(hdr.parms_id, pk.parms_id().data(), sizeof(hdr.parms_id));

        const auto poly_bytes = static_cast<size_t>(hdr.pk_poly_size) * sizeof(uint64_t);
        std::vector<uint8_t> payload(poly_bytes * 2);
        std::memcpy(payload.data(), pk.data().data(0), poly_bytes);
        std::memcpy(payload.data() + poly_bytes, pk.data().data(1), poly_bytes);
        hdr.payload_crc32 = he::crc32(payload.data(), payload.size());

        std::vector<uint8_t> out(sizeof(hdr) + payload.size());
        std::memcpy(out.data(), &hdr, sizeof(hdr));
        std::memcpy(out.data() + sizeof(hdr), payload.data(), payload.size());
        he::write_file_binary(out_path, out);

        std::cout << "embedded_package_out=" << out_path << " bytes=" << out.size() << "\n";
        std::cout << "N=" << hdr.poly_modulus_degree << " q=" << hdr.coeff_modulus << " scale_bits=" << hdr.scale_bits
                  << "\n";
        return 0;
    }

    struct XorShift64
    {
        uint64_t s{ 0x9e3779b97f4a7c15ULL };
    };

    void xorshift_fill(void *ctx, uint8_t *dst, size_t size)
    {
        auto *st = reinterpret_cast<XorShift64 *>(ctx);
        size_t off = 0;
        while (off < size)
        {
            uint64_t x = st->s;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            st->s = x;
            for (int i = 0; i < 8 && off < size; ++i)
            {
                dst[off++] = static_cast<uint8_t>((x >> (8 * i)) & 0xFFu);
            }
        }
    }

    int cmd_encrypt_mini(const std::map<std::string, std::string> &args)
    {
        const auto pkg_path = require_arg(args, "--package");
        const auto out_path = require_arg(args, "--out");
        const auto value = std::stod(require_arg(args, "--value"));
        const auto rng_seed = parse_u64_or_default(args, "--rng-seed", 0x123456789abcdefULL);

        auto pkg_bytes = he::read_file_binary(pkg_path);
        auto parsed = he_esp::parse_embedded_package(pkg_bytes.data(), pkg_bytes.size());
        if (!parsed.ok)
        {
            throw std::runtime_error(std::string("embedded package parse failed: ") + parsed.error);
        }

        he_esp::MiniCkksContext ctx{};
        const char *err = nullptr;
        if (!he_esp::mini_ckks_init_from_package(parsed.view, ctx, &err))
        {
            throw std::runtime_error(std::string("mini init failed: ") + (err ? err : "unknown"));
        }

        const auto n = ctx.poly_modulus_degree;
        std::vector<uint64_t> c0(n), c1(n);
        XorShift64 rng{ rng_seed };

        const uint32_t scale_bits = args.count("--scale-bits")
                                        ? static_cast<uint32_t>(std::stoul(args.at("--scale-bits")))
                                        : ctx.scale_bits;
        if (!he_esp::mini_ckks_encrypt_scalar(ctx, value, scale_bits, xorshift_fill, &rng, c0.data(), c1.data(), &err))
        {
            he_esp::mini_ckks_release(ctx);
            throw std::runtime_error(std::string("mini encrypt failed: ") + (err ? err : "unknown"));
        }

        const double scale = std::pow(2.0, static_cast<int>(scale_bits));
        std::vector<uint8_t> ct_bytes(he_esp::mini_ckks_ciphertext_serialized_size(ctx));
        size_t wrote =
            he_esp::mini_ckks_serialize_ciphertext(ctx, scale, c0.data(), c1.data(), ct_bytes.data(), ct_bytes.size());
        he_esp::mini_ckks_release(ctx);
        if (!wrote)
        {
            throw std::runtime_error("mini ciphertext serialization failed");
        }
        ct_bytes.resize(wrote);
        he::write_file_binary(out_path, ct_bytes);
        std::cout << "mini_cipher_out=" << out_path << " bytes=" << ct_bytes.size() << "\n";
        std::cout << "value=" << value << " scale_bits=" << scale_bits << " rng_seed=" << rng_seed << "\n";
        return 0;
    }

    int cmd_decrypt_check(const std::map<std::string, std::string> &args)
    {
        const auto bundle_path = require_arg(args, "--bundle");
        const auto secret_path = require_arg(args, "--secret");
        const auto cipher_path = require_arg(args, "--cipher");

        auto bundle_bytes = he::read_file_binary(bundle_path);
        auto bundle = he::parse_bundle(bundle_bytes);

        auto parms = load_params_from_blob(bundle.payload.params_blob);
        SEALContext context(parms, true, sec_level_type::tc128);
        if (!context.parameters_set())
        {
            throw std::runtime_error("SEALContext parameters_set=false from bundle params");
        }

        SecretKey sk = load_sk_from_file(context, secret_path);
        Ciphertext ct = load_ct_from_file(context, cipher_path);

        Decryptor decryptor(context, sk);
        CKKSEncoder encoder(context);

        Plaintext plain;
        decryptor.decrypt(ct, plain);

        std::vector<double> decoded(encoder.slot_count());
        encoder.decode(plain, decoded);

        const std::size_t print_slots = args.count("--print-slots")
                                            ? static_cast<std::size_t>(std::stoul(args.at("--print-slots")))
                                            : std::size_t(8);

        std::cout << "decoded_first=";
        for (std::size_t i = 0; i < std::min(print_slots, decoded.size()); ++i)
        {
            if (i)
            {
                std::cout << ',';
            }
            std::cout << decoded[i];
        }
        std::cout << "\n";

        bool decrypt_check_pass = true;
        std::vector<double> expected_vals;
        if (args.count("--expected"))
        {
            expected_vals = he::parse_csv_doubles(args.at("--expected"));
            if (expected_vals.size() > decoded.size())
            {
                throw std::runtime_error("expected list larger than slot count");
            }

            double max_abs_err = 0.0;
            for (std::size_t i = 0; i < expected_vals.size(); ++i)
            {
                max_abs_err = std::max(max_abs_err, std::fabs(decoded[i] - expected_vals[i]));
            }
            const double threshold = args.count("--max-abs-err") ? std::stod(args.at("--max-abs-err")) : 0.1;
            std::cout << "expected=" << he::join_doubles(expected_vals) << "\n";
            std::cout << "max_abs_error=" << std::setprecision(17) << max_abs_err << "\n";
            std::cout << "max_abs_error_threshold=" << threshold << "\n";
            decrypt_check_pass = (max_abs_err <= threshold);
            std::cout << "check=" << (decrypt_check_pass ? "PASS" : "FAIL") << "\n";
            if (!decrypt_check_pass)
            {
                return 3;
            }
        }

        const bool compute_after_pass =
            args.count("--compute-after-pass") ? (std::stoi(args.at("--compute-after-pass")) != 0) : true;
        if (compute_after_pass && decrypt_check_pass && !expected_vals.empty())
        {
            const double a = args.count("--compute-a") ? std::stod(args.at("--compute-a")) : 1.0;
            const double b = args.count("--compute-b") ? std::stod(args.at("--compute-b")) : 2.0;
            const double c = args.count("--compute-c") ? std::stod(args.at("--compute-c")) : 3.0;
            const double compute_threshold =
                args.count("--compute-max-abs-err") ? std::stod(args.at("--compute-max-abs-err")) : 0.2;

            Evaluator evaluator(context);
            Ciphertext eval_ct = ct;

            Plaintext plain_a, plain_b, plain_c;
            encoder.encode(a, eval_ct.parms_id(), eval_ct.scale(), plain_a);
            encoder.encode(b, eval_ct.parms_id(), eval_ct.scale(), plain_b);
            encoder.encode(c, eval_ct.parms_id(), eval_ct.scale(), plain_c);

            evaluator.add_plain_inplace(eval_ct, plain_a);
            evaluator.add_plain_inplace(eval_ct, plain_b);
            evaluator.multiply_plain_inplace(eval_ct, plain_c);

            Plaintext eval_plain;
            decryptor.decrypt(eval_ct, eval_plain);
            std::vector<double> eval_decoded(encoder.slot_count());
            encoder.decode(eval_plain, eval_decoded);

            double max_eval_err = 0.0;
            std::vector<double> expected_eval(expected_vals.size());
            for (std::size_t i = 0; i < expected_vals.size(); ++i)
            {
                expected_eval[i] = (expected_vals[i] + a + b) * c;
                max_eval_err = std::max(max_eval_err, std::fabs(eval_decoded[i] - expected_eval[i]));
            }

            std::cout << "compute_formula=((x+" << a << ")+" << b << ")*" << c << "\n";
            std::cout << "compute_decoded_first=";
            for (std::size_t i = 0; i < std::min(print_slots, eval_decoded.size()); ++i)
            {
                if (i)
                {
                    std::cout << ',';
                }
                std::cout << eval_decoded[i];
            }
            std::cout << "\n";
            std::cout << "compute_expected_first=";
            for (std::size_t i = 0; i < std::min(print_slots, expected_eval.size()); ++i)
            {
                if (i)
                {
                    std::cout << ',';
                }
                std::cout << expected_eval[i];
            }
            std::cout << "\n";
            std::cout << "compute_max_abs_error=" << std::setprecision(17) << max_eval_err << "\n";
            std::cout << "compute_max_abs_error_threshold=" << compute_threshold << "\n";
            std::cout << "compute_check=" << ((max_eval_err <= compute_threshold) ? "PASS" : "FAIL") << "\n";
            if (max_eval_err > compute_threshold)
            {
                return 4;
            }
        }

        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc < 2)
        {
            print_usage();
            return 1;
        }

        const std::string cmd = argv[1];
        const auto args = parse_kv_args(argc, argv, 2);

        if (cmd == "export-bundle")
        {
            return cmd_export_bundle(args);
        }
        if (cmd == "encrypt-from-bundle")
        {
            return cmd_encrypt_from_bundle(args);
        }
        if (cmd == "export-embedded-package")
        {
            return cmd_export_embedded_package(args);
        }
        if (cmd == "encrypt-mini")
        {
            return cmd_encrypt_mini(args);
        }
        if (cmd == "decrypt-check")
        {
            return cmd_decrypt_check(args);
        }

        throw std::runtime_error("unknown command: " + cmd);
    }
    catch (const std::exception &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        print_usage();
        return 2;
    }
}
