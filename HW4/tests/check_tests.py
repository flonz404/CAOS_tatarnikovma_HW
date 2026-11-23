from collections import deque
from pathlib import Path
import json
import re
import signal
import socket
import struct
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
BIN = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT).resolve()
RESULTS = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else ROOT / "results"
RESULTS.mkdir(parents=True, exist_ok=True)
passed = []


def until(condition, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.01)
    raise AssertionError("истекло время ожидания теста")


def free_port():
    with socket.socket() as connection:
        connection.bind(("127.0.0.1", 0))
        return connection.getsockname()[1]


def wire(*values):
    return struct.pack("!4I", *values)


def receive(connection):
    data = b""
    while len(data) < 16:
        part = connection.recv(16 - len(data))
        assert part, "неожиданный конец TCP-потока"
        data += part
    return struct.unpack("!4I", data)


class Session:
    def __init__(self, name, data, startup=10, bind="127.0.0.1"):
        self.folder = RESULTS / name
        self.folder.mkdir(exist_ok=True)
        self.data = data
        self.port = free_port()
        self.processes = []
        self.files = []
        self.clients = {}
        self.observers = {}
        text = data if isinstance(data, str) else "\n".join(map(str, data)) + "\n"
        (self.folder / "input.txt").write_text(text)
        self.server = self.spawn("server", "hotel_server", [bind, self.port, startup], text)

    def spawn(self, name, program, args, text=None):
        output = open(self.folder / f"{name}.txt", "w")
        self.files.append(output)
        process = subprocess.Popen([str(BIN / program), *map(str, args)],
                                   stdin=subprocess.PIPE if text is not None else subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT, cwd=self.folder)
        self.processes.append(process)
        if text is not None:
            try:
                process.stdin.write(text.encode())
                process.stdin.close()
            except BrokenPipeError:
                pass
        return process

    def output(self, name):
        return (self.folder / f"{name}.txt").read_text()

    def ready(self):
        until(lambda: "Сервер готов:" in self.output("server") or self.server.poll() is not None)
        assert self.server.poll() is None, self.output("server")

    def add_client(self, number, name=None):
        name = name or f"client_{number}"
        process = self.spawn(name, "hotel_client", ["127.0.0.1", self.port, number])
        self.clients[name] = process
        return process

    def add_observer(self, name="observer"):
        process = self.spawn(name, "hotel_observer", ["127.0.0.1", self.port])
        self.observers[name] = process
        until(lambda: "Новые события:" in self.output(name) or process.poll() is not None)
        assert "Новые события:" in self.output(name), self.output(name)
        return process

    def connect(self):
        connection = socket.create_connection(("127.0.0.1", self.port), timeout=15)
        connection.settimeout(15)
        return connection

    def wait(self, timeout=45):
        deadline = time.monotonic() + timeout
        for process in self.processes:
            process.wait(timeout=max(0.1, deadline - time.monotonic()))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
        for process in self.processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for output in self.files:
            output.close()


