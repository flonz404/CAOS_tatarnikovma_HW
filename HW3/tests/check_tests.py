"""Проверка гостиницы, дочерних процессов и очистки ресурсов POSIX."""

from check_model import check_events
from pathlib import Path
import ctypes
import errno
import os
import random
import signal
import subprocess
import sys
import tempfile
import time


binary = Path(sys.argv[1]).resolve()
variant = sys.argv[2] if len(sys.argv) > 2 else "named"
assert variant in ("named", "unnamed"), "вариант должен быть named или unnamed"
tests = Path(__file__).resolve().parent
source = tests.parent / ("hotel.c" if variant == "named" else "hotel_unnamed.c")
memory_prefix = "hotel3" if variant == "named" else "hotel3u"
results = Path.cwd() / "results" / variant
results.mkdir(parents=True, exist_ok=True)
passed = 0

libc = ctypes.CDLL(None, use_errno=True)
libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
libc.shm_open.restype = ctypes.c_int
libc.sem_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
libc.sem_open.restype = ctypes.c_void_p
libc.sem_close.argtypes = [ctypes.c_void_p]
libc.sem_close.restype = ctypes.c_int


def check_cleanup(pid):
    # Ничего не создаём: открытие без O_CREAT должно вернуть ENOENT.
    name = f"/{memory_prefix}_{pid}_memory".encode()
    fd = libc.shm_open(name, os.O_RDWR, 0)
    error = ctypes.get_errno()
    if fd >= 0:
        os.close(fd)
    assert fd == -1 and error == errno.ENOENT, ("осталась общая память", name, error)
    for index in range(103):
        name = f"/hotel3_{pid}_{index}".encode()
        semaphore = libc.sem_open(name, 0)
        error = ctypes.get_errno()
        missing = semaphore in (None, ctypes.c_void_p(-1).value)
        if not missing:
            libc.sem_close(semaphore)
        assert missing and error == errno.ENOENT, ("остался семафор", name, error)
    try:
        os.killpg(pid, 0)
    except ProcessLookupError:
        return
    raise AssertionError(f"после завершения остались процессы группы {pid}")


def kill_test_group(process):
    # Каждый тест получает свою группу, поэтому остальные запуски не затрагиваются.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def execute(directory, text, executable=None, env=None):
    process = subprocess.Popen(
        [str(executable or binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, cwd=directory, start_new_session=True, env=env,
    )
    try:
        output, _ = process.communicate(text, timeout=60)
        (directory / "output.txt").write_text(output)
        check_cleanup(process.pid)
        return process.returncode, output
    finally:
        kill_test_group(process)


def run_test(name, text, valid=True, message="Ошибка"):
    global passed
    directory = results / name
    directory.mkdir(exist_ok=True)
    # Удаляем только лог этого теста, чтобы не принять старый результат за новый.
    log_path = directory / "hotel.log"
    if log_path.is_file():
        log_path.unlink()
    code, output = execute(directory, text)
    if valid:
        assert code == 0, (name, code, output[-1000:])
        check_events(output, list(map(int, text.split())), log_path.read_text())
    else:
        assert code == 1 and message in output, (name, output)
        assert not log_path.exists(), "при неверном вводе моделирование не должно запускаться"
    passed += 1
    print(f"OK: {name}", flush=True)


def check_signal(name, signal_number, before_arrival=False, whole_group=False):
    global passed
    directory = results / name
    directory.mkdir(exist_ok=True)
    data = "1\n2\n5\n5\n10\n5\n1\n1\n" if before_arrival else "1\n3\n0\n0\n1\n5\n10\n10\n10\n"
    output_path = directory / "output.txt"
    with output_path.open("w") as output:
        process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=output,
                                   stderr=subprocess.STDOUT, text=True, cwd=directory,
                                   start_new_session=True)
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
            if whole_group:
                os.killpg(process.pid, signal_number)
            else:
                process.send_signal(signal_number)
            assert process.wait(timeout=3) == 128 + signal_number
            check_cleanup(process.pid)
        finally:
            kill_test_group(process)
    log = (directory / "hotel.log").read_text()
    assert "прервано пользователем" in log and "Полностью завершили проживание: 0." in log
    expected = (0, 2, 0, 0) if before_arrival else (3, 0, 2, 1)
    for label, number in zip(("Всего приняли клиентов", "Не успели принять клиентов",
                              "Ожидают заселения", "Занятых номеров"), expected):
        assert f"{label}: {number}." in log, log
    passed += 1
    print(f"OK: {name}", flush=True)


def check_input_signal():
    global passed
    directory = results / "sigint_input"
    directory.mkdir(exist_ok=True)
    process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, cwd=directory,
                               start_new_session=True)
    try:
        assert "Гостиница" in process.stdout.readline()
        process.send_signal(signal.SIGINT)
        output, _ = process.communicate(timeout=3)
        (directory / "output.txt").write_text(output)
        assert process.returncode == 130
        check_cleanup(process.pid)
    finally:
        kill_test_group(process)
    passed += 1
    print("OK: sigint_input", flush=True)


def check_parallel():
    global passed
    running = []
    data = "1\n3\n0\n0\n1\n0\n2\n1\n1\n"
    try:
        for index in range(2):
            directory = results / f"parallel_{index + 1}"
            directory.mkdir(exist_ok=True)
            process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, cwd=directory, start_new_session=True)
            running.append((process, directory))
            process.stdin.write(data)
            process.stdin.close()
            process.stdin = None
        for process, directory in running:
            output, _ = process.communicate(timeout=10)
            (directory / "output.txt").write_text(output)
            assert process.returncode == 0, output
            check_events(output, list(map(int, data.split())), (directory / "hotel.log").read_text())
            check_cleanup(process.pid)
    finally:
        for process, _ in running:
            kill_test_group(process)
    passed += 1
    print("OK: parallel_instances", flush=True)


