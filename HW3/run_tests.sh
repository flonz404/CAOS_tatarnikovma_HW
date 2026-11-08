#!/bin/sh

cd "$(dirname "$0")" || exit 1
compiler=${CC:-gcc}
system=$(uname -s)
variant=${1:-all}

case "$variant" in
    named|unnamed|independent|all) ;;
    *) echo "Использование: sh run_tests.sh [named|unnamed|independent|all]"; exit 1 ;;
esac

if [ "$variant" = unnamed ] && [ "$system" != Linux ]; then
    echo "Неименованные межпроцессные семафоры проверяются в Linux."
    exit 1
fi

run_variant() {
    name=$1
    source=$2
    # в старых версиях glibc функции общей памяти находятся в librt
    if [ "$system" = Linux ]; then
        "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread "$source.c" -o "$source" -lrt || return 1
    else
        "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread "$source.c" -o "$source" || return 1
    fi
    python3 tests/check_tests.py "./$source" "$name"
}

if [ "$variant" = named ] || [ "$variant" = all ]; then
    run_variant named hotel || exit 1
fi
if [ "$variant" = unnamed ] || [ "$variant" = all ]; then
    if [ "$system" = Linux ]; then
        run_variant unnamed hotel_unnamed || exit 1
    else
        echo "Вариант unnamed пропущен: для него нужен Linux."
    fi
fi

if [ "$variant" = independent ] || [ "$variant" = all ]; then
    for role in manager administrator client; do
        program="hotel_$role"
        if [ "$role" = administrator ]; then
            program=hotel_admin
        fi
        if [ "$system" = Linux ]; then
            "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread "independent/$role.c" independent/hotel.c -o "$program" -lrt || exit 1
        else
            "$compiler" -std=c11 -Wall -Wextra -Wpedantic -pthread "independent/$role.c" independent/hotel.c -o "$program" || exit 1
        fi
    done
    python3 tests/check_independent.py . || exit 1
fi