def check_model(session):
    rooms_count, count, minimum, maximum, cutoff, _ = session.data[:6]
    stays = session.data[6:]
    output = session.output("server")
    assert session.server.returncode == 0, output
    assert all(process.returncode == 0 for process in session.clients.values())
    rooms, assigned, departed = {}, {}, {}
    waiting = deque()
    arrived, rejected = set(), set()
    day = -1
    for line in output.splitlines():
        match = re.match(r"Сутки (\d+): (.*)", line)
        if not match:
            continue
        current, event = int(match[1]), match[2]
        assert current >= day
        day = current
        match = re.fullmatch(r"получен запрос клиента (\d+) на (\d+) суток\.", event)
        if match:
            guest, stay = map(int, match.groups())
            assert guest not in arrived and current < cutoff and stay == stays[guest - 1]
            arrived.add(guest)
        match = re.fullmatch(r"клиент (\d+) поставлен в очередь\.", event)
        if match:
            guest = int(match[1])
            assert guest in arrived and guest not in waiting
            waiting.append(guest)
        match = re.fullmatch(r"администратор назначил клиенту (\d+) номер (\d+) на (\d+) суток\.", event)
        if match:
            guest, room, stay = map(int, match.groups())
            assert waiting and waiting.popleft() == guest, "нарушена очередь FIFO"
            assert 1 <= room <= rooms_count and room not in rooms and guest not in assigned
            assert stay == stays[guest - 1]
            rooms[room] = guest
            assigned[guest] = (current, room)
        match = re.fullmatch(r"получен выезд клиента (\d+), номер (\d+) освобождён\.", event)
        if match:
            guest, room = map(int, match.groups())
            assert rooms.pop(room) == guest
            assert assigned[guest] == (current - stays[guest - 1], room), "неполный срок проживания"
            assert guest not in departed
            departed[guest] = (current, room)
        match = re.fullmatch(r"клиент (\d+) не принят, приём закрыт\.", event)
        if match:
            guest = int(match[1])
            assert guest not in rejected and guest not in arrived and current == cutoff
            rejected.add(guest)
    assert not rooms and not waiting
    assert arrived == set(departed) and len(arrived | rejected) == count
    assert "завершил" not in output, "сервер не должен собирать вывод о проживании клиентов"
    last_arrival = 0
    for number in range(1, count + 1):
        text = session.output(f"client_{number}")
        arrival = re.search(r"Прибытие: сутки (\d+)\. Проживание: (\d+) суток\.", text)
        assert arrival and int(arrival[2]) == stays[number - 1]
        arrival_time = int(arrival[1])
        assert minimum <= arrival_time - last_arrival <= maximum
        last_arrival = arrival_time
        assert all(int(value) == number for value in re.findall(r"клиент (\d+)", text, re.I))
        if arrival_time >= cutoff:
            assert number in rejected and "не принят" in text and "заселился" not in text
            continue
        assert number in assigned
        assert f"Сутки {arrival_time}: клиент {number} прибыл" in text
        start, room = assigned[number]
        assert f"Сутки {start}: клиент {number} получил назначение и заселился в номер {room}" in text
        progress = re.findall(r"Сутки (\d+): клиент \d+ завершил (\d+) суток проживания из (\d+)\.", text)
        assert [(int(d), int(n), int(total)) for d, n, total in progress] == [
            (start + n, n, stays[number - 1]) for n in range(1, stays[number - 1] + 1)]
        finish, room = departed[number]
        assert f"Сутки {finish}: клиент {number} выехал из номера {room}, сервер подтвердил выезд." in text
    expected_day = max([value[0] for value in departed.values()] + ([cutoff] if rejected else []))
    assert f"Обслуживание завершено на сутках: {expected_day}." in output
    return {"day": expected_day, "accepted": len(arrived), "rejected": len(rejected)}


def run_case(name, function):
    result = function()
    passed.append({"test": name, "result": result})
    print(f"OK {name}", flush=True)


def data_case(path, watch=False):
    data = list(map(int, path.read_text().split()))
    with Session(("observer_" if watch else "") + path.stem, data) as session:
        if "invalid" in path.stem:
            session.wait()
            assert session.server.returncode == 1
            assert "Сервер готов:" not in session.output("server")
            return
        session.ready()
        if watch:
            session.add_observer()
        for number in range(1, data[1] + 1):
            session.add_client(number)
        session.wait(timeout=120)
        result = check_model(session)
        if watch:
            check_observer(session, result)
        return result


def normal_case(name, data, bind="127.0.0.1"):
    with Session(name, data, bind=bind) as session:
        session.ready()
        for number in reversed(range(1, data[1] + 1)):
            session.add_client(number)
        session.wait(timeout=120)
        return check_model(session)


def missing_client():
    with Session("missing_client", [1, 2, 0, 0, 1, 0, 1, 1], startup=1) as session:
        session.ready()
        client = session.add_client(1)
        session.wait()
        assert session.server.returncode == client.returncode == 1
        assert "подключились не все" in session.output("server")
        assert "Моделирование началось" not in session.output("server")


def extra_connections():
    with Session("extra_connections", [1, 2, 0, 0, 1, 0, 1, 1]) as session:
        session.ready()
        session.add_client(1)
        until(lambda: "подключён" in session.output("client_1"))
        duplicate = session.add_client(1, "duplicate")
        invalid = session.add_client(3, "invalid_number")
        assert duplicate.wait(timeout=10) == invalid.wait(timeout=10) == 1
        with session.connect() as stranger:
            stranger.sendall(wire(999, 0, 0, 0))
            assert receive(stranger)[0] == 14
        # незавершённое подключение не мешает обычным клиентам
        with session.connect() as silent:
            silent.sendall(b"\0")
            session.add_client(2)
            session.wait()
        del session.clients["duplicate"], session.clients["invalid_number"]
        return check_model(session)


