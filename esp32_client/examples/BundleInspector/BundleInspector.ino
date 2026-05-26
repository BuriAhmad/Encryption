#include <Arduino.h>

#include "bundle_blob_example.h"
#include "../../src/he_bundle_format.h"
#include "../../src/he_memory_plan.h"

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("HE Bundle Inspector starting...");

    if (kBundleBlobSize == 0)
    {
        Serial.println("No bundle data. Populate bundle_blob_example.h first.");
        return;
    }

    const auto parsed = he_esp::parse_bundle(kBundleBlob, kBundleBlobSize);
    if (!parsed.ok)
    {
        Serial.print("Bundle parse failed: ");
        Serial.println(parsed.error);
        return;
    }

    const auto &h = parsed.view.header;
    Serial.print("Bundle OK. SEAL version: ");
    Serial.print(h.seal_major);
    Serial.print(".");
    Serial.println(h.seal_minor);

    Serial.print("poly_modulus_degree=");
    Serial.println(h.poly_modulus_degree);
    Serial.print("coeff_modulus_count=");
    Serial.println(h.coeff_modulus_count);
    Serial.print("scale_bits=");
    Serial.println(h.scale_bits);

    const auto mem = he_esp::estimate_memory(h.poly_modulus_degree, h.coeff_modulus_count);
    Serial.print("estimate plaintext bytes=");
    Serial.println(mem.plaintext_bytes);
    Serial.print("estimate ciphertext bytes=");
    Serial.println(mem.ciphertext_bytes);
    Serial.print("estimate public key bytes=");
    Serial.println(mem.public_key_bytes);
    Serial.print("estimate working set bytes=");
    Serial.println(mem.working_set_bytes);
}

void loop()
{
    delay(3000);
}
