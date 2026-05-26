#include <Arduino.h>
#include <cmath>

#include "embedded_package_blob.h"
#include "../../src/he_embedded_package.h"
#include "../../src/mini_ckks_client.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_system.h>
#include <esp_heap_caps.h>
#endif

namespace
{
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
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("MiniEncryptDemo start");
#if defined(ARDUINO_ARCH_ESP32)
    Serial.print("free_heap_start=");
    Serial.println(ESP.getFreeHeap());
    Serial.print("free_psram_start=");
    Serial.println(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

    if (kEmbeddedPackageBlobSize == 0)
    {
        Serial.println("No embedded package data. Populate embedded_package_blob.h first.");
        return;
    }

    auto parsed = he_esp::parse_embedded_package(kEmbeddedPackageBlob, kEmbeddedPackageBlobSize);
    if (!parsed.ok)
    {
        Serial.print("Package parse failed: ");
        Serial.println(parsed.error);
        return;
    }

    he_esp::MiniCkksContext ctx{};
    const char *err = nullptr;
    if (!he_esp::mini_ckks_init_from_package(parsed.view, ctx, &err))
    {
        Serial.print("mini init failed: ");
        Serial.println(err ? err : "unknown");
        return;
    }

    const uint32_t n = ctx.poly_modulus_degree;
    uint64_t *c0 = static_cast<uint64_t *>(malloc(sizeof(uint64_t) * n));
    uint64_t *c1 = static_cast<uint64_t *>(malloc(sizeof(uint64_t) * n));
    if (!c0 || !c1)
    {
        Serial.println("Failed to allocate ciphertext buffers");
        free(c0);
        free(c1);
        he_esp::mini_ckks_release(ctx);
        return;
    }

    const uint32_t t0 = millis();
    if (!he_esp::mini_ckks_encrypt_scalar(ctx, 1.25, ctx.scale_bits, fill_random, nullptr, c0, c1, &err))
    {
        Serial.print("mini encrypt failed: ");
        Serial.println(err ? err : "unknown");
        free(c0);
        free(c1);
        he_esp::mini_ckks_release(ctx);
        return;
    }
    const uint32_t t1 = millis();

    size_t out_size = he_esp::mini_ckks_ciphertext_serialized_size(ctx);
    uint8_t *out = static_cast<uint8_t *>(malloc(out_size));
    size_t wrote = he_esp::mini_ckks_serialize_ciphertext(
        ctx, pow(2.0, static_cast<int>(ctx.scale_bits)), c0, c1, out, out_size);
    const uint32_t t2 = millis();

    Serial.print("Ciphertext serialized bytes=");
    Serial.println(wrote);
    Serial.print("encrypt_ms=");
    Serial.println(t1 - t0);
    Serial.print("serialize_ms=");
    Serial.println(t2 - t1);
    Serial.print("First 16 bytes: ");
    for (size_t i = 0; i < 16 && i < wrote; ++i)
    {
        if (i)
        {
            Serial.print(" ");
        }
        if (out[i] < 16)
        {
            Serial.print("0");
        }
        Serial.print(out[i], HEX);
    }
    Serial.println();

    free(out);
    free(c0);
    free(c1);
    he_esp::mini_ckks_release(ctx);
#if defined(ARDUINO_ARCH_ESP32)
    Serial.print("free_heap_end=");
    Serial.println(ESP.getFreeHeap());
    Serial.print("free_psram_end=");
    Serial.println(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif
}

void loop()
{
    delay(5000);
}