def interrupted(target, sig, watch=False):
    name = ("observer_" if watch else "") + f"{target}_{sig.name}"
    with Session(name, [1, 3, 0, 0, 1, 1, 100, 100, 100]) as session:
        session.ready()
        if watch:
            session.add_observer()
        for number in range(1, 4):
            session.add_client(number)
        until(lambda: "назначил" in session.output("server"))
        process = session.server if target == "server" else session.clients["client_1"]
        process.send_signal(sig)
        session.wait(timeout=15)
        assert process.returncode == (-sig if sig == signal.SIGKILL else 128 + sig)
        for other in session.processes:
            if other is not process:
                assert other.returncode == 1
        if watch:
            text = session.output("observer")
            assert "завершено" not in text
            assert "остановлено" in text or "связь с сервером потеряна" in text


def fragmented_client():
    with Session("fragmented_client", [1, 2, 0, 0, 1, 0, 1, 1]) as session:
        session.ready()
        with session.connect() as connection:
            for byte in wire(1, 0, 1, 0):
                connection.sendall(bytes([byte]))
                time.sleep(0.002)
            assert receive(connection) == (2, 0, 1, 1)
            session.add_client(2)
            room = None
            while True:
                kind, day, value, extra = receive(connection)
                if kind == 3:
                    if day == 0:
                        connection.sendall(wire(5, day, 1, 0))
                    elif room is None:
                        connection.sendall(wire(4, day, 0, 0))
                    else:
                        connection.sendall(wire(9, day, 1, 1) + wire(10, day, room, 0))
                elif kind == 7:
                    room = value
                    connection.sendall(wire(8, day, room, 1))
                elif kind == 11:
                    break
                else:
                    assert kind == 6
        session.wait()
        assert all(process.returncode == 0 for process in session.processes)


def invalid_message():
    with Session("invalid_message", [1, 1, 0, 0, 1, 0, 1]) as session:
        session.ready()
        with session.connect() as connection:
            connection.sendall(wire(1, 0, 1, 0))
            assert receive(connection)[0] == 2
            assert receive(connection)[0] == 3
            connection.sendall(wire(10, 0, 1, 0))
            assert receive(connection)[0] == 13
        session.wait()
        assert session.server.returncode == 1 and "неверное сообщение" in session.output("server")


def stalled_client():
    with Session("stalled_client", [1, 2, 0, 0, 1, 0, 1, 1]) as session:
        session.ready()
        with session.connect() as connection:
            connection.sendall(wire(1, 0, 1, 0))
            assert receive(connection)[0] == 2
            session.add_client(2)
            assert receive(connection)[0] == 3
            connection.sendall(b"\0\0")
            session.wait(timeout=15)
        assert session.server.returncode == 1 and "не ответил за 10 секунд" in session.output("server")
        assert "получен запрос клиента 2" in session.output("server"), "сервер заблокирован первым клиентом"


def port_busy():
    with Session("port_busy", [1, 1, 0, 0, 1, 0, 1]) as session:
        session.ready()
        duplicate = session.spawn("second_server", "hotel_server", ["127.0.0.1", session.port], "1 1 0 0 1 0 1\n")
        assert duplicate.wait(timeout=10) == 1
        session.add_client(1)
        session.wait()
        return check_model(session)


