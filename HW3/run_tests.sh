#!/bin/sh

cd "$(dirname "$0")" || exit 1
compiler=${CC:-gcc}
# В старых версиях glibc функции общей памяти находятся в librt.
if [ "$(uname -s)" = Linux ]; then
    "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread hotel.c -o hotel -lrt || exit 1
else
    "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread hotel.c -o hotel || exit 1
fi
python3 tests/check_tests.py ./hotel