def check_failed_child():
    global passed
    if not sys.platform.startswith("linux"):
        print("SKIP: проверка PID дочерних процессов через /proc выполняется в Linux", flush=True)
        return
    directory = results / "failed_child"
    directory.mkdir(exist_ok=True)
    output_path = directory / "output.txt"
    data = "1\n3\n0\n0\n1\n5\n10\n10\n10\n"
    with output_path.open("w") as output:
        process = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=output,
                                   stderr=subprocess.STDOUT, text=True, cwd=directory,
                                   start_new_session=True)
        try:
            process.stdin.write(data)
            process.stdin.close()
            deadline = time.monotonic() + 10
            while "заселился" not in output_path.read_text():
                assert process.poll() is None and time.monotonic() < deadline
                time.sleep(0.01)
            child_file = Path(f"/proc/{process.pid}/task/{process.pid}/children")
            children = list(map(int, child_file.read_text().split()))
            assert len(children) == 4, "должны существовать администратор и три клиента"
            os.kill(children[0], signal.SIGKILL)
            assert process.wait(timeout=3) == 1
            check_cleanup(process.pid)
            assert all(not Path(f"/proc/{pid}").exists() for pid in children), "остались дети или зомби"
            assert "остановлено из-за ошибки" in (directory / "hotel.log").read_text()
        finally:
            kill_test_group(process)
    passed += 1
    print("OK: failed_child_and_waitpid", flush=True)


def check_failures():
    global passed
    data = "1\n3\n0\n0\n1\n0\n2\n1\n1\n"
    directory = results / "failed_log"
    directory.mkdir(exist_ok=True)
    (directory / "hotel.log").mkdir(exist_ok=True)
    code, output = execute(directory, data)
    assert code == 1 and "Не удалось создать hotel.log" in output
    passed += 1
    print("OK: failed_log", flush=True)

    # Подмены системных вызовов подключаются только к отдельному тестовому бинарнику.
    # Основная программа не содержит специальных режимов для искусственных ошибок.
    with tempfile.TemporaryDirectory(prefix="hotel3-failures-") as temporary:
        build = Path(temporary)
        compiler = os.environ.get("CC", "gcc")
        options = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread"]
        semaphore_call = "sem_open" if variant == "named" else "sem_init"
        overrides = [f"-D{semaphore_call}=test_{semaphore_call}"]
        if variant == "unnamed":
            overrides += ["-Dsem_destroy=test_sem_destroy", "-Dmunmap=test_munmap"]
        subprocess.run([compiler, *options, "-Dfork=test_fork", *overrides,
                        "-Dftruncate=test_ftruncate", "-Dmmap=test_mmap", "-c",
                        str(source), "-o", str(build / "hotel.o")], check=True)
        libraries = ["-lrt"] if sys.platform.startswith("linux") else []
        faulty = build / "hotel_failures"
        subprocess.run([compiler, *options, str(build / "hotel.o"), str(tests / "failures.c"),
                        "-o", str(faulty), *libraries], check=True)
        if variant == "unnamed":
            directory = results / "unnamed_lifecycle"
            directory.mkdir(exist_ok=True)
            code, output = execute(directory, data, executable=faulty,
                                   env=dict(os.environ, HOTEL_TEST_FAILURE=""))
            assert code == 0, output
            check_events(output, list(map(int, data.split())), (directory / "hotel.log").read_text())
            passed += 1
            print("OK: unnamed_lifecycle", flush=True)
        for failure in ("fork", semaphore_call, "ftruncate", "mmap"):
            directory = results / f"failed_{failure}"
            directory.mkdir(exist_ok=True)
            env = dict(os.environ, HOTEL_TEST_FAILURE=failure)
            code, output = execute(directory, data, executable=faulty, env=env)
            assert code == 1 and failure in output, output
            assert "Ошибка проверки семафоров" not in output, output
            passed += 1
            print(f"OK: failed_{failure}", flush=True)


for path in sorted(tests.glob("test_*.txt")):
    run_test(path.stem, path.read_text(), valid=not path.stem.startswith("test_invalid"))

run_test("empty_input", "", valid=False)
run_test("missing_stay", "1\n1\n0\n0\n1\n0\n", valid=False)
run_test("invalid_text", "abc\n", valid=False)
run_test("invalid_number_tail", "1abc\n", valid=False)
run_test("invalid_long_number", "0" * 200 + "1\n", valid=False)
run_test("invalid_overflow", "9" * 100 + "\n", valid=False)
run_test("input_without_final_newline", "1\n1\n0\n0\n1\n0\n1")

# Порядок одновременно пришедших клиентов зависит от планировщика процессов.
# Проверяем инварианты и фактическую очередь, а не побайтовое совпадение вывода.
generator = random.Random(1)
for index in range(20):
    data = [1 if index % 2 == 0 else 7, 100, 0, 0, 1, 0]
    data += [generator.randint(1, 10) for _ in range(100)]
    run_test(f"stress_{index + 1:02}", "\n".join(map(str, data)) + "\n")

check_signal("sigint_queue", signal.SIGINT)
check_signal("sigterm_queue", signal.SIGTERM)
check_signal("sigint_before_arrival", signal.SIGINT, before_arrival=True)
check_signal("sigint_process_group", signal.SIGINT, whole_group=True)
check_signal("sigterm_process_group", signal.SIGTERM, whole_group=True)
check_input_signal()
check_parallel()
check_failed_child()
check_failures()
print(f"\nВариант {variant}: все {passed} проверок пройдены. Результаты сохранены в {results}.")
