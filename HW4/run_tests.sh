#!/bin/sh

cd "$(dirname "$0")" || exit 1
compiler=${CC:-gcc}
for role in server client observer; do
    "$compiler" -std=c11 -Wall -Wextra -Wpedantic "$role.c" hotel.c -o "hotel_$role" || exit 1
done
python3 tests/check_tests.py .
