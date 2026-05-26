#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "embedded_package_blob.h"
#include "he_allocator.h"
#include "he_embedded_package.h"
#include "mini_ckks_client.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#include <esp_system.h>
#endif

namespace
{
    constexpr uint32_t kBaud = 115200;
    constexpr size_t kHexBytesPerLine = 32;
    constexpr uint32_t kMaxBenchRuns = 100;

    he_esp::MiniCkksContext g_ctx{};
    bool g_ready = false;

    struct MemorySnapshot
    {
        size_t heap_free{ 0 };
        size_t heap_min{ 0 };
        size_t heap_largest{ 0 };
        size_t psram_found{ 0 };
        size_t psram_free{ 0 };
        size_t psram_total{ 0 };
        size_t psram_min{ 0 };
        size_t psram_largest{ 0 };
    };

    struct EncryptMetrics
    {
        bool ok{ false };
        const char *error{ nullptr };
        size_t ciphertext_bytes{ 0 };
        uint32_t encrypt_ms{ 0 };
        uint32_t serialize_ms{ 0 };
        uint32_t total_ms{ 0 };
        size_t tracked_start{ 0 };
        size_t tracked_peak{ 0 };
        size_t tracked_psram_start{ 0 };
        size_t tracked_psram_peak{ 0 };
        size_t tracked_internal_start{ 0 };
        size_t tracked_internal_peak{ 0 };
        size_t heap_free_start{ 0 };
        size_t heap_free_min_seen{ 0 };
        size_t heap_free_end{ 0 };
        size_t psram_free_start{ 0 };
        size_t psram_free_min_seen{ 0 };
        size_t psram_free_end{ 0 };
        size_t largest_allocation{ 0 };
        size_t total_allocations{ 0 };
        size_t failed_allocations{ 0 };
    };

    size_t min_size(size_t a, size_t b)
    {
        return (a < b) ? a : b;
    }

    void print_size_kv(const char *key, size_t value)
    {
        Serial.print(key);
        Serial.print("=");
        Serial.println(static_cast<unsigned long>(value));
    }

    void print_u64_kv(const char *key, uint64_t value)
    {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
        Serial.print(key);
        Serial.print("=");
        Serial.println(buf);
    }

    void fill_random(void *, uint8_t *dst, size_t size)
    {
#if defined(ARDUINO_ARCH_ESP32)
        size_t off = 0;
        while (off < size)
        {
            uint32_t r = esp_random();
            for (int i = 0; i < 4 && off < size; ++i)
            {
                dst[off++] = static_cast<uint8_t>((r >> (8 * i)) & 0xFFu);
            }
        }
#else
        for (size_t i = 0; i < size; ++i)
        {
            dst[i] = static_cast<uint8_t>(i * 131u + 17u);
        }
#endif
    }

    MemorySnapshot read_memory_snapshot()
    {
        MemorySnapshot snap{};
#if defined(ARDUINO_ARCH_ESP32)
        snap.heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snap.heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snap.heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        snap.psram_found = psramFound() ? 1 : 0;
        snap.psram_free = ESP.getFreePsram();
        snap.psram_total = ESP.getPsramSize();
        snap.psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        snap.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#endif
        return snap;
    }

