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

            if (scaled >= 0)
            {
                uint64_t c = static_cast<uint64_t>(scaled);
                c %= ctx.coeff_modulus;
                for (uint32_t i = 0; i < ctx.poly_modulus_degree; ++i)
                {
                    plain_ntt[i] = c;
                }
            }
            else
            {
                uint64_t c = static_cast<uint64_t>(-scaled);
                c %= ctx.coeff_modulus;
                c = (c == 0) ? 0 : (ctx.coeff_modulus - c);
                for (uint32_t i = 0; i < ctx.poly_modulus_degree; ++i)
                {
                    plain_ntt[i] = c;
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
                coeffu %= ctx.coeff_modulus;
                plain_ntt[i] = (is_negative && coeffu) ? (ctx.coeff_modulus - coeffu) : coeffu;
            }

            free_he_buffer(conj_values);
            ntt_to_rev(plain_ntt, log_n, ctx.root_powers, ctx.coeff_modulus);
            return true;
        }

        bool encrypt_plain_ntt(
            const MiniCkksContext &ctx, uint64_t *plain, RandomFillFn rand_fill, void *rand_ctx, uint64_t *out_c0,
            uint64_t *out_c1, const char **error)
        {
            const uint32_t n = ctx.poly_modulus_degree;
            uint64_t *u = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
            uint64_t *e0 = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
            uint64_t *e1 = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
            if (!u || !e0 || !e1)
            {
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
                u[i] = sample_ternary_coeff(rand_fill, rand_ctx, ctx.coeff_modulus);
            }
            ntt_to_rev(u, static_cast<int>(std::log2(static_cast<double>(n))), ctx.root_powers, ctx.coeff_modulus);

            for (uint32_t i = 0; i < n; ++i)
            {
                out_c0[i] = mod_mul(ctx.pk0[i], u[i], ctx.coeff_modulus);
                out_c1[i] = mod_mul(ctx.pk1[i], u[i], ctx.coeff_modulus);
            }

            for (uint32_t i = 0; i < n; ++i)
            {
                int32_t n0 = sample_cbd_noise(rand_fill, rand_ctx);
                int32_t n1 = sample_cbd_noise(rand_fill, rand_ctx);
                e0[i] = (n0 >= 0) ? static_cast<uint64_t>(n0) : (ctx.coeff_modulus - static_cast<uint64_t>(-n0));
                e1[i] = (n1 >= 0) ? static_cast<uint64_t>(n1) : (ctx.coeff_modulus - static_cast<uint64_t>(-n1));
            }

            ntt_to_rev(e0, static_cast<int>(std::log2(static_cast<double>(n))), ctx.root_powers, ctx.coeff_modulus);
            ntt_to_rev(e1, static_cast<int>(std::log2(static_cast<double>(n))), ctx.root_powers, ctx.coeff_modulus);

            for (uint32_t i = 0; i < n; ++i)
            {
                out_c0[i] = mod_add(out_c0[i], e0[i], ctx.coeff_modulus);
                out_c1[i] = mod_add(out_c1[i], e1[i], ctx.coeff_modulus);
                out_c0[i] = mod_add(out_c0[i], plain[i], ctx.coeff_modulus);
            }

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
        ctx.coeff_modulus = pkg.header.coeff_modulus;
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

        ctx.root_powers = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
        ctx.inv_root_powers = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * n));
        ctx.matrix_reps_index_map = static_cast<uint32_t *>(alloc_he_buffer(sizeof(uint32_t) * n));
        ctx.complex_root_powers = static_cast<double *>(alloc_he_buffer(sizeof(double) * 2 * n));
        ctx.complex_inv_root_powers = static_cast<double *>(alloc_he_buffer(sizeof(double) * 2 * n));
        if (!ctx.root_powers || !ctx.inv_root_powers || !ctx.matrix_reps_index_map || !ctx.complex_root_powers ||
            !ctx.complex_inv_root_powers)
        {
            mini_ckks_release(ctx);
            if (error)
            {
                *error = "failed allocating encoder/NTT tables";
            }
            return false;
        }

        uint64_t root = 0;
        if (!find_minimal_2n_root(ctx.coeff_modulus, n, root))
        {
            mini_ckks_release(ctx);
            if (error)
            {
                *error = "could not find primitive 2N-th root";
            }
            return false;
        }
        uint64_t inv_root = mod_inv(root, ctx.coeff_modulus);

        ctx.root_powers[0] = 1;
        ctx.inv_root_powers[0] = 1;

        uint64_t power = root;
        for (uint32_t i = 1; i < n; ++i)
        {
            uint32_t idx = reverse_bits(i, log_n);
            ctx.root_powers[idx] = power;
            power = mod_mul(power, root, ctx.coeff_modulus);
        }

        power = inv_root;
        for (uint32_t i = 1; i < n; ++i)
        {
            uint32_t idx = reverse_bits(i - 1, log_n) + 1;
            ctx.inv_root_powers[idx] = power;
            power = mod_mul(power, inv_root, ctx.coeff_modulus);
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

        ctx.n_inv = mod_inv(n, ctx.coeff_modulus);
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
        ctx.matrix_reps_index_map = nullptr;
        ctx.complex_root_powers = nullptr;
        ctx.complex_inv_root_powers = nullptr;
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

        uint64_t *plain = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * ctx.poly_modulus_degree));
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

        uint64_t *plain = static_cast<uint64_t *>(alloc_he_buffer(sizeof(uint64_t) * ctx.poly_modulus_degree));
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
        const uint64_t data_uint64_count = static_cast<uint64_t>(ctx.poly_modulus_degree) * 2;
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

        const uint64_t data_uint64_count = static_cast<uint64_t>(ctx.poly_modulus_degree) * 2;
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
        uint64_t coeff_modulus_size = 1;
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

        const size_t poly_bytes = static_cast<size_t>(ctx.poly_modulus_degree) * sizeof(uint64_t);
        std::memcpy(out + off, c0, poly_bytes);
        off += poly_bytes;
        std::memcpy(out + off, c1, poly_bytes);
        off += poly_bytes;

        return off;
    }
} // namespace he_esp
