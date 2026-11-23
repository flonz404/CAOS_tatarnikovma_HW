from collections import deque
import re


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
        # после обработки событий очередь ждёт только при занятых номерах
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