    void print_memory(const char *prefix)
    {
        const MemorySnapshot snap = read_memory_snapshot();
        char key[48];
        std::snprintf(key, sizeof(key), "%s_heap_free", prefix);
        print_size_kv(key, snap.heap_free);
        std::snprintf(key, sizeof(key), "%s_heap_min", prefix);
        print_size_kv(key, snap.heap_min);
        std::snprintf(key, sizeof(key), "%s_heap_largest", prefix);
        print_size_kv(key, snap.heap_largest);
        std::snprintf(key, sizeof(key), "%s_psram_found", prefix);
        print_size_kv(key, snap.psram_found);
        std::snprintf(key, sizeof(key), "%s_psram_free", prefix);
        print_size_kv(key, snap.psram_free);
        std::snprintf(key, sizeof(key), "%s_psram_total", prefix);
        print_size_kv(key, snap.psram_total);
        std::snprintf(key, sizeof(key), "%s_psram_min", prefix);
        print_size_kv(key, snap.psram_min);
        std::snprintf(key, sizeof(key), "%s_psram_largest", prefix);
        print_size_kv(key, snap.psram_largest);

        const he_esp::HeAllocationStats stats = he_esp::get_he_allocation_stats();
        std::snprintf(key, sizeof(key), "%s_tracked_current", prefix);
        print_size_kv(key, stats.current_bytes);
        std::snprintf(key, sizeof(key), "%s_tracked_peak", prefix);
        print_size_kv(key, stats.peak_bytes);
        std::snprintf(key, sizeof(key), "%s_tracked_psram_current", prefix);
        print_size_kv(key, stats.current_psram_bytes);
        std::snprintf(key, sizeof(key), "%s_tracked_psram_peak", prefix);
        print_size_kv(key, stats.peak_psram_bytes);
        std::snprintf(key, sizeof(key), "%s_tracked_internal_current", prefix);
        print_size_kv(key, stats.current_internal_bytes);
        std::snprintf(key, sizeof(key), "%s_tracked_internal_peak", prefix);
        print_size_kv(key, stats.peak_internal_bytes);
    }

    void update_min_seen(size_t &heap_min_seen, size_t &psram_min_seen)
    {
        const MemorySnapshot snap = read_memory_snapshot();
        heap_min_seen = min_size(heap_min_seen, snap.heap_free);
        if (snap.psram_found)
        {
            psram_min_seen = min_size(psram_min_seen, snap.psram_free);
        }
    }

    void print_parameters()
    {
        print_size_kv("PACKAGE_BYTES", kEmbeddedPackageBlobSize);
        print_size_kv("N", g_ctx.poly_modulus_degree);
        print_size_kv("SCALE_BITS", g_ctx.scale_bits);
        print_u64_kv("COEFF_MODULUS", g_ctx.coeff_modulus);
        print_size_kv("SEAL_MAJOR", g_ctx.seal_major);
        print_size_kv("SEAL_MINOR", g_ctx.seal_minor);
        print_size_kv("C0_BYTES", static_cast<size_t>(g_ctx.poly_modulus_degree) * sizeof(uint64_t));
        print_size_kv("C1_BYTES", static_cast<size_t>(g_ctx.poly_modulus_degree) * sizeof(uint64_t));
        const size_t poly_u64_bytes = static_cast<size_t>(g_ctx.poly_modulus_degree) * sizeof(uint64_t);
        const size_t complex_array_bytes = static_cast<size_t>(g_ctx.poly_modulus_degree) * sizeof(double) * 2;
        const size_t persistent_table_bytes = (poly_u64_bytes * 2) +
                                              (static_cast<size_t>(g_ctx.poly_modulus_degree) * sizeof(uint32_t)) +
                                              (complex_array_bytes * 2);
        const size_t encrypt_temp_peak_estimate =
            (poly_u64_bytes * 6) + he_esp::mini_ckks_ciphertext_serialized_size(g_ctx);
        print_size_kv("VECTOR_ENCODE_COMPLEX_BYTES", complex_array_bytes);
        print_size_kv("PERSISTENT_TABLE_BYTES", persistent_table_bytes);
        print_size_kv("ENCRYPT_TEMP_PEAK_ESTIMATE_BYTES", encrypt_temp_peak_estimate);
        print_size_kv("CIPHERTEXT_SERIALIZED_BYTES", he_esp::mini_ckks_ciphertext_serialized_size(g_ctx));
    }

