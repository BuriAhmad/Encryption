#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/pc_tools/build"
OUT_DIR="${ROOT_DIR}/pc_tools/test_vectors"

mkdir -p "${BUILD_DIR}" "${OUT_DIR}"

cmake -S "${ROOT_DIR}/pc_tools" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j4

SEED="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
VALUES="1.25,-2.5,3.75,4.5"

"${BUILD_DIR}/he_tool" export-bundle \
  --bundle-out "${OUT_DIR}/bundle.bin" \
  --secret-out "${OUT_DIR}/secret.bin" \
  --poly 4096 \
  --coeff-bits 50 \
  --scale-bits 20 \
  --seed-hex "${SEED}"

"${BUILD_DIR}/he_tool" encrypt-from-bundle \
  --bundle "${OUT_DIR}/bundle.bin" \
  --out "${OUT_DIR}/cipher.bin" \
  --values "${VALUES}" \
  --seed-hex "${SEED}"

"${BUILD_DIR}/he_tool" export-embedded-package \
  --bundle "${OUT_DIR}/bundle.bin" \
  --out "${OUT_DIR}/embedded_package.bin"

"${BUILD_DIR}/he_tool" encrypt-mini \
  --package "${OUT_DIR}/embedded_package.bin" \
  --out "${OUT_DIR}/mini_cipher.bin" \
  --values "${VALUES}" \
  --rng-seed 12345

"${BUILD_DIR}/he_tool" decrypt-check \
  --bundle "${OUT_DIR}/bundle.bin" \
  --secret "${OUT_DIR}/secret.bin" \
  --cipher "${OUT_DIR}/cipher.bin" \
  --expected "${VALUES}" \
  --print-slots 8 | tee "${OUT_DIR}/decrypt_report.txt"

"${BUILD_DIR}/he_tool" decrypt-check \
  --bundle "${OUT_DIR}/bundle.bin" \
  --secret "${OUT_DIR}/secret.bin" \
  --cipher "${OUT_DIR}/mini_cipher.bin" \
  --expected "${VALUES}" \
  --print-slots 8 | tee "${OUT_DIR}/mini_decrypt_report.txt"

cat > "${OUT_DIR}/vector_manifest.txt" <<EOF
bundle=bundle.bin
secret=secret.bin
cipher=cipher.bin
embedded_package=embedded_package.bin
mini_cipher=mini_cipher.bin
values=${VALUES}
seed_hex=${SEED}
EOF

echo "Test vector artifacts written to ${OUT_DIR}"
