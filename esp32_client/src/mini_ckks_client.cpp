#include "mini_ckks_client.h"

#include "he_allocator.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace he_esp
{
    namespace
    {
        struct SealHeader
        {
            uint16_t magic{ 0xA15E };
            uint8_t header_size{ 0x10 };
            uint8_t version_major{ 0 };
            uint8_t version_minor{ 0 };
            uint8_t compr_mode{ 0 }; // none
            uint16_t reserved{ 0 };
            uint64_t size{ 0 };
        };

        static_assert(sizeof(SealHeader) == 16, "SEAL header must be 16 bytes");

        inline uint64_t mod_add(uint64_t a, uint64_t b, uint64_t mod)
        {
            uint64_t c = a + b;
            if (c >= mod)
            {
                c -= mod;
            }
            return c;
        }

        inline uint64_t mod_sub(uint64_t a, uint64_t b, uint64_t mod)
        {
            return (a >= b) ? (a - b) : (mod - (b - a));
        }

        inline uint64_t mod_mul(uint64_t a, uint64_t b, uint64_t mod)
        {
#if defined(__SIZEOF_INT128__)
            __uint128_t z = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
            return static_cast<uint64_t>(z % mod);
#else
            // Slow fallback without __int128.
            uint64_t result = 0;
            while (b)
            {
                if (b & 1)
                {
                    result = mod_add(result, a, mod);
                }
                a = mod_add(a, a, mod);
                b >>= 1;
            }
            return result;
#endif
        }

        uint64_t mod_pow(uint64_t base, uint64_t exp, uint64_t mod)
        {
            uint64_t result = 1;
            while (exp)
            {
                if (exp & 1)
                {
                    result = mod_mul(result, base, mod);
                }
                base = mod_mul(base, base, mod);
                exp >>= 1;
            }
            return result;
        }

        uint64_t mod_inv(uint64_t x, uint64_t mod)
        {
            // mod is prime in SEAL coeff modulus.
            return mod_pow(x, mod - 2, mod);
        }

        uint32_t reverse_bits(uint32_t x, int bit_count)
        {
            uint32_t y = 0;
            for (int i = 0; i < bit_count; ++i)
            {
                y = (y << 1u) | ((x >> i) & 1u);
            }
            return y;
        }

        bool find_minimal_2n_root(uint64_t mod, uint32_t n, uint64_t &root)
        {
            const uint64_t order = static_cast<uint64_t>(2) * n;
            if ((mod - 1) % order != 0)
            {
                return false;
            }
            const uint64_t exp = (mod - 1) / order;

            for (uint64_t cand = 2; cand < mod; ++cand)
            {
                uint64_t r = mod_pow(cand, exp, mod);
                if (mod_pow(r, order, mod) != 1)
                {
                    continue;
                }
                if (mod_pow(r, order / 2, mod) == 1)
                {
                    continue;
                }
                uint64_t min_root = r;
                const uint64_t generator_sq = mod_mul(r, r, mod);
                uint64_t current_generator = r;
                for (uint64_t i = 0; i < order; i += 2)
                {
                    if (current_generator < min_root)
                    {
                        min_root = current_generator;
                    }
                    current_generator = mod_mul(current_generator, generator_sq, mod);
                }
                root = min_root;
                return true;
            }
            return false;
        }

        // Port of SEAL's DWT transform_to_rev pattern for modular arithmetic.
        void ntt_to_rev(uint64_t *values, int log_n, const uint64_t *roots, uint64_t mod)
        {
            const size_t n = size_t(1) << log_n;
            size_t gap = n >> 1;
            size_t m = 1;
            size_t root_index = 0;

            for (; m < (n >> 1); m <<= 1)
            {
                size_t offset = 0;
                for (size_t i = 0; i < m; ++i)
                {
                    uint64_t r = roots[++root_index];
                    uint64_t *x = values + offset;
                    uint64_t *y = x + gap;
                    for (size_t j = 0; j < gap; ++j)
                    {
                        uint64_t u = *x;
                        uint64_t v = mod_mul(*y, r, mod);
                        *x++ = mod_add(u, v, mod);
                        *y++ = mod_sub(u, v, mod);
                    }
                    offset += gap << 1;
                }
                gap >>= 1;
            }

            for (size_t i = 0; i < m; ++i)
            {
                uint64_t r = roots[++root_index];
                uint64_t u = values[0];
                uint64_t v = mod_mul(values[1], r, mod);
                values[0] = mod_add(u, v, mod);
                values[1] = mod_sub(u, v, mod);
                values += 2;
            }
        }

        void intt_from_rev(uint64_t *values, int log_n, const uint64_t *inv_roots, uint64_t n_inv, uint64_t mod)
        {
            const size_t n = size_t(1) << log_n;
            size_t gap = 1;
            size_t m = n >> 1;
            size_t root_index = 0;

            for (; m > 1; m >>= 1)
            {
                size_t offset = 0;
                for (size_t i = 0; i < m; ++i)
                {
                    uint64_t r = inv_roots[++root_index];
                    uint64_t *x = values + offset;
                    uint64_t *y = x + gap;
                    for (size_t j = 0; j < gap; ++j)
                    {
                        uint64_t u = *x;
                        uint64_t v = *y;
                        *x++ = mod_add(u, v, mod);
                        *y++ = mod_mul(mod_sub(u, v, mod), r, mod);
                    }
                    offset += gap << 1;
                }
                gap <<= 1;
            }

            uint64_t r = inv_roots[++root_index];
            uint64_t scaled_r = mod_mul(r, n_inv, mod);
            uint64_t *x = values;
            uint64_t *y = x + gap;
            for (size_t j = 0; j < gap; ++j)
            {
                uint64_t u = *x;
                uint64_t v = *y;
                *x++ = mod_mul(mod_add(u, v, mod), n_inv, mod);
                *y++ = mod_mul(mod_sub(u, v, mod), scaled_r, mod);
            }
        }

        bool drop_last_prime_ntt(
            const MiniCkksContext &ctx, const uint64_t *input, uint64_t *output, uint64_t *temp_last,
            uint64_t *temp_mod)
        {
            const uint32_t n = ctx.poly_modulus_degree;
            const uint32_t key_k = ctx.coeff_modulus_count;
            if (key_k < 2)
            {
                std::memcpy(output, input, sizeof(uint64_t) * n);
                return true;
            }

            const uint32_t target_k = key_k - 1;
            const uint64_t last_mod = ctx.coeff_moduli[key_k - 1];
            const uint64_t half = last_mod >> 1;
            const int log_n = static_cast<int>(std::log2(static_cast<double>(n)));

            std::memcpy(temp_last, input + (static_cast<size_t>(key_k - 1) * n), sizeof(uint64_t) * n);
            intt_from_rev(
                temp_last, log_n, ctx.inv_root_powers + (static_cast<size_t>(key_k - 1) * n),
                ctx.n_inv[key_k - 1], last_mod);

            for (uint32_t i = 0; i < n; ++i)
            {
                temp_last[i] = mod_add(temp_last[i], half, last_mod);
            }

            for (uint32_t j = 0; j < target_k; ++j)
            {
                const uint64_t mod = ctx.coeff_moduli[j];
                const uint64_t neg_half_mod = mod - (half % mod);
                const uint64_t inv_last_mod_q = mod_inv(last_mod % mod, mod);
                const uint64_t *input_i = input + (static_cast<size_t>(j) * n);
                uint64_t *output_i = output + (static_cast<size_t>(j) * n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    temp_mod[i] = mod_add(temp_last[i] % mod, neg_half_mod, mod);
                }
                ntt_to_rev(temp_mod, log_n, ctx.root_powers + (static_cast<size_t>(j) * n), mod);
                for (uint32_t i = 0; i < n; ++i)
                {
                    output_i[i] = mod_mul(mod_sub(input_i[i], temp_mod[i], mod), inv_last_mod_q, mod);
                }
            }
            return true;
        }

        int hamming_weight_u8(uint8_t x)
        {
            int c = 0;
            while (x)
            {
                c += static_cast<int>(x & 1u);
                x >>= 1;
            }
            return c;
        }

        int32_t sample_cbd_noise(RandomFillFn rand_fill, void *rand_ctx)
        {
            uint8_t x[6] = { 0 };
            rand_fill(rand_ctx, x, sizeof(x));
            x[2] &= 0x1Fu;
            x[5] &= 0x1Fu;
            return static_cast<int32_t>(hamming_weight_u8(x[0]) + hamming_weight_u8(x[1]) + hamming_weight_u8(x[2]) -
                                        hamming_weight_u8(x[3]) - hamming_weight_u8(x[4]) - hamming_weight_u8(x[5]));
        }

        uint64_t sample_ternary_coeff(RandomFillFn rand_fill, void *rand_ctx, uint64_t mod)
        {
            uint8_t b = 0;
            rand_fill(rand_ctx, &b, 1);
            uint8_t r = static_cast<uint8_t>(b % 3u);
            if (r == 0)
            {
                return mod - 1; // -1 mod q
            }
            if (r == 1)
            {
                return 0;
            }
            return 1;
        }

        int8_t sample_ternary_signed(RandomFillFn rand_fill, void *rand_ctx)
        {
            uint8_t b = 0;
            rand_fill(rand_ctx, &b, 1);
            uint8_t r = static_cast<uint8_t>(b % 3u);
            if (r == 0)
            {
                return -1;
            }
            if (r == 1)
            {
                return 0;
            }
            return 1;
        }

        uint64_t signed_to_mod(int32_t value, uint64_t mod)
        {
            if (value >= 0)
            {
                return static_cast<uint64_t>(value) % mod;
            }
            uint64_t magnitude = static_cast<uint64_t>(-value) % mod;
            return magnitude ? (mod - magnitude) : 0;
        }

        bool encode_scalar_ntt(const MiniCkksContext &ctx, double value, uint32_t scale_bits, uint64_t *plain_ntt)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
            if (scale_bits >= 63)
            {
                return false;
            }
            double scale = std::ldexp(1.0, static_cast<int>(scale_bits));
            double scaled = std::round(value * scale);
            if (!std::isfinite(scaled))
            {
                return false;
            }

            const uint32_t n = ctx.poly_modulus_degree;
            for (uint32_t j = 0; j < ctx.ciphertext_coeff_modulus_count; ++j)
            {
                const uint64_t mod = ctx.coeff_moduli[j];
                uint64_t c = static_cast<uint64_t>(std::fabs(scaled));
                c %= mod;
                c = (std::signbit(scaled) && c) ? (mod - c) : c;
                uint64_t *plain_i = plain_ntt + (static_cast<size_t>(j) * n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    plain_i[i] = c;
                }
            }
            return true;
        }

        inline double complex_real(const double *values, size_t i)
        {
            return values[i * 2];
        }

        inline double complex_imag(const double *values, size_t i)
        {
            return values[i * 2 + 1];
        }

        inline void complex_set(double *values, size_t i, double real, double imag)
        {
            values[i * 2] = real;
            values[i * 2 + 1] = imag;
        }

        inline void complex_add(double ar, double ai, double br, double bi, double &rr, double &ri)
        {
            rr = ar + br;
            ri = ai + bi;
        }

        inline void complex_sub(double ar, double ai, double br, double bi, double &rr, double &ri)
        {
            rr = ar - br;
            ri = ai - bi;
        }

        inline void complex_mul(double ar, double ai, double br, double bi, double &rr, double &ri)
        {
            rr = (ar * br) - (ai * bi);
            ri = (ar * bi) + (ai * br);
        }

        void complex_transform_from_rev(double *values, int log_n, const double *roots, double scalar)
        {
            const size_t n = size_t(1) << log_n;
            size_t gap = 1;
            size_t m = n >> 1;
            size_t root_index = 0;

            for (; m > 1; m >>= 1)
            {
                size_t offset = 0;
                for (size_t i = 0; i < m; ++i)
                {
                    const double rr = complex_real(roots, ++root_index);
                    const double ri = complex_imag(roots, root_index);
                    double *x = values + (offset * 2);
                    double *y = x + (gap * 2);
                    for (size_t j = 0; j < gap; ++j)
                    {
                        const double xr = x[0];
                        const double xi = x[1];
                        const double yr = y[0];
                        const double yi = y[1];
                        double sr = 0, si = 0, dr = 0, di = 0, mr = 0, mi = 0;
                        complex_add(xr, xi, yr, yi, sr, si);
                        complex_sub(xr, xi, yr, yi, dr, di);
                        complex_mul(dr, di, rr, ri, mr, mi);
                        x[0] = sr;
                        x[1] = si;
                        y[0] = mr;
                        y[1] = mi;
                        x += 2;
                        y += 2;
                    }
                    offset += gap << 1;
                }
                gap <<= 1;
            }

            const double rr = complex_real(roots, ++root_index) * scalar;
            const double ri = complex_imag(roots, root_index) * scalar;
            double *x = values;
            double *y = x + (gap * 2);
            for (size_t j = 0; j < gap; ++j)
            {
                const double xr = x[0];
                const double xi = x[1];
                const double yr = y[0];
                const double yi = y[1];
                double sr = 0, si = 0, dr = 0, di = 0, mr = 0, mi = 0;
                complex_add(xr, xi, yr, yi, sr, si);
                complex_sub(xr, xi, yr, yi, dr, di);
                complex_mul(dr, di, rr, ri, mr, mi);
                x[0] = sr * scalar;
                x[1] = si * scalar;
                y[0] = mr;
                y[1] = mi;
                x += 2;
                y += 2;
            }
        }

        bool encode_vector_ntt(
            const MiniCkksContext &ctx, const double *values, size_t values_size, uint32_t scale_bits,
            uint64_t *plain_ntt)
        {
            if ((!values && values_size > 0) || values_size > (ctx.poly_modulus_degree / 2) || scale_bits >= 63)
            {
                return false;
            }

            const uint32_t n = ctx.poly_modulus_degree;
            const uint32_t slots = n / 2;
            const int log_n = static_cast<int>(std::log2(static_cast<double>(n)));
            double *conj_values = static_cast<double *>(alloc_he_buffer(sizeof(double) * 2 * n));
            if (!conj_values)
            {
                return false;
            }
            std::memset(conj_values, 0, sizeof(double) * 2 * n);

            for (size_t i = 0; i < values_size; ++i)
            {
                if (!std::isfinite(values[i]))
                {
                    free_he_buffer(conj_values);
                    return false;
                }
                const uint32_t idx1 = ctx.matrix_reps_index_map[i];
                const uint32_t idx2 = ctx.matrix_reps_index_map[slots + i];
                complex_set(conj_values, idx1, values[i], 0.0);
                complex_set(conj_values, idx2, values[i], -0.0);
            }

            const double scale = std::ldexp(1.0, static_cast<int>(scale_bits));
            const double fix = scale / static_cast<double>(n);
            complex_transform_from_rev(conj_values, log_n, ctx.complex_inv_root_powers, fix);

            for (uint32_t j = 0; j < ctx.ciphertext_coeff_modulus_count; ++j)
            {
                const uint64_t mod = ctx.coeff_moduli[j];
                uint64_t *plain_i = plain_ntt + (static_cast<size_t>(j) * n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    const double coeffd = std::round(complex_real(conj_values, i));
                    if (!std::isfinite(coeffd))
                    {
                        free_he_buffer(conj_values);
                        return false;
                    }
                    const bool is_negative = std::signbit(coeffd);
                    uint64_t coeffu = static_cast<uint64_t>(std::fabs(coeffd));
                    coeffu %= mod;
                    plain_i[i] = (is_negative && coeffu) ? (mod - coeffu) : coeffu;
                }
                ntt_to_rev(plain_i, log_n, ctx.root_powers + (static_cast<size_t>(j) * n), mod);
            }

            free_he_buffer(conj_values);
            return true;
        }

        bool encrypt_plain_ntt(
            const MiniCkksContext &ctx, uint64_t *plain, RandomFillFn rand_fill, void *rand_ctx, uint64_t *out_c0,
            uint64_t *out_c1, const char **error)
        {
            const uint32_t n = ctx.poly_modulus_degree;
            const uint32_t key_k = ctx.coeff_modulus_count;
            const uint32_t target_k = ctx.ciphertext_coeff_modulus_count;
            const size_t poly_bytes = sizeof(uint64_t) * n;
            uint64_t *full_c0 = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n * key_k));
            uint64_t *full_c1 = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n * key_k));
            uint64_t *drop_last = nullptr;
            uint64_t *drop_temp = nullptr;
            int8_t *u_small = static_cast<int8_t *>(alloc_he_buffer(sizeof(int8_t) * n));
            int8_t *e0_small = static_cast<int8_t *>(alloc_he_buffer(sizeof(int8_t) * n));
            int8_t *e1_small = static_cast<int8_t *>(alloc_he_buffer(sizeof(int8_t) * n));
            uint64_t *u = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
            uint64_t *e0 = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
            uint64_t *e1 = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
            if (key_k > target_k)
            {
                drop_last = static_cast<uint64_t *>(alloc_he_buffer(poly_bytes));
                drop_temp = static_cast<uint64_t *>(alloc_he_buffer(poly_bytes));
            }
            if (!full_c0 || !full_c1 || !u_small || !e0_small || !e1_small || !u || !e0 || !e1 ||
                (key_k > target_k && (!drop_last || !drop_temp)))
            {
                free_he_buffer(full_c0);
                free_he_buffer(full_c1);
                free_he_buffer(drop_last);
                free_he_buffer(drop_temp);
                free_he_buffer(u_small);
                free_he_buffer(e0_small);
                free_he_buffer(e1_small);
                free_he_buffer(u);
                free_he_buffer(e0);
                free_he_buffer(e1);
                if (error)
                {
                    *error = "failed allocating encryption scratch";
                }
                return false;
            }

            for (uint32_t i = 0; i < n; ++i)
            {
                u_small[i] = sample_ternary_signed(rand_fill, rand_ctx);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                int32_t n0 = sample_cbd_noise(rand_fill, rand_ctx);
                int32_t n1 = sample_cbd_noise(rand_fill, rand_ctx);
                e0_small[i] = static_cast<int8_t>(n0);
                e1_small[i] = static_cast<int8_t>(n1);
            }

            const int log_n = static_cast<int>(std::log2(static_cast<double>(n)));
            for (uint32_t j = 0; j < key_k; ++j)
            {
                const uint64_t mod = ctx.coeff_moduli[j];
                const size_t poly_offset = static_cast<size_t>(j) * n;
                for (uint32_t i = 0; i < n; ++i)
                {
                    u[i] = signed_to_mod(u_small[i], mod);
                    e0[i] = signed_to_mod(e0_small[i], mod);
                    e1[i] = signed_to_mod(e1_small[i], mod);
                }

                const uint64_t *roots = ctx.root_powers + poly_offset;
                ntt_to_rev(u, log_n, roots, mod);
                ntt_to_rev(e0, log_n, roots, mod);
                ntt_to_rev(e1, log_n, roots, mod);

                uint64_t *out0_i = full_c0 + poly_offset;
                uint64_t *out1_i = full_c1 + poly_offset;
                const uint64_t *pk0_i = ctx.pk0 + poly_offset;
                const uint64_t *pk1_i = ctx.pk1 + poly_offset;
                for (uint32_t i = 0; i < n; ++i)
                {
                    out0_i[i] = mod_mul(pk0_i[i], u[i], mod);
                    out1_i[i] = mod_mul(pk1_i[i], u[i], mod);
                    out0_i[i] = mod_add(out0_i[i], e0[i], mod);
                    out1_i[i] = mod_add(out1_i[i], e1[i], mod);
                }
            }

            if (key_k == target_k)
            {
                std::memcpy(out_c0, full_c0, sizeof(uint64_t) * n * target_k);
                std::memcpy(out_c1, full_c1, sizeof(uint64_t) * n * target_k);
            }
            else if (key_k == target_k + 1)
            {
                drop_last_prime_ntt(ctx, full_c0, out_c0, drop_last, drop_temp);
                drop_last_prime_ntt(ctx, full_c1, out_c1, drop_last, drop_temp);
            }
            else
            {
                if (error)
                {
                    *error = "dropping more than one key prime is not implemented";
                }
                free_he_buffer(full_c0);
                free_he_buffer(full_c1);
                free_he_buffer(drop_last);
                free_he_buffer(drop_temp);
                free_he_buffer(u_small);
                free_he_buffer(e0_small);
                free_he_buffer(e1_small);
                free_he_buffer(u);
                free_he_buffer(e0);
                free_he_buffer(e1);
                return false;
            }

            for (uint32_t j = 0; j < target_k; ++j)
            {
                const uint64_t mod = ctx.coeff_moduli[j];
                const size_t poly_offset = static_cast<size_t>(j) * n;
                uint64_t *out0_i = out_c0 + poly_offset;
                const uint64_t *plain_i = plain + poly_offset;
                for (uint32_t i = 0; i < n; ++i)
                {
                    out0_i[i] = mod_add(out0_i[i], plain_i[i], mod);
                }
            }

            std::memset(u, 0, poly_bytes);
            std::memset(e0, 0, poly_bytes);
            std::memset(e1, 0, poly_bytes);
            free_he_buffer(full_c0);
            free_he_buffer(full_c1);
            free_he_buffer(drop_last);
            free_he_buffer(drop_temp);
            free_he_buffer(u_small);
            free_he_buffer(e0_small);
            free_he_buffer(e1_small);
            free_he_buffer(u);
            free_he_buffer(e0);
            free_he_buffer(e1);
            return true;
        }
    } // namespace

    bool mini_ckks_init_from_package(const EmbeddedPackageView &pkg, MiniCkksContext &ctx, const char **error)
    {
        if (error)
        {
            *error = nullptr;
        }
        if (pkg.header.pk_poly_size != pkg.header.poly_modulus_degree)
        {
            if (error)
            {
                *error = "invalid package polynomial sizing";
            }
            return false;
        }

        ctx = {};
        ctx.seal_major = pkg.header.seal_major;
        ctx.seal_minor = pkg.header.seal_minor;
        ctx.poly_modulus_degree = pkg.header.poly_modulus_degree;
        ctx.coeff_modulus_count = pkg.header.coeff_modulus_count;
        ctx.ciphertext_coeff_modulus_count = (ctx.coeff_modulus_count > 1) ? (ctx.coeff_modulus_count - 1) : 1;
        ctx.coeff_modulus = pkg.header.coeff_modulus;
        ctx.coeff_moduli = pkg.coeff_moduli ? pkg.coeff_moduli : &ctx.coeff_modulus;
        ctx.scale_bits = pkg.header.scale_bits;
        std::memcpy(ctx.parms_id, pkg.header.parms_id, sizeof(ctx.parms_id));
        ctx.pk0 = pkg.pk0;
        ctx.pk1 = pkg.pk1;

        const uint32_t n = ctx.poly_modulus_degree;
        const int log_n = static_cast<int>(std::log2(static_cast<double>(n)));
        if ((1u << log_n) != n)
        {
            if (error)
            {
                *error = "poly modulus degree must be power of two";
            }
            return false;
        }

        const size_t rns_poly_count = static_cast<size_t>(ctx.coeff_modulus_count) * n;
        ctx.root_powers = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * rns_poly_count));
        ctx.inv_root_powers = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * rns_poly_count));
        ctx.n_inv = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * ctx.coeff_modulus_count));
        ctx.matrix_reps_index_map = static_cast<uint32_t *>(alloc_he_buffer(sizeof(uint32_t) * n));
        ctx.complex_root_powers = static_cast<double *>(alloc_he_buffer(sizeof(double) * 2 * n));
        ctx.complex_inv_root_powers = static_cast<double *>(alloc_he_buffer(sizeof(double) * 2 * n));
        if (!ctx.root_powers || !ctx.inv_root_powers || !ctx.n_inv || !ctx.matrix_reps_index_map ||
            !ctx.complex_root_powers || !ctx.complex_inv_root_powers)
        {
            mini_ckks_release(ctx);
            if (error)
            {
                *error = "failed allocating encoder/NTT tables";
            }
            return false;
        }

        for (uint32_t j = 0; j < ctx.coeff_modulus_count; ++j)
        {
            const uint64_t mod = ctx.coeff_moduli[j];
            uint64_t root = 0;
            if (!find_minimal_2n_root(mod, n, root))
            {
                mini_ckks_release(ctx);
                if (error)
                {
                    *error = "could not find primitive 2N-th root";
                }
                return false;
            }
            uint64_t inv_root = mod_inv(root, mod);
            uint64_t *root_powers_i = ctx.root_powers + (static_cast<size_t>(j) * n);
            uint64_t *inv_root_powers_i = ctx.inv_root_powers + (static_cast<size_t>(j) * n);

            root_powers_i[0] = 1;
            inv_root_powers_i[0] = 1;

            uint64_t power = root;
            for (uint32_t i = 1; i < n; ++i)
            {
                uint32_t idx = reverse_bits(i, log_n);
                root_powers_i[idx] = power;
                power = mod_mul(power, root, mod);
            }

            power = inv_root;
            for (uint32_t i = 1; i < n; ++i)
            {
                uint32_t idx = reverse_bits(i - 1, log_n) + 1;
                inv_root_powers_i[idx] = power;
                power = mod_mul(power, inv_root, mod);
            }

            ctx.n_inv[j] = mod_inv(n, mod);
        }

        const uint64_t gen = 3;
        uint64_t pos = 1;
        const uint64_t m = static_cast<uint64_t>(n) << 1;
        const uint32_t slots = n >> 1;
        for (uint32_t i = 0; i < slots; ++i)
        {
            const uint64_t index1 = (pos - 1) >> 1;
            const uint64_t index2 = (m - pos - 1) >> 1;
            ctx.matrix_reps_index_map[i] = reverse_bits(static_cast<uint32_t>(index1), log_n);
            ctx.matrix_reps_index_map[slots + i] = reverse_bits(static_cast<uint32_t>(index2), log_n);
            pos *= gen;
            pos &= (m - 1);
        }

        std::memset(ctx.complex_root_powers, 0, sizeof(double) * 2 * n);
        std::memset(ctx.complex_inv_root_powers, 0, sizeof(double) * 2 * n);
        const double two_pi = 6.283185307179586476925286766559;
        for (uint32_t i = 1; i < n; ++i)
        {
            const uint32_t root_idx = reverse_bits(i, log_n);
            const double root_angle = two_pi * static_cast<double>(root_idx) / static_cast<double>(m);
            complex_set(ctx.complex_root_powers, i, std::cos(root_angle), std::sin(root_angle));

            const uint32_t inv_idx = reverse_bits(i - 1, log_n) + 1;
            const double inv_angle = -two_pi * static_cast<double>(inv_idx) / static_cast<double>(m);
            complex_set(ctx.complex_inv_root_powers, i, std::cos(inv_angle), std::sin(inv_angle));
        }

        return true;
    }

    void mini_ckks_release(MiniCkksContext &ctx)
    {
        if (ctx.root_powers)
        {
            free_he_buffer(ctx.root_powers);
        }
        if (ctx.inv_root_powers)
        {
            free_he_buffer(ctx.inv_root_powers);
        }
        if (ctx.n_inv)
        {
            free_he_buffer(ctx.n_inv);
        }
        if (ctx.matrix_reps_index_map)
        {
            free_he_buffer(ctx.matrix_reps_index_map);
        }
        if (ctx.complex_root_powers)
        {
            free_he_buffer(ctx.complex_root_powers);
        }
        if (ctx.complex_inv_root_powers)
        {
            free_he_buffer(ctx.complex_inv_root_powers);
        }
        ctx.root_powers = nullptr;
        ctx.inv_root_powers = nullptr;
        ctx.n_inv = nullptr;
        ctx.matrix_reps_index_map = nullptr;
        ctx.complex_root_powers = nullptr;
        ctx.complex_inv_root_powers = nullptr;
        ctx.coeff_moduli = nullptr;
        ctx.pk0 = nullptr;
        ctx.pk1 = nullptr;
    }

    bool mini_ckks_encrypt_scalar(
        const MiniCkksContext &ctx, double value, uint32_t scale_bits, RandomFillFn rand_fill, void *rand_ctx,
        uint64_t *out_c0, uint64_t *out_c1, const char **error)
    {
        if (error)
        {
            *error = nullptr;
        }
        if (!rand_fill)
        {
            if (error)
            {
                *error = "random callback is null";
            }
            return false;
        }
        if (!out_c0 || !out_c1)
        {
            if (error)
            {
                *error = "output buffers are null";
            }
            return false;
        }

        uint64_t *plain = static_cast<uint64_t *>(
            alloc_he_buffer(sizeof(uint64_t) * ctx.poly_modulus_degree * ctx.ciphertext_coeff_modulus_count));
        if (!plain)
        {
            if (error)
            {
                *error = "failed allocating plaintext buffer";
            }
            return false;
        }

        bool ok = encode_scalar_ntt(ctx, value, scale_bits, plain);
        if (!ok)
        {
            free_he_buffer(plain);
            if (error)
            {
                *error = "scalar encode failed";
            }
            return false;
        }

        ok = encrypt_plain_ntt(ctx, plain, rand_fill, rand_ctx, out_c0, out_c1, error);
        free_he_buffer(plain);
        return ok;
    }

    bool mini_ckks_encrypt_vector(
        const MiniCkksContext &ctx, const double *values, size_t values_size, uint32_t scale_bits,
        RandomFillFn rand_fill, void *rand_ctx, uint64_t *out_c0, uint64_t *out_c1, const char **error)
    {
        if (error)
        {
            *error = nullptr;
        }
        if (!rand_fill)
        {
            if (error)
            {
                *error = "random callback is null";
            }
            return false;
        }
        if (!out_c0 || !out_c1)
        {
            if (error)
            {
                *error = "output buffers are null";
            }
            return false;
        }

        uint64_t *plain = static_cast<uint64_t *>(
            alloc_he_buffer(sizeof(uint64_t) * ctx.poly_modulus_degree * ctx.ciphertext_coeff_modulus_count));
        if (!plain)
        {
            if (error)
            {
                *error = "failed allocating plaintext buffer";
            }
            return false;
        }

        bool ok = encode_vector_ntt(ctx, values, values_size, scale_bits, plain);
        if (!ok)
        {
            free_he_buffer(plain);
            if (error)
            {
                *error = "vector encode failed";
            }
            return false;
        }

        ok = encrypt_plain_ntt(ctx, plain, rand_fill, rand_ctx, out_c0, out_c1, error);
        free_he_buffer(plain);
        return ok;
    }

    size_t mini_ckks_ciphertext_serialized_size(const MiniCkksContext &ctx)
    {
        const uint64_t data_uint64_count =
            static_cast<uint64_t>(ctx.poly_modulus_degree) * ctx.ciphertext_coeff_modulus_count * 2;
        return static_cast<size_t>(113 + data_uint64_count * sizeof(uint64_t));
    }

    size_t mini_ckks_serialize_ciphertext(
        const MiniCkksContext &ctx, double scale, const uint64_t *c0, const uint64_t *c1, uint8_t *out,
        size_t out_size)
    {
        if (!out || !c0 || !c1)
        {
            return 0;
        }
        const size_t need = mini_ckks_ciphertext_serialized_size(ctx);
        if (out_size < need)
        {
            return 0;
        }

        const uint64_t data_uint64_count =
            static_cast<uint64_t>(ctx.poly_modulus_degree) * ctx.ciphertext_coeff_modulus_count * 2;
        const uint64_t dynarray_size = 16 + 8 + data_uint64_count * sizeof(uint64_t);

        SealHeader outer{};
        outer.version_major = static_cast<uint8_t>(ctx.seal_major);
        outer.version_minor = static_cast<uint8_t>(ctx.seal_minor);
        outer.size = static_cast<uint64_t>(need);

        SealHeader inner{};
        inner.version_major = static_cast<uint8_t>(ctx.seal_major);
        inner.version_minor = static_cast<uint8_t>(ctx.seal_minor);
        inner.size = dynarray_size;

        size_t off = 0;
        std::memcpy(out + off, &outer, sizeof(outer));
        off += sizeof(outer);

        std::memcpy(out + off, ctx.parms_id, sizeof(ctx.parms_id));
        off += sizeof(ctx.parms_id);

        uint8_t is_ntt_form = 1;
        std::memcpy(out + off, &is_ntt_form, sizeof(is_ntt_form));
        off += sizeof(is_ntt_form);

        uint64_t size_poly = 2;
        uint64_t poly_modulus_degree = ctx.poly_modulus_degree;
        uint64_t coeff_modulus_size = ctx.ciphertext_coeff_modulus_count;
        uint64_t correction_factor = 1;

        std::memcpy(out + off, &size_poly, sizeof(size_poly));
        off += sizeof(size_poly);
        std::memcpy(out + off, &poly_modulus_degree, sizeof(poly_modulus_degree));
        off += sizeof(poly_modulus_degree);
        std::memcpy(out + off, &coeff_modulus_size, sizeof(coeff_modulus_size));
        off += sizeof(coeff_modulus_size);
        std::memcpy(out + off, &scale, sizeof(scale));
        off += sizeof(scale);
        std::memcpy(out + off, &correction_factor, sizeof(correction_factor));
        off += sizeof(correction_factor);

        std::memcpy(out + off, &inner, sizeof(inner));
        off += sizeof(inner);

        std::memcpy(out + off, &data_uint64_count, sizeof(data_uint64_count));
        off += sizeof(data_uint64_count);

        const size_t poly_bytes =
            static_cast<size_t>(ctx.poly_modulus_degree) * ctx.ciphertext_coeff_modulus_count * sizeof(uint64_t);
        std::memcpy(out + off, c0, poly_bytes);
        off += poly_bytes;
        std::memcpy(out + off, c1, poly_bytes);
        off += poly_bytes;

        return off;
    }
} // namespace he_esp
