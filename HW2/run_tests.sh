#!/bin/sh

# Скрипт можно запускать из любой папки. Для проверки вывода нужен Python 3.
cd "$(dirname "$0")" || exit 1
compiler=${CC:-gcc}
"$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread hotel.c -o hotel || exit 1
python3 tests/check_tests.py ./hotel
