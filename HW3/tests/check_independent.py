"""проверка отдельно запускаемых программ гостиницы"""

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


binaries = Path(sys.argv[1]).resolve()
tests = Path(__file__).resolve().parent
results = Path.cwd() / 'results' / 'independent'
results.mkdir(parents=True, exist_ok=True)
passed = 0
sequence = 0
libc = ctypes.CDLL(None, use_errno=True)
libc.shm_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint]
libc.shm_open.restype = ctypes.c_int
libc.sem_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
libc.sem_open.restype = ctypes.c_void_p
libc.sem_close.argtypes = [ctypes.c_void_p]
libc.sem_close.restype = ctypes.c_int


def check_cleanup(session):
    name = f'/h3i_{session}_memory'.encode()
    fd = libc.shm_open(name, os.O_RDWR, 0)
    error = ctypes.get_errno()
    if fd >= 0:
        os.close(fd)
    assert fd == -1 and error == errno.ENOENT, ('осталась общая память', name)
    for index in range(104):
        name = f'/h3i_{session}_{index}'.encode()
        event = libc.sem_open(name, 0)
        error = ctypes.get_errno()
        missing = event in (None, ctypes.c_void_p(-1).value)
        if not missing:
            libc.sem_close(event)
        assert missing and error == errno.ENOENT, ('остался семафор', name)


def success(name):
    global passed
    passed += 1
    print(f'OK: {name}', flush=True)


class HotelRun:
    def __init__(self, name, data, timeout=10, session=None, programs=None, env=None):
        global sequence
        sequence += 1
        self.name = name
        self.session = session or f't{os.getpid():x}{sequence:x}'
        self.directory = results / name
        self.directory.mkdir(exist_ok=True)
        self.data = data
        self.programs = programs or binaries
        self.processes = []
        self.files = []
        self.output_path = self.directory / 'output.txt'
        self.log_path = self.directory / f'hotel_{self.session}.log'
        self.manager = self.launch('hotel_manager', [self.session, str(timeout)],
                                   self.output_path, env=env)
        if data is not None:
            self.manager.stdin.write(data)
            self.manager.stdin.close()

    def launch(self, program, arguments, output_path, env=None, cwd=None):
        output = output_path.open('w')
        self.files.append(output)
        process = subprocess.Popen([str(self.programs / program), *arguments],
                                   stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                                   cwd=cwd or self.directory, text=True, start_new_session=True, env=env)
        self.processes.append(process)
        return process

    def output(self):
        return self.output_path.read_text(errors='replace')

    def wait_output(self, message, timeout=10):
        end = time.monotonic() + timeout
        while message not in self.output():
            assert self.manager.poll() is None and time.monotonic() < end, self.output()[-3000:]
            time.sleep(0.01)

    def ready(self):
        self.wait_output('готова к подключениям.')

    def worker(self, number=None, env=None, cwd=None):
        program = 'hotel_admin' if number is None else 'hotel_client'
        args = [self.session] if number is None else [self.session, str(number)]
        process = self.launch(program, args, self.directory / f'worker_{len(self.processes)}.txt', env, cwd)
        process.stdin.close()
        return process

    def start_workers(self, client_count, reverse=False):
        if not reverse:
            self.worker()
        for index in (range(client_count, 0, -1) if reverse else range(1, client_count + 1)):
            self.worker(index)
        if reverse:
            self.worker()

    def wait(self, timeout=60):
        end = time.monotonic() + timeout
        while True:
            codes = [p.poll() for p in self.processes]
            if all(code is not None for code in codes):
                return codes
            assert time.monotonic() < end, (self.name, codes, self.output()[-3000:])
            time.sleep(0.01)

    def validate(self):
        codes = self.wait()
        assert all(code == 0 for code in codes), (self.name, codes, self.output()[-3000:])
        check_events(self.output(), list(map(int, self.data.split())), self.log_path.read_text())
        check_cleanup(self.session)

    def close(self):
        for process in self.processes:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
        for process in self.processes:
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            if process.stdin and not process.stdin.closed:
                process.stdin.close()
        for output in self.files:
            output.close()


def run_case(name, data, valid=True, reverse=False):
    run = HotelRun(name, data)
    try:
        if valid:
            run.ready()
            run.start_workers(int(data.split()[1]), reverse)
            run.validate()
        else:
            assert run.wait()[0] == 1, run.output()
            assert 'Ошибка' in run.output() and not run.log_path.exists()
            check_cleanup(run.session)
    finally:
        run.close()
    success(name)


