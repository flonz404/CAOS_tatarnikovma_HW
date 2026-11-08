#!/bin/sh

cd "$(dirname "$0")" || exit 1
compiler=${CC:-gcc}
system=$(uname -s)
variant=${1:-all}

case "$variant" in
    named|unnamed|all) ;;
    *) echo "Использование: sh run_tests.sh [named|unnamed|all]"; exit 1 ;;
esac

if [ "$variant" = unnamed ] && [ "$system" != Linux ]; then
    echo "Неименованные межпроцессные семафоры проверяются в Linux."
    exit 1
fi

run_variant() {
    name=$1
    source=$2
    # В старых версиях glibc функции общей памяти находятся в librt.
    if [ "$system" = Linux ]; then
        "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread "$source.c" -o "$source" -lrt || return 1
    else
        "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread "$source.c" -o "$source" || return 1
    fi
    python3 tests/check_tests.py "./$source" "$name"
}

if [ "$variant" != unnamed ]; then
    run_variant named hotel || exit 1
fi
if [ "$variant" != named ]; then
    if [ "$system" = Linux ]; then
        run_variant unnamed hotel_unnamed || exit 1
    else
        echo "Вариант unnamed пропущен: для него нужен Linux."
    fi
fi