def fake_server():
    folder = RESULTS / "fragmented_server"
    folder.mkdir(exist_ok=True)
    with socket.socket() as listener, open(folder / "client.txt", "w") as output:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        process = subprocess.Popen([str(BIN / "hotel_client"), "127.0.0.1", str(listener.getsockname()[1]), "1"],
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            with listener.accept()[0] as connection:
                connection.settimeout(10)
                assert receive(connection) == (1, 0, 1, 0)
                for byte in wire(2, 0, 1, 1):
                    connection.sendall(bytes([byte]))
                    time.sleep(0.002)
                connection.sendall(wire(3, 0, 0, 0))
                assert receive(connection) == (5, 0, 1, 0)
                connection.sendall(wire(6, 0, 1, 0) + wire(7, 0, 1, 1))
                assert receive(connection) == (8, 0, 1, 1)
                connection.sendall(wire(3, 1, 0, 0))
                assert receive(connection) == (9, 1, 1, 1)
                assert receive(connection) == (10, 1, 1, 0)
                connection.sendall(wire(11, 1, 1, 0))
            assert process.wait(timeout=10) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def invalid_arguments():
    folder = RESULTS / "invalid_arguments"
    folder.mkdir(exist_ok=True)
    for role, arguments in [
        ("server", []), ("client", []), ("server", ["localhost", "5000"]),
        ("server", ["127.0.0.1", "0"]), ("server", ["127.0.0.1", "65536"]),
        ("server", ["127.0.0.1", "5000", "0"]),
        ("client", ["127.0.0.1", "5000", "101"]),
        ("client", ["0.0.0.0", "5000", "1"]),
        ("client", ["127.0.0.1", str(free_port()), "1"]),
    ]:
        result = subprocess.run([str(BIN / f"hotel_{role}"), *arguments], input="", text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        assert result.returncode == 1, result.stdout
        with open(folder / "output.txt", "a") as output:
            output.write(f"{role} {arguments}\n{result.stdout}\n")


def check_observer(session, result, name="observer"):
    text = session.output(name)
    assert session.observers[name].returncode == 0, text
    assert f"Номеров: {session.data[0]}. Клиентов: {session.data[1]}." in text
    assert list(map(int, re.findall(r"^Сутки (\d+)$", text, re.M))) == list(range(result["day"] + 1))
    connected = re.findall(r"Подключился клиент (\d+)\.", text)
    connected += re.findall(r"Клиент (\d+): ожидает прибытия, подключён\.", text)
    assert sorted(map(int, connected)) == list(range(1, session.data[1] + 1))
    expected, observed, progress, checked = [], [], [], []
    patterns = [
        (r"получен запрос клиента (\d+) на (\d+) суток\.", r"Клиент (\d+) прибыл и запрашивает номер на (\d+) суток\.", "request"),
        (r"клиент (\d+) поставлен в очередь\.", r"Клиент (\d+) поставлен в очередь, место при постановке: \d+\.", "queue"),
        (r"администратор назначил клиенту (\d+) номер (\d+) на (\d+) суток\.", r"Администратор назначил клиенту (\d+) номер (\d+) на (\d+) суток\.", "room"),
        (r"получен выезд клиента (\d+), номер (\d+) освобождён\.", r"Клиент (\d+) выехал, номер (\d+) освобождён\.", "checkout"),
        (r"клиент (\d+) не принят, приём закрыт\.", r"Клиент (\d+) не принят: прибытие на сутках \d+, приём закрыт\.", "rejected"),
        (r"приём закрыт, очередь продолжает обслуживаться\.", r"Приём закрыт\. Очередь продолжает обслуживаться\.", "closed"),
    ]
    for line in session.output("server").splitlines():
        match = re.fullmatch(r"Сутки (\d+): (.*)", line)
        if match:
            for source, _, kind in patterns:
                event = re.fullmatch(source, match[2])
                if event:
                    expected.append((kind, int(match[1]), *map(int, event.groups())))
    day = -1
    for line in text.splitlines():
        match = re.fullmatch(r"Сутки (\d+)", line)
        if match:
            day = int(match[1])
        for _, pattern, kind in patterns:
            event = re.fullmatch(pattern, line)
            if event:
                observed.append((kind, day, *map(int, event.groups())))
        match = re.fullmatch(r"Клиент (\d+) завершил (\d+) суток проживания из (\d+)\.", line)
        if match:
            progress.append((day, *map(int, match.groups())))
        match = re.fullmatch(r"Клиент (\d+) заселился в номер (\d+)\.", line)
        if match:
            checked.append((day, *map(int, match.groups())))
    assert observed == expected, "наблюдатель потерял, переставил или повторил событие"
    expected_progress, expected_checked = [], []
    for number in range(1, session.data[1] + 1):
        client = session.output(f"client_{number}")
        expected_progress.extend(tuple(map(int, item)) for item in re.findall(
            r"Сутки (\d+): клиент (\d+) завершил (\d+) суток проживания из (\d+)\.", client))
        expected_checked.extend(tuple(map(int, item)) for item in re.findall(
            r"Сутки (\d+): клиент (\d+) получил назначение и заселился в номер (\d+)", client))
    assert sorted(progress) == sorted(expected_progress), "неполная информация о проживании"
    assert sorted(checked) == sorted(expected_checked), "нет подтверждения заселения"
    assert f"Моделирование завершено на сутках: {result['day']}." in text
    assert f"Завершили проживание: {result['accepted']}. Не приняты: {result['rejected']}." in text


def read_snapshot(connection):
    messages = [receive(connection)]
    assert messages[0][0] == 16
    while messages[-1][0] != 23:
        messages.append(receive(connection))
        assert len(messages) <= 303, "снимок не завершён"
    assert messages[1][0] == 19
    assert sorted(item[2] for item in messages if item[0] == 20) == list(range(1, messages[0][3] + 1))
    assert sorted(item[2] for item in messages if item[0] == 21) == list(range(1, messages[0][3] + 1))
    return messages


def check_late_observer(session, name, result):
    text = session.output(name)
    assert session.observers[name].returncode == 0, text
    snapshot, live = text.split("Новые события:\n", 1)
    assert len(re.findall(r"Клиент \d+: прибытие на сутках", snapshot)) == session.data[1]
    for number, stay in enumerate(session.data[6:], 1):
        assert re.search(fr"Клиент {number}: прибытие на сутках \d+, срок проживания {stay} суток\.", snapshot)
    actual = [line for line in live.splitlines() if line and "(после подключения)" not in line]
    reference = [line for line in session.output("observer").split("Новые события:\n", 1)[1].splitlines() if line]
    assert actual and reference[-len(actual):] == actual, "после снимка потеряны или повторены события"
    assert f"Завершили проживание: {result['accepted']}. Не приняты: {result['rejected']}." in live


def observers_dynamic():
    with Session("observers_dynamic", [1, 3, 0, 0, 1, 1, 1, 1, 1]) as session:
        session.ready()
        session.add_client(1)
        until(lambda: "подключён" in session.output("client_1"))
        session.add_observer()
        second = session.add_observer("observer_second")
        for number in (2, 3):
            session.add_client(number)
        until(lambda: len(re.findall(r"Клиент \d+ заселился", session.output("observer"))) >= 2)
        session.add_observer("observer_late")
        snapshot = session.output("observer_late").split("Новые события:")[0]
        assert "завершил проживание" in snapshot and "проживает в номере 1" in snapshot
        assert "место в очереди: 1" in snapshot and "время заселения – сутки 1" in snapshot
        second.terminate()
        assert second.wait(timeout=10) == 143
        session.add_observer("observer_reconnected")
        session.wait()
        result = check_model(session)
        check_observer(session, result)
        check_late_observer(session, "observer_late", result)
        check_late_observer(session, "observer_reconnected", result)


def observer_disconnect(sig):
    with Session(f"observer_disconnect_{sig.name}", [1, 2, 0, 0, 1, 1, 1, 1]) as session:
        session.ready()
        observer = session.add_observer()
        session.add_observer("observer_healthy")
        for number in (1, 2):
            session.add_client(number)
        until(lambda: "назначил" in session.output("server"))
        observer.send_signal(sig)
        session.wait()
        assert observer.returncode == (-sig if sig == signal.SIGKILL else 128 + sig)
        result = check_model(session)
        check_observer(session, result, "observer_healthy")
        return result


def observer_startup_timeout():
    with Session("observer_startup_timeout", [1, 1, 0, 0, 1, 0, 1], startup=1) as session:
        session.ready()
        session.add_observer()
        session.wait()
        assert all(process.returncode == 1 for process in session.processes)
        assert "остановлено" in session.output("observer")


def observer_invalid_message():
    with Session("observer_invalid_message", [1, 1, 0, 0, 1, 0, 1]) as session:
        session.ready()
        session.add_observer()
        with session.connect() as observer:
            for byte in wire(15, 0, 0, 0):
                observer.sendall(bytes([byte]))
            snapshot = read_snapshot(observer)
            assert snapshot[0] == (16, 1, 1, 1)
            observer.sendall(wire(1, 0, 1, 0))
            assert receive(observer)[0] == 14
        session.add_client(1)
        session.wait()
        result = check_model(session)
        check_observer(session, result)
        return result


def observer_slow():
    with Session("observer_slow", [10, 10, 0, 0, 1, 0] + [1000] * 10) as session:
        session.ready()
        session.add_observer()
        with socket.socket() as observer:
            observer.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 512)
            observer.settimeout(10)
            observer.connect(("127.0.0.1", session.port))
            observer.sendall(wire(15, 0, 0, 0))
            assert receive(observer)[0] == 16
            for number in range(1, 11):
                session.add_client(number)
            session.wait(timeout=60)
        result = check_model(session)
        check_observer(session, result)
        return result


def observers_complete():
    with Session("observers_complete", [3, 100, 0, 0, 1, 0] + [i % 3 + 1 for i in range(100)]) as session:
        session.ready()
        for name in ("observer", "second", "third"):
            session.add_observer(name)
        for number in range(1, 101):
            session.add_client(number)
        session.wait(timeout=120)
        result = check_model(session)
        for name in session.observers:
            check_observer(session, result, name)


def observers_after_cutoff():
    with Session("observers_after_cutoff", [1, 3, 1, 1, 3, 1, 4, 1, 1]) as session:
        session.ready()
        session.add_observer()
        for number in (1, 2, 3):
            session.add_client(number)
        until(lambda: "Клиент 1 завершил 2 суток" in session.output("observer"))
        session.add_observer("late")
        snapshot = session.output("late").split("Новые события:")[0]
        for line in ["Состояние гостиницы на сутках 3. Приём закрыт.",
                     "Клиент 1: проживает в номере 1.",
                     "Клиент 2: ожидает номер, место в очереди: 1.", "Клиент 3: не принят."]:
            assert line in snapshot, snapshot
        session.wait()
        result = check_model(session)
        check_observer(session, result)
        check_late_observer(session, "late", result)


def observers_reserved():
    with Session("observers_reserved", [1, 1, 0, 0, 1, 0, 1]) as session:
        session.ready()
        with session.connect() as guest:
            guest.sendall(wire(1, 0, 1, 0))
            assert receive(guest)[0] == 2
            assert receive(guest)[0] == 3
            guest.sendall(wire(5, 0, 1, 0))
            assert receive(guest)[0] == 6
            assert receive(guest)[0] == 7
            session.add_observer()
            assert "назначен номер 1, ожидается подтверждение" in session.output("observer")
            guest.sendall(wire(8, 0, 1, 1))
            assert receive(guest)[0] == 3
            guest.sendall(wire(9, 1, 1, 1) + wire(10, 1, 1, 0))
            assert receive(guest)[0] == 11
        session.wait()
        assert all(process.returncode == 0 for process in session.processes)
        assert "Клиент 1 завершил 1 суток проживания из 1." in session.output("observer")


def observers_limit():
    with Session("observers_limit", [1, 1, 0, 0, 1, 0, 1]) as session:
        session.ready()
        session.add_observer()
        connections = []
        try:
            for _ in range(99):
                observer = session.connect()
                connections.append(observer)
                observer.sendall(wire(15, 0, 0, 0))
                read_snapshot(observer)
            with session.connect() as excess:
                excess.sendall(wire(15, 0, 0, 0))
                assert receive(excess)[0] == 14
            connections.pop().close()
            session.add_observer("replacement")
            session.add_client(1)
            session.wait()
            result = check_model(session)
            check_observer(session, result)
            check_observer(session, result, "replacement")
        finally:
            for connection in connections:
                connection.close()


def observers_reconnect():
    with Session("observers_reconnect", [1, 1, 0, 0, 1, 1, 3]) as session:
        session.ready()
        session.add_observer()
        session.add_client(1)
        until(lambda: "Клиент 1 заселился" in session.output("observer"))
        for index in range(12):
            name = f"connection_{index}"
            observer = session.add_observer(name)
            assert "Клиент 1: проживает в номере 1." in session.output(name)
            observer.terminate()
            assert observer.wait(timeout=10) == 143
        session.add_observer("last")
        session.wait()
        result = check_model(session)
        check_observer(session, result)
        check_late_observer(session, "last", result)


def observer_snapshot_wire(invalid=False):
    folder = RESULTS / ("observer_snapshot_invalid" if invalid else "observer_snapshot_wire")
    folder.mkdir(exist_ok=True)
    with socket.socket() as listener, open(folder / "observer.txt", "w") as output:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        process = subprocess.Popen([str(BIN / "hotel_observer"), "127.0.0.1", str(listener.getsockname()[1])],
                                   stdout=output, stderr=subprocess.STDOUT)
        try:
            with listener.accept()[0] as connection:
                connection.settimeout(10)
                assert receive(connection) == (15, 0, 0, 0)
                messages = [(16, 2, 1, 1), (19, 3, 1, 1), (20, 0, 1, 4), (21, 8, 1, 1),
                            (22, 0, 1, 0), (23, 3, 1, 0)]
                if invalid:
                    messages = messages[:3] + [(21, 999, 1, 1)]
                for byte in b"".join(wire(*message) for message in messages):
                    connection.sendall(bytes([byte]))
                    time.sleep(0.001)
                if not invalid:
                    connection.sendall(wire(3, 4, 0, 0) + wire(9, 4, 1, 4) + wire(10, 4, 1, 1) + wire(18, 4, 1, 0))
            assert process.wait(timeout=10) == (1 if invalid else 0)
            if not invalid:
                assert "Клиент 1 завершил 4 суток проживания из 4." in (folder / "observer.txt").read_text()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


for path in sorted((ROOT / "tests").glob("test_*.txt")):
    run_case(path.stem, lambda path=path: data_case(path))
for name, text in [("invalid_token", "abc\n"), ("empty_input", ""),
                   ("long_number", "1" * 128 + "\n"), ("missing_stay", "1 1 0 0 1 0\n")]:
    def check_input(name=name, text=text):
        with Session(name, text) as session:
            session.wait()
            assert session.server.returncode == 1
    run_case(name, check_input)
run_case("reverse_order", lambda: normal_case("reverse_order", [2, 6, 0, 0, 1, 0, 2, 4, 1, 3, 2, 1]))
run_case("wildcard_address", lambda: normal_case("wildcard_address", [1, 2, 0, 0, 1, 0, 1, 2], "0.0.0.0"))
for index in range(3):
    name = f"stress_{index}"
    data = [1 + index * 3, 100, 0, 0, 1, 0] + [(i * 7 + index) % 10 + 1 for i in range(100)]
    run_case(name, lambda name=name, data=data: normal_case(name, data))
run_case("missing_client", missing_client)
run_case("extra_connections", extra_connections)
run_case("fragmented_client", fragmented_client)
run_case("fragmented_server", fake_server)
run_case("invalid_message", invalid_message)
run_case("stalled_client", stalled_client)
run_case("port_busy", port_busy)
run_case("invalid_arguments", invalid_arguments)
for target in ["server", "client"]:
    for sig in [signal.SIGINT, signal.SIGTERM, signal.SIGKILL]:
        run_case(f"{target}_{sig.name}", lambda target=target, sig=sig: interrupted(target, sig))
for path in sorted((ROOT / "tests").glob("test_*.txt")):
    if "invalid" not in path.stem:
        run_case("observer_" + path.stem, lambda path=path: data_case(path, watch=True))
run_case("observers_dynamic", observers_dynamic)
run_case("observer_startup_timeout", observer_startup_timeout)
run_case("observer_invalid_message", observer_invalid_message)
run_case("observer_slow", observer_slow)
for sig in [signal.SIGINT, signal.SIGTERM, signal.SIGKILL]:
    run_case(f"observer_disconnect_{sig.name}", lambda sig=sig: observer_disconnect(sig))
for target, sig in [("server", signal.SIGINT), ("server", signal.SIGKILL), ("client", signal.SIGTERM)]:
    run_case(f"observer_{target}_{sig.name}", lambda target=target, sig=sig: interrupted(target, sig, watch=True))
for test in [observers_complete, observers_after_cutoff, observers_reserved, observers_limit, observers_reconnect]:
    run_case(test.__name__, test)
run_case("observer_snapshot_wire", observer_snapshot_wire)
run_case("observer_snapshot_invalid", lambda: observer_snapshot_wire(invalid=True))
(RESULTS / "summary.json").write_text(json.dumps(passed, ensure_ascii=False, indent=2) + "\n")
print(f"Пройдено тестов: {len(passed)}.")