    void print_hex_payload(const uint8_t *data, size_t size)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        char line[(kHexBytesPerLine * 2) + 1];
        size_t offset = 0;
        while (offset < size)
        {
            const size_t chunk = min_size(kHexBytesPerLine, size - offset);
            for (size_t i = 0; i < chunk; ++i)
            {
                const uint8_t b = data[offset + i];
                line[i * 2] = kHex[b >> 4];
                line[i * 2 + 1] = kHex[b & 0x0F];
            }
            line[chunk * 2] = '\0';
            Serial.println(line);
            offset += chunk;
        }
    }

    bool run_encrypt(const double *values, size_t values_size, bool emit_ciphertext, EncryptMetrics &metrics)
    {
        metrics = {};
        if (!g_ready)
        {
            metrics.error = "not_ready";
            return false;
        }

        he_esp::reset_he_allocation_peaks();
        const MemorySnapshot start_snap = read_memory_snapshot();
        const he_esp::HeAllocationStats start_stats = he_esp::get_he_allocation_stats();
        metrics.tracked_start = start_stats.current_bytes;
        metrics.tracked_psram_start = start_stats.current_psram_bytes;
        metrics.tracked_internal_start = start_stats.current_internal_bytes;
        metrics.heap_free_start = start_snap.heap_free;
        metrics.heap_free_min_seen = start_snap.heap_free;
        metrics.psram_free_start = start_snap.psram_free;
        metrics.psram_free_min_seen = start_snap.psram_free;

        const uint32_t n = g_ctx.poly_modulus_degree;
        uint64_t *c0 = static_cast<uint64_t *>(he_esp::alloc_he_buffer(sizeof(uint64_t) * n));
        uint64_t *c1 = static_cast<uint64_t *>(he_esp::alloc_he_buffer(sizeof(uint64_t) * n));
        const size_t out_size = he_esp::mini_ckks_ciphertext_serialized_size(g_ctx);
        uint8_t *out = static_cast<uint8_t *>(he_esp::alloc_he_buffer(out_size));
        update_min_seen(metrics.heap_free_min_seen, metrics.psram_free_min_seen);

        if (!c0 || !c1 || !out)
        {
            metrics.error = "allocation_failed";
            he_esp::free_he_buffer(c0);
            he_esp::free_he_buffer(c1);
            he_esp::free_he_buffer(out);
            return false;
        }

        const char *err = nullptr;
        const uint32_t t0 = millis();
        const bool ok = he_esp::mini_ckks_encrypt_vector(
            g_ctx, values, values_size, g_ctx.scale_bits, fill_random, nullptr, c0, c1, &err);
        const uint32_t t1 = millis();
        update_min_seen(metrics.heap_free_min_seen, metrics.psram_free_min_seen);

        if (!ok)
        {
            metrics.error = err ? err : "encrypt_failed";
            he_esp::free_he_buffer(c0);
            he_esp::free_he_buffer(c1);
            he_esp::free_he_buffer(out);
            return false;
        }

        const double scale = pow(2.0, static_cast<int>(g_ctx.scale_bits));
        const size_t wrote = he_esp::mini_ckks_serialize_ciphertext(g_ctx, scale, c0, c1, out, out_size);
        const uint32_t t2 = millis();
        update_min_seen(metrics.heap_free_min_seen, metrics.psram_free_min_seen);

        if (!wrote)
        {
            metrics.error = "serialize_failed";
            he_esp::free_he_buffer(c0);
            he_esp::free_he_buffer(c1);
            he_esp::free_he_buffer(out);
            return false;
        }

        if (emit_ciphertext)
        {
            Serial.println("BEGIN_CIPHERTEXT_HEX");
            print_hex_payload(out, wrote);
            Serial.println("END_CIPHERTEXT_HEX");
        }

        he_esp::free_he_buffer(c0);
        he_esp::free_he_buffer(c1);
        he_esp::free_he_buffer(out);
        update_min_seen(metrics.heap_free_min_seen, metrics.psram_free_min_seen);

        const MemorySnapshot end_snap = read_memory_snapshot();
        const he_esp::HeAllocationStats end_stats = he_esp::get_he_allocation_stats();
        metrics.ok = true;
        metrics.ciphertext_bytes = wrote;
        metrics.encrypt_ms = t1 - t0;
        metrics.serialize_ms = t2 - t1;
        metrics.total_ms = t2 - t0;
        metrics.tracked_peak = end_stats.peak_bytes;
        metrics.tracked_psram_peak = end_stats.peak_psram_bytes;
        metrics.tracked_internal_peak = end_stats.peak_internal_bytes;
        metrics.heap_free_end = end_snap.heap_free;
        metrics.psram_free_end = end_snap.psram_free;
        metrics.largest_allocation = end_stats.largest_allocation_bytes;
        metrics.total_allocations = end_stats.total_allocations;
        metrics.failed_allocations = end_stats.failed_allocations;
        return true;
    }

    void print_encrypt_metrics(const EncryptMetrics &metrics)
    {
        print_size_kv("CIPHERTEXT_BYTES", metrics.ciphertext_bytes);
        print_size_kv("ENCRYPT_MS", metrics.encrypt_ms);
        print_size_kv("SERIALIZE_MS", metrics.serialize_ms);
        print_size_kv("TOTAL_MS", metrics.total_ms);
        print_size_kv("TRACKED_BYTES_START", metrics.tracked_start);
        print_size_kv("TRACKED_BYTES_PEAK", metrics.tracked_peak);
        print_size_kv("TRACKED_BYTES_PEAK_DELTA", metrics.tracked_peak - metrics.tracked_start);
        print_size_kv("TRACKED_PSRAM_START", metrics.tracked_psram_start);
        print_size_kv("TRACKED_PSRAM_PEAK", metrics.tracked_psram_peak);
        print_size_kv("TRACKED_PSRAM_PEAK_DELTA", metrics.tracked_psram_peak - metrics.tracked_psram_start);
        print_size_kv("TRACKED_INTERNAL_START", metrics.tracked_internal_start);
        print_size_kv("TRACKED_INTERNAL_PEAK", metrics.tracked_internal_peak);
        print_size_kv("TRACKED_INTERNAL_PEAK_DELTA", metrics.tracked_internal_peak - metrics.tracked_internal_start);
        print_size_kv("HEAP_FREE_START", metrics.heap_free_start);
        print_size_kv("HEAP_FREE_MIN_SEEN", metrics.heap_free_min_seen);
        print_size_kv("HEAP_FREE_END", metrics.heap_free_end);
        print_size_kv("PSRAM_FREE_START", metrics.psram_free_start);
        print_size_kv("PSRAM_FREE_MIN_SEEN", metrics.psram_free_min_seen);
        print_size_kv("PSRAM_FREE_END", metrics.psram_free_end);
        print_size_kv("LARGEST_ALLOCATION_BYTES", metrics.largest_allocation);
        print_size_kv("TOTAL_ALLOCATIONS", metrics.total_allocations);
        print_size_kv("FAILED_ALLOCATIONS", metrics.failed_allocations);
    }

    void print_values_csv(const double *values, size_t values_size)
    {
        for (size_t i = 0; i < values_size; ++i)
        {
            if (i)
            {
                Serial.print(",");
            }
            Serial.print(values[i], 12);
        }
    }

    bool encrypt_and_print(const double *values, size_t values_size)
    {
        EncryptMetrics metrics{};
        Serial.println("BEGIN_ENCRYPT_REPORT");
        Serial.print("VALUES=");
        print_values_csv(values, values_size);
        Serial.println();
        print_size_kv("VALUES_COUNT", values_size);
        print_parameters();
        he_esp::reset_he_allocation_peaks();
        print_memory("before_encrypt");
        const bool ok = run_encrypt(values, values_size, true, metrics);
        if (!ok)
        {
            Serial.print("ERROR ");
            Serial.println(metrics.error ? metrics.error : "unknown");
            Serial.println("END_ENCRYPT_REPORT");
            return false;
        }
        print_encrypt_metrics(metrics);
        print_memory("after_encrypt");
        Serial.println("END_ENCRYPT_REPORT");
        return true;
    }

    void benchmark_and_print(double value, uint32_t runs)
    {
        if (!g_ready)
        {
            Serial.println("ERROR not_ready");
            return;
        }
        if (runs == 0)
        {
            runs = 1;
        }
        if (runs > kMaxBenchRuns)
        {
            runs = kMaxBenchRuns;
        }

        uint32_t encrypt_min = 0xFFFFFFFFu;
        uint32_t encrypt_max = 0;
        uint32_t serialize_min = 0xFFFFFFFFu;
        uint32_t serialize_max = 0;
        uint32_t total_min = 0xFFFFFFFFu;
        uint32_t total_max = 0;
        uint64_t encrypt_sum = 0;
        uint64_t serialize_sum = 0;
        uint64_t total_sum = 0;
        size_t max_tracked_delta = 0;
        size_t max_psram_delta = 0;
        size_t max_internal_delta = 0;
        size_t min_heap_free_seen = static_cast<size_t>(~static_cast<size_t>(0));
        size_t min_psram_free_seen = static_cast<size_t>(~static_cast<size_t>(0));
        size_t largest_allocation = 0;
        size_t total_allocations = 0;
        size_t failed_allocations = 0;
        size_t ciphertext_bytes = 0;
        uint32_t completed = 0;

        const MemorySnapshot before = read_memory_snapshot();
        Serial.println("BEGIN_BENCH_REPORT");
        Serial.print("VALUE=");
        Serial.println(value, 12);
        print_size_kv("RUNS_REQUESTED", runs);
        print_parameters();
        he_esp::reset_he_allocation_peaks();
        print_memory("bench_start");

        for (uint32_t i = 0; i < runs; ++i)
        {
            EncryptMetrics metrics{};
            if (!run_encrypt(&value, 1, false, metrics))
            {
                Serial.print("ERROR bench_run_failed index=");
                Serial.print(i);
                Serial.print(" reason=");
                Serial.println(metrics.error ? metrics.error : "unknown");
                break;
            }

            ++completed;
            ciphertext_bytes = metrics.ciphertext_bytes;
            encrypt_sum += metrics.encrypt_ms;
            serialize_sum += metrics.serialize_ms;
            total_sum += metrics.total_ms;
            encrypt_min = min(encrypt_min, metrics.encrypt_ms);
            encrypt_max = max(encrypt_max, metrics.encrypt_ms);
            serialize_min = min(serialize_min, metrics.serialize_ms);
            serialize_max = max(serialize_max, metrics.serialize_ms);
            total_min = min(total_min, metrics.total_ms);
            total_max = max(total_max, metrics.total_ms);
            max_tracked_delta = max(max_tracked_delta, metrics.tracked_peak - metrics.tracked_start);
            max_psram_delta = max(max_psram_delta, metrics.tracked_psram_peak - metrics.tracked_psram_start);
            max_internal_delta = max(max_internal_delta, metrics.tracked_internal_peak - metrics.tracked_internal_start);
            min_heap_free_seen = min_size(min_heap_free_seen, metrics.heap_free_min_seen);
            min_psram_free_seen = min_size(min_psram_free_seen, metrics.psram_free_min_seen);
            largest_allocation = max(largest_allocation, metrics.largest_allocation);
            total_allocations += metrics.total_allocations;
            failed_allocations += metrics.failed_allocations;
        }

        const MemorySnapshot after = read_memory_snapshot();
        print_size_kv("RUNS_COMPLETED", completed);
        print_size_kv("CIPHERTEXT_BYTES", ciphertext_bytes);
        if (completed > 0)
        {
            print_size_kv("ENCRYPT_MS_MIN", encrypt_min);
            print_size_kv("ENCRYPT_MS_AVG", static_cast<size_t>(encrypt_sum / completed));
            print_size_kv("ENCRYPT_MS_MAX", encrypt_max);
            print_size_kv("SERIALIZE_MS_MIN", serialize_min);
            print_size_kv("SERIALIZE_MS_AVG", static_cast<size_t>(serialize_sum / completed));
            print_size_kv("SERIALIZE_MS_MAX", serialize_max);
            print_size_kv("TOTAL_MS_MIN", total_min);
            print_size_kv("TOTAL_MS_AVG", static_cast<size_t>(total_sum / completed));
            print_size_kv("TOTAL_MS_MAX", total_max);
        }
        print_size_kv("TRACKED_PEAK_DELTA_MAX", max_tracked_delta);
        print_size_kv("TRACKED_PSRAM_PEAK_DELTA_MAX", max_psram_delta);
        print_size_kv("TRACKED_INTERNAL_PEAK_DELTA_MAX", max_internal_delta);
        print_size_kv("HEAP_FREE_BEFORE", before.heap_free);
        print_size_kv("HEAP_FREE_MIN_SEEN", min_heap_free_seen);
        print_size_kv("HEAP_FREE_AFTER", after.heap_free);
        print_size_kv("PSRAM_FREE_BEFORE", before.psram_free);
        print_size_kv("PSRAM_FREE_MIN_SEEN", min_psram_free_seen);
        print_size_kv("PSRAM_FREE_AFTER", after.psram_free);
        print_size_kv("LARGEST_ALLOCATION_BYTES", largest_allocation);
        print_size_kv("TOTAL_ALLOCATIONS", total_allocations);
        print_size_kv("FAILED_ALLOCATIONS", failed_allocations);
        print_memory("bench_end");
        Serial.println("END_BENCH_REPORT");
    }

    String arg_after_command(const String &command, const char *name)
    {
        String arg = command.substring(strlen(name));
        arg.trim();
        return arg;
    }

    size_t parse_csv_values(String text, double *out, size_t max_count)
    {
        text.trim();
        if (text.length() == 0 || !out || max_count == 0)
        {
            return 0;
        }

        size_t count = 0;
        int start = 0;
        while (start < text.length() && count < max_count)
        {
            int comma = text.indexOf(',', start);
            if (comma < 0)
            {
                comma = text.length();
            }
            String token = text.substring(start, comma);
            token.trim();
            if (token.length() > 0)
            {
                out[count++] = token.toDouble();
            }
            start = comma + 1;
        }
        return count;
    }

    void handle_command(String command)
    {
        command.trim();
        if (command.length() == 0)
        {
            return;
        }

        if (command == "PING")
        {
            Serial.println("PONG");
            return;
        }

        if (command == "INFO")
        {
            Serial.print("READY=");
            Serial.println(g_ready ? 1 : 0);
            print_parameters();
            print_memory("info");
            return;
        }

        if (command == "MEM")
        {
            print_memory("mem");
            return;
        }

        if (command.startsWith("ENCRYPT"))
        {
            String value_text = arg_after_command(command, "ENCRYPT");
            if (value_text.length() == 0)
            {
                value_text = "1.25";
            }
            const size_t max_values = g_ctx.poly_modulus_degree / 2;
            double *values = static_cast<double *>(he_esp::alloc_he_buffer(sizeof(double) * max_values));
            if (!values)
            {
                Serial.println("ERROR values_allocation_failed");
                return;
            }
            size_t values_count = parse_csv_values(value_text, values, max_values);
            if (values_count == 0)
            {
                values[0] = 1.25;
                values_count = 1;
            }
            encrypt_and_print(values, values_count);
            he_esp::free_he_buffer(values);
            return;
        }

        if (command.startsWith("BENCH"))
        {
            String args = arg_after_command(command, "BENCH");
            double value = 1.25;
            uint32_t runs = 10;
            if (args.length() > 0)
            {
                const int split = args.indexOf(' ');
                if (split < 0)
                {
                    value = args.toDouble();
                }
                else
                {
                    String value_text = args.substring(0, split);
                    String runs_text = args.substring(split + 1);
                    value_text.trim();
                    runs_text.trim();
                    value = value_text.toDouble();
                    runs = static_cast<uint32_t>(runs_text.toInt());
                }
            }
            benchmark_and_print(value, runs);
            return;
        }

        Serial.print("ERROR unknown_command ");
        Serial.println(command);
    }
}

void setup()
{
    Serial.begin(kBaud);
    delay(800);

    Serial.println("MiniEncryptDemo serial mode");
    print_memory("boot");

    if (kEmbeddedPackageBlobSize == 0)
    {
        Serial.println("ERROR no_embedded_package_blob");
        return;
    }

    auto parsed = he_esp::parse_embedded_package(kEmbeddedPackageBlob, kEmbeddedPackageBlobSize);
    if (!parsed.ok)
    {
        Serial.print("ERROR package_parse_failed ");
        Serial.println(parsed.error);
        return;
    }

    const char *err = nullptr;
    if (!he_esp::mini_ckks_init_from_package(parsed.view, g_ctx, &err))
    {
        Serial.print("ERROR mini_init_failed ");
        Serial.println(err ? err : "unknown");
        return;
    }

    g_ready = true;
    Serial.println("READY");
    Serial.println("Send: INFO, MEM, ENCRYPT 1.25, or BENCH 1.25 10");
}

void loop()
{
    if (Serial.available())
    {
        String command = Serial.readStringUntil('\n');
        handle_command(command);
    }
    delay(10);
}
