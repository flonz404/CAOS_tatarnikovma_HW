#!/bin/sh

cd "$(dirname "$0")" || exit 1
compiler=${CC:-gcc}
"$compiler" -std=c11 -Wall -Wextra -Wpedantic server.c hotel.c -o hotel_server || exit 1
"$compiler" -std=c11 -Wall -Wextra -Wpedantic client.c hotel.c -o hotel_client || exit 1
python3 tests/check_tests.py .