def check_signal(role, signal_number, during_startup=False):
    name = f'signal_{role}_{signal_number}' + ('_startup' if during_startup else '')
    data = '1\n3\n0\n0\n1\n5\n10\n10\n10\n'
    run = HotelRun(name, data)
    try:
        run.ready()
        if during_startup:
            target = run.worker(1) if role == 'client' else run.manager
            if role == 'client':
                run.wait_output('Подключено участников: 1 из 4.')
        else:
            run.start_workers(3)
            run.wait_output('заселился')
            target = {'manager': run.manager, 'admin': run.processes[1], 'client': run.processes[2]}[role]
        target.send_signal(signal_number)
        codes = run.wait(timeout=20)
        assert codes[0] == 128 + signal_number, (name, codes, run.output())
        assert target.returncode == 128 + signal_number
        assert all(p is target or p is run.manager or p.returncode == 0 for p in run.processes), codes
        assert 'прервано пользователем' in run.log_path.read_text()
        check_cleanup(run.session)
    finally:
        run.close()
    success(name)


def check_missing_participant():
    run = HotelRun('missing_participant', '1\n2\n0\n0\n1\n0\n1\n1\n', timeout=1)
    try:
        run.ready()
        run.worker()
        run.worker(1)
        codes = run.wait(timeout=15)
        assert codes[0] == 1 and 'не все участники' in run.output(), (codes, run.output())
        check_cleanup(run.session)
    finally:
        run.close()
    success('missing_participant')


def check_duplicates():
    data = '1\n2\n0\n0\n1\n0\n2\n1\n'
    run = HotelRun('duplicate_participants', data)
    try:
        run.ready()
        run.worker()
        run.worker(1)
        run.wait_output('Подключено участников: 2 из 3.')
        assert 'Сутки 0' not in run.output(), 'модель запущена до подключения всех участников'
        for number in (None, 1, 3):
            rejected = run.worker(number)
            assert rejected.wait(timeout=5) == 1
        run.worker(2)
        codes = run.wait()
        assert codes[0] == 0, (codes, run.output())
        assert codes[1:3] == [0, 0] and codes[3:6] == [1, 1, 1] and codes[6] == 0, codes
        check_events(run.output(), list(map(int, data.split())), run.log_path.read_text())
        check_cleanup(run.session)
    finally:
        run.close()
    success('duplicate_participants')


def check_parallel():
    data = '1\n3\n0\n0\n1\n0\n2\n1\n1\n'
    runs = []
    try:
        for index in range(2):
            run = HotelRun(f'parallel_{index}', data)
            runs.append(run)
            run.ready()
        for run in runs:
            run.start_workers(3)
        for run in runs:
            run.validate()
    finally:
        for run in runs:
            run.close()
    success('parallel_sessions')


def check_session_reuse():
    data = '1\n1\n0\n0\n1\n0\n1\n'
    first = HotelRun('session_owner', data)
    second = None
    try:
        first.ready()
        second = HotelRun('duplicate_session', data, session=first.session)
        assert second.wait()[0] == 1 and 'уже существует' in second.output()
        first.start_workers(1)
        first.validate()
    finally:
        first.close()
        if second:
            second.close()
    again = HotelRun('session_reuse', data, session=first.session)
    try:
        again.ready()
        again.start_workers(1)
        again.validate()
    finally:
        again.close()
    success('session_collision_and_reuse')


def check_no_manager():
    session = f'abs{os.getpid():x}'
    for program, extra in [('hotel_admin', []), ('hotel_client', ['1'])]:
        result = subprocess.run([str(binaries / program), session, *extra], capture_output=True,
                                text=True, timeout=5)
        assert result.returncode == 1 and 'не готова' in result.stderr
    check_cleanup(session)
    success('no_manager')


def check_input_signal():
    run = HotelRun('signal_during_input', None)
    try:
        run.wait_output('Гостиница:')
        time.sleep(0.05)
        run.manager.send_signal(signal.SIGINT)
        assert run.wait(timeout=5)[0] == 130, run.output()
        assert not run.log_path.exists()
        check_cleanup(run.session)
    finally:
        run.close()
    success('signal_during_input')


