"""Проверка событий модели: FIFO, номера, полные сроки и завершение потоков."""

from collections import deque
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import time


binary = Path(sys.argv[1]).resolve()
tests = Path(__file__).resolve().parent
results = Path.cwd() / "results"
results.mkdir(exist_ok=True)
passed = 0


def check_events(output, data, log):
    room_count, client_count, arrival_min, arrival_max, cutoff, _ = data[:6]
    stays = data[6:]
    rooms = {}
    waiting = deque()
    arrived = set()
    assigned = {}
    living = {}
    progress = {}
    left = set()
    rejected = set()
    day = -1
    closed = False

    def check_end_of_day():
        # После обработки событий никто не должен ждать при свободных номерах.
        assert not assigned, "клиент не подтвердил заселение"
        assert not waiting or len(rooms) == room_count, "очередь при свободном номере"

    for line in output.splitlines():
        match = re.fullmatch(r"Сутки (\d+)", line)
        if match:
            check_end_of_day()
            assert int(match[1]) == day + 1, "пропущены условные сутки"
            day = int(match[1])
            continue

        match = re.fullmatch(r"Прибыл клиент (\d+) и запрашивает номер на (\d+) суток\.", line)
        if match:
            guest, stay = map(int, match.groups())
            assert 1 <= guest <= client_count
            assert guest not in arrived and guest not in rejected
            assert day < cutoff and stay == stays[guest - 1]
            if arrival_min == arrival_max:
                assert day == guest * arrival_min, "неверный момент прибытия"
            arrived.add(guest)
            continue

        match = re.fullmatch(r"Клиент (\d+) поставлен в очередь\.", line)
        if match:
            guest = int(match[1])
            assert guest in arrived and guest not in waiting
            assert guest not in assigned and guest not in living and guest not in left
            waiting.append(guest)
            continue

        match = re.fullmatch(r"Администратор назначил клиенту (\d+) номер (\d+) на (\d+) суток\.", line)
        if match:
            guest, room, stay = map(int, match.groups())
            assert waiting and waiting.popleft() == guest, "нарушен FIFO"
            assert 1 <= room <= room_count and room not in rooms, "номер уже занят"
            assert guest not in rooms.values(), "клиенту назначено два номера"
            assert stay == stays[guest - 1]
            rooms[room] = guest
            assigned[guest] = (room, day)
            assert len(rooms) <= room_count
            continue

        match = re.fullmatch(r"Клиент (\d+) заселился в номер (\d+) на (\d+) суток\.", line)
        if match:
            guest, room, stay = map(int, match.groups())
            assert assigned.pop(guest) == (room, day)
            assert rooms[room] == guest and stay == stays[guest - 1]
            living[guest] = (room, day)
            progress[guest] = 0
            continue

        match = re.fullmatch(r"Клиент (\d+) завершил (\d+) суток проживания из (\d+)\.", line)
        if match:
            guest, days, stay = map(int, match.groups())
            assert guest in living
            assert days == progress[guest] + 1 == day - living[guest][1]
            assert days <= stay == stays[guest - 1], "искажён срок проживания"
            progress[guest] = days
            continue

        match = re.fullmatch(r"Клиент (\d+) выехал из номера (\d+)\.", line)
        if match:
            guest, room = map(int, match.groups())
            assert rooms.pop(room) == guest
            assert living.pop(guest) == (room, day - stays[guest - 1])
            assert progress[guest] == stays[guest - 1], "неполный срок проживания"
            left.add(guest)
            continue

        match = re.fullmatch(r"Клиент (\d+) не принят: прибытие на сутках (\d+), приём закрыт\.", line)
        if match:
            guest, arrival = map(int, match.groups())
            assert 1 <= guest <= client_count and guest not in arrived and guest not in rejected
            assert day == cutoff and arrival >= cutoff
            if arrival_min == arrival_max:
                assert arrival == guest * arrival_min
            rejected.add(guest)
            continue

        if line == "Приём новых клиентов закрыт. Очередь продолжает обслуживаться.":
            assert day == cutoff and not closed
            closed = True

    check_end_of_day()
    assert arrived == left and len(left) + len(rejected) == client_count
    assert not rooms and not waiting and not living
    assert closed == (day >= cutoff)
    if arrival_min == arrival_max:
        expected = {i for i in range(1, client_count + 1) if i * arrival_min < cutoff}
        assert arrived == expected, "неверно обработана граница приёма"

    expected_log = (
        f"Моделирование завершено на сутках: {day}\n"
        f"Всего приняли клиентов: {len(arrived)}.\n"
        f"Полностью завершили проживание: {len(left)}.\n"
        f"Не успели принять клиентов: {len(rejected)}.\n"
        "Ожидают заселения: 0.\n"
        "Занятых номеров: 0.\n"
    )
    assert log == expected_log, "итоговый лог не соответствует событиям"


