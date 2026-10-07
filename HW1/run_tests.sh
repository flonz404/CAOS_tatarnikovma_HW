#!/bin/sh

gcc -std=c11 -Wall -Wextra hotel.c -o hotel || exit 1

mkdir -p results || exit 1

# Запуск обычного теста и сравнение итогового лога с ожидаемым.
# Аргументы: название, сутки завершения, принято, не принято.
run_test() {
    name=$1
    ./hotel < "tests/test_$name.txt" > "results/$name.out" 2>&1 || exit 1
    cp hotel.log "results/$name.log" || exit 1

    printf 'Моделирование завершено на сутках: %s\nВсего приняли клиентов: %s.\nНе успели принять клиентов: %s.\n' \
        "$2" "$3" "$4" > "results/$name.expected.log" || exit 1
    diff -u "results/$name.expected.log" "results/$name.log" || exit 1
    printf 'OK: %s\n' "$name"
}

# В двух сложных сценариях проверяем ещё и порядок событий с их временем.
check_events() {
    name=$1
    awk '
        /^Сутки / { day = $2 }
        /^Администратор заселил/ || /^Клиент [0-9]+ выехал/ {
            print day ": " $0
        }
    ' "results/$name.out" > "results/$name.events" || exit 1
    diff -u "tests/expected/$name.events" "results/$name.events" || exit 1
    printf 'OK: порядок событий %s\n' "$name"
}

# Ошибочный ввод должен завершить программу с кодом 1 и сообщением.
run_error_test() {
    name=$1
    if ./hotel < "tests/test_$name.txt" > "results/$name.out" 2>&1; then
        printf 'ОШИБКА: %s принят как корректный ввод\n' "$name"
        exit 1
    else
        code=$?
    fi
    if [ "$code" -ne 1 ]; then
        printf 'ОШИБКА: %s завершился с кодом %s вместо 1\n' "$name" "$code"
        exit 1
    fi
    grep -F "$2" "results/$name.out" > /dev/null || exit 1
    printf 'OK: %s\n' "$name"
}

run_test cutoff 5 2 1
run_test empty 3 0 2
run_test rooms 2 3 0
run_test minimum 1 1 0
run_test max_rooms 1 100 0
run_test max_queue 100 100 0
run_test max_stay 10001 2 0
run_test max_interval 10000 0 2
run_test queue_priority 5 3 0
check_events queue_priority
run_test simultaneous_checkout 5 5 0
check_events simultaneous_checkout

run_error_test invalid_rooms 'Ошибка: нужно целое число от 1 до 100.'
run_error_test invalid_interval 'Ошибка: нужно целое число от 5 до 10000.'
run_error_test invalid_stay 'Ошибка: нужно целое число от 1 до 10000.'

printf '\nВсе 13 тестов пройдены. Результаты сохранены в results.\n'