def check_arguments():
    for args in [('hotel_manager',), ('hotel_admin',), ('hotel_client',),
                 ('hotel_manager', '../bad'), ('hotel_admin', 'a' * 13),
                 ('hotel_client', '', '1'), ('hotel_client', 'demo', '0'),
                 ('hotel_client', 'demo', '101'), ('hotel_client', 'demo', '1abc'),
                 ('hotel_manager', 'demo', '0'), ('hotel_manager', 'demo', '301')]:
        result = subprocess.run([str(binaries / args[0]), *args[1:]], input='',
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 1 and result.stderr, (args, result)
    success('invalid_arguments')


def check_failed_log():
    run = HotelRun('failed_log', '1\n1\n0\n0\n1\n0\n1\n')
    try:
        run.ready()
        run.log_path.mkdir()
        run.start_workers(1)
        assert run.wait()[0] == 1 and 'Не удалось создать лог' in run.output(), run.output()
        check_cleanup(run.session)
    finally:
        run.close()
    success('failed_log')


def check_failed_worker():
    run = HotelRun('failed_worker', '1\n3\n0\n0\n1\n1\n10\n10\n10\n')
    try:
        run.ready()
        run.start_workers(3)
        run.wait_output('заселился')
        if sys.platform.startswith('linux'):
            child_file = Path(f'/proc/{run.manager.pid}/task/{run.manager.pid}/children')
            assert not child_file.read_text().strip(), 'координатор создал дочерние процессы'
        victim = run.processes[1]
        victim.kill()
        victim.wait(timeout=3)
        codes = run.wait(timeout=25)
        assert codes[0] == 1 and codes[1] == -signal.SIGKILL, (codes, run.output())
        assert 'остановлено из-за ошибки' in run.log_path.read_text()
        check_cleanup(run.session)
    finally:
        run.close()
    success('independent_pids_and_failed_worker')


def check_different_directories():
    run = HotelRun('different_directories', '1\n1\n0\n0\n1\n0\n1\n')
    try:
        run.ready()
        with tempfile.TemporaryDirectory(prefix='hotel3-worker-') as directory:
            run.worker(cwd=directory)
            run.worker(1, cwd=directory)
            run.validate()
    finally:
        run.close()
    success('different_directories')


def check_failures():
    data = '1\n3\n0\n0\n1\n0\n2\n1\n1\n'
    with tempfile.TemporaryDirectory(prefix='hotel3-independent-') as temporary:
        build = Path(temporary)
        compiler = os.environ.get('CC', 'gcc')
        options = ['-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread']
        source = tests.parent / 'independent'
        subprocess.run([compiler, *options, '-Dsem_open=test_sem_open', '-Dftruncate=test_ftruncate',
                        '-Dmmap=test_mmap', '-c', str(source / 'hotel.c'), '-o', str(build / 'hotel.o')], check=True)
        libraries = ['-lrt'] if sys.platform.startswith('linux') else []
        for role, program in [('manager', 'hotel_manager'), ('administrator', 'hotel_admin'),
                              ('client', 'hotel_client')]:
            subprocess.run([compiler, *options, str(source / f'{role}.c'), str(build / 'hotel.o'),
                            str(tests / 'failures.c'), '-o', str(build / program), *libraries], check=True)
        for failure in ('sem_open', 'ftruncate', 'mmap'):
            run = HotelRun(f'failed_{failure}', data, programs=build,
                           env=dict(os.environ, HOTEL_TEST_FAILURE=failure))
            try:
                assert run.wait()[0] == 1 and failure in run.output(), run.output()
                check_cleanup(run.session)
            finally:
                run.close()
            success(f'failed_{failure}')
        run = HotelRun('failed_attach', data, programs=build)
        try:
            run.ready()
            rejected = run.worker(env=dict(os.environ, HOTEL_TEST_FAILURE='sem_open'))
            assert rejected.wait(timeout=5) == 1
            run.start_workers(3)
            codes = run.wait()
            assert codes[0] == 0 and all(code == 0 for code in codes[2:]), (codes, run.output())
            check_events(run.output(), list(map(int, data.split())), run.log_path.read_text())
            check_cleanup(run.session)
        finally:
            run.close()
        success('failed_attach')


for path in sorted(tests.glob('test_*.txt')):
    run_case(path.stem, path.read_text(), valid=not path.stem.startswith('test_invalid'))

for name, data in [('empty_input', ''), ('missing_stay', '1\n1\n0\n0\n1\n0\n'),
                   ('invalid_text', 'abc\n'), ('invalid_number_tail', '1abc\n'),
                   ('invalid_long_number', '0' * 200 + '1\n'), ('invalid_overflow', '9' * 100 + '\n')]:
    run_case(name, data, valid=False)
run_case('input_without_final_newline', '1\n1\n0\n0\n1\n0\n1')
run_case('clients_before_admin', '1\n3\n0\n0\n1\n0\n2\n1\n1\n', reverse=True)

generator = random.Random(1)
for index in range(10):
    data = [1 if index % 2 == 0 else 7, 100, 0, 0, 1, 0]
    data += [generator.randint(1, 10) for _ in range(100)]
    run_case(f'stress_{index + 1:02}', '\n'.join(map(str, data)) + '\n')

for role in ('manager', 'admin', 'client'):
    for signal_number in (signal.SIGINT, signal.SIGTERM):
        check_signal(role, signal_number)
check_signal('manager', signal.SIGINT, during_startup=True)
check_signal('client', signal.SIGTERM, during_startup=True)
check_missing_participant()
check_duplicates()
check_parallel()
check_session_reuse()
check_no_manager()
check_input_signal()
check_arguments()
check_failed_log()
check_failed_worker()
check_different_directories()
check_failures()
print(f'\nВариант independent: все {passed} проверок пройдены. Результаты сохранены в {results}.')