def run_test(name, text, valid=True, message="Ошибка"):
    global passed
    directory = results / name
    directory.mkdir(exist_ok=True)
    # Удаляем только лог этого теста, чтобы не принять старый результат за новый.
    log_path = directory / "hotel.log"
    if log_path.is_file():
        log_path.unlink()
    result = subprocess.run(
        [str(binary)], input=text, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, cwd=directory, timeout=60,
    )
    (directory / "output.txt").write_text(result.stdout)
    if valid:
        assert result.returncode == 0, (name, result.returncode, result.stdout[-1000:])
        check_events(result.stdout, list(map(int, text.split())), log_path.read_text())
    else:
        assert result.returncode == 1 and message in result.stdout, (name, result.stdout)
        assert not log_path.exists(), "при неверном вводе моделирование не должно запускаться"
    passed += 1
    print(f"OK: {name}", flush=True)


def check_signal(name, signal_number, before_arrival=False):
    global passed
    directory = results / name
    directory.mkdir(exist_ok=True)
    data = "1\n2\n5\n5\n10\n5\n1\n1\n" if before_arrival else "1\n3\n0\n0\n1\n5\n10\n10\n10\n"
    output_path = directory / "output.txt"
    with output_path.open("w") as output:
        process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=output,
                                   stderr=subprocess.STDOUT, text=True, cwd=directory)
        try:
            process.stdin.write(data)
            process.stdin.close()
            deadline = time.monotonic() + 10
            while True:
                text = output_path.read_text(errors="replace")
                ready = "Сутки 0\n" in text
                if not before_arrival:
                    ready = ready and text.count("поставлен в очередь.") == 3 and "заселился" in text
                if ready:
                    break
                assert process.poll() is None and time.monotonic() < deadline, "модель не запустилась"
                time.sleep(0.01)
            process.send_signal(signal_number)
            assert process.wait(timeout=3) == 128 + signal_number
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    log = (directory / "hotel.log").read_text()
    assert "прервано пользователем" in log and "Полностью завершили проживание: 0." in log
    expected = (0, 2, 0, 0) if before_arrival else (3, 0, 2, 1)
    for label, number in zip(("Всего приняли клиентов", "Не успели принять клиентов",
                              "Ожидают заселения", "Занятых номеров"), expected):
        assert f"{label}: {number}." in log, log
    passed += 1
    print(f"OK: {name}", flush=True)


for path in sorted(tests.glob("test_*.txt")):
    run_test(path.stem, path.read_text(), valid=not path.stem.startswith("test_invalid"))

run_test("empty_input", "", valid=False)
run_test("missing_stay", "1\n1\n0\n0\n1\n0\n", valid=False)
run_test("invalid_text", "abc\n", valid=False)
run_test("invalid_number_tail", "1abc\n", valid=False)
run_test("invalid_long_number", "0" * 200 + "1\n", valid=False)
run_test("invalid_overflow", "9" * 100 + "\n", valid=False)
run_test("input_without_final_newline", "1\n1\n0\n0\n1\n0\n1")

# Порядок одновременно пришедших клиентов зависит от планировщика.
# Проверяем инварианты и фактическую очередь, а не побайтовое совпадение вывода.
generator = random.Random(1)
for index in range(20):
    data = [1 if index % 2 == 0 else 7, 100, 0, 0, 1, 0]
    data += [generator.randint(1, 10) for _ in range(100)]
    run_test(f"stress_{index + 1:02}", "\n".join(map(str, data)) + "\n")

check_signal("sigint_queue", signal.SIGINT)
check_signal("sigterm_queue", signal.SIGTERM)
check_signal("sigint_before_arrival", signal.SIGINT, before_arrival=True)
print(f"\nВсе {passed} проверок пройдены. Результаты сохранены в {results}.")
