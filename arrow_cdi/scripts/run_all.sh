#!/usr/bin/env bash
# Reproducible build + full verification for arrow_cdi.
# Requires: gcc, make, python3 (with pyarrow on sys.path; see README).
set -euo pipefail

cd "$(dirname "$0")/.."

CC="${CC:-gcc}"

echo ">> Cleaning previous build"
make CC="$CC" clean

echo ">> Building release libraries + in-process consumer"
make CC="$CC" all

echo ">> Building AddressSanitizer libraries and test binaries"
make CC="$CC" asan

echo ">> Running native test suite under AddressSanitizer"
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" ./build/native_tests_asan

echo ">> Running Python/PyArrow interop and failpoint tests"
python3 -I tests/py_tests.py

echo ">> Running end-to-end in-process demo"
python3 -I demo/run_demo.py

echo
echo "All build and verification steps completed successfully."
