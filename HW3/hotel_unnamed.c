#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define MAX_ROOMS 100
#define MAX_CLIENTS 100
#define MAX_DAYS 10000
#define ADMIN_EVENT MAX_CLIENTS
#define DAY_DONE (MAX_CLIENTS + 1)
#define ROOM_LOCK (MAX_CLIENTS + 2)
#define SEM_COUNT (MAX_CLIENTS + 3)

enum ClientStatus {
    NOT_ARRIVED,
    WAITING,
    ASSIGNED,
    LIVING,
    LEFT,
    REJECTED
};

struct Client {
    int arrival_time;          // время прибытия
    int stay_days;             // срок проживания
    int check_in_time;         // время заселения
    int room_num;              // индекс номера, -1 если не назначен
    enum ClientStatus state;
    int work_pending;          // есть необработанное событие
};

// данные, общие для родителя и детей
struct SharedState {
    sem_t semaphores[SEM_COUNT];
    struct Client clients[MAX_CLIENTS];
    int rooms[MAX_ROOMS];
    int queue[MAX_CLIENTS];
    int room_count, client_count;
    int head, tail;
    int current_time;
    int accepted, completed;
    int stopping;
    int admin_pending;
    int pending_work;
};

struct SharedState *hotel = NULL;
// указатели на семафоры из общей памяти
sem_t *events[SEM_COUNT];
pid_t owner_pid;
pid_t children[MAX_CLIENTS + 1];
int child_count = 0;
int failed = 0;
char memory_name[32];
int memory_created = 0;
volatile sig_atomic_t stop_requested = 0;
volatile sig_atomic_t child_changed = 0;

void handle_signal(int signal_number){
    if(signal_number == SIGCHLD)
        child_changed = 1;
    else
        stop_requested = signal_number;
}

int read_int(const char *text, int min, int max, int *value)
{
    char line[128];
    char *end;
    printf("%s", text);
    if (scanf("%127s", line) != 1) {
        if (!stop_requested)
            printf("Ошибка чтения\n");
        return 0;
    }
    int next = getchar();
    if (next != EOF && !isspace((unsigned char)next)) {
        printf("Ошибка: слишком длинное число.\n");
        return 0;
    }
    long number = strtol(line, &end, 10);
    if (end == line || *end != '\0' || number < min || number > max) {
        printf("Ошибка: нужно целое число от %d до %d.\n", min, max);
        return 0;
    }
    *value = (int)number;
    return !stop_requested;
}

int find_free_room(int rooms[], int room_count){
    for(int i = 0; i < room_count; ++i){
        if(rooms[i] == -1)
            return i;
    }
    return -1;
}

int create_memory(void){
    snprintf(memory_name, sizeof(memory_name), "/hotel3u_%ld_memory", (long)owner_pid);
    int fd = shm_open(memory_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if(fd == -1){
        perror("shm_open");
        return 0;
    }
    memory_created = 1;
    int ok = 1;
    if(ftruncate(fd, sizeof(struct SharedState)) == -1){
        perror("ftruncate");
        ok = 0;
    }
    if(ok){
        void *address = mmap(NULL, sizeof(struct SharedState), PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if(address == MAP_FAILED){
            perror("mmap");
            ok = 0;
        }
        else{
            hotel = address;
            memset(hotel, 0, sizeof(*hotel));
        }
    }
    if(close(fd) == -1){
        perror("close shared memory");
        ok = 0;
    }
    return ok;
}

int create_semaphore(int index, unsigned int value){
    // единица разрешает работу семафора между процессами
    sem_t *event = &hotel->semaphores[index];
    if(sem_init(event, 1, value) == -1){
        perror("sem_init");
        return 0;
    }
    events[index] = event;
    return 1;
}

// родитель уничтожает созданные семафоры после завершения детей
void close_resources(void){
    if(getpid() == owner_pid){
        for(int i = 0; i < SEM_COUNT; ++i){
            if(events[i] == NULL)
                continue;
            if(sem_destroy(events[i]) == -1){
                perror("sem_destroy");
                failed = 1;
            }
            events[i] = NULL;
        }
    }
    if(hotel && munmap(hotel, sizeof(*hotel)) == -1){
        perror("munmap");
        failed = 1;
    }
    hotel = NULL;
    if(getpid() == owner_pid && memory_created){
        if(shm_unlink(memory_name) == -1){
            perror("shm_unlink");
            failed = 1;
        }
        memory_created = 0;
    }
}

// родитель узнает об ошибке ребёнка через waitpid
void ipc_error(const char *text){
    perror(text);
    if(getpid() != owner_pid)
        _exit(1);
    failed = 1;
}

void check_children(void){
    child_changed = 0;
    for(int i = 0; i < child_count; ++i){
        if(children[i] == 0)
            continue;
        int status;
        pid_t result = waitpid(children[i], &status, WNOHANG);
        if(result == -1){
            if(errno != EINTR){
                if(errno == ECHILD)
                    children[i] = 0;
                perror("waitpid");
                failed = 1;
            }
        }
        else if(result > 0){
            children[i] = 0;
            if(!WIFEXITED(status) || WEXITSTATUS(status) != 0){
                if(!failed)
                    fprintf(stderr, "Ошибка: дочерний процесс %ld завершился аварийно.\n", (long)result);
                failed = 1;
            }
        }
    }
}

void post_event(sem_t *event){
    if(sem_post(event) == -1)
        ipc_error("sem_post");
}

int wait_event(sem_t *event, int interruptible){
    while(1){
        if(getpid() == owner_pid){
            if(child_changed)
                check_children();
            if(failed || (interruptible && stop_requested))
                return 0;

            // проверяем детей во время ожидания, чтобы заметить сбой
#ifdef __APPLE__
            // в macOS нет sem_timedwait
            if(sem_trywait(event) == 0)
                return 1;
            if(errno == EAGAIN){
                struct timespec pause = {0, 100000};
                nanosleep(&pause, NULL);
                continue;
            }
#else
            struct timespec deadline;
            if(clock_gettime(CLOCK_REALTIME, &deadline) == -1){
                ipc_error("clock_gettime");
                return 0;
            }
            deadline.tv_nsec += 100000000;
            if(deadline.tv_nsec >= 1000000000){
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            if(sem_timedwait(event, &deadline) == 0)
                return 1;
            if(errno == ETIMEDOUT)
                continue;
#endif
        }
        else if(sem_wait(event) == 0)
            return 1;

        if(errno != EINTR){
            ipc_error("sem_wait");
            return 0;
        }
    }
}

// вызываются с захваченным ROOM_LOCK
void wake_client(int client_index){
    if(!hotel->clients[client_index].work_pending){
        hotel->clients[client_index].work_pending = 1;
        ++hotel->pending_work;
        post_event(events[client_index]);
    }
}

void wake_admin(void){
    if(!hotel->admin_pending){
        hotel->admin_pending = 1;
        ++hotel->pending_work;
        post_event(events[ADMIN_EVENT]);
    }
}

void finish_work(void){
    --hotel->pending_work;
    if(hotel->pending_work == 0)
        post_event(events[DAY_DONE]);
}

void check_in(struct Client clients[], int rooms[], int client_index,
        int room_index, int current_time){
    clients[client_index].room_num = room_index;
    clients[client_index].check_in_time = current_time;
    clients[client_index].state = ASSIGNED;
    rooms[room_index] = client_index;
    printf("Администратор назначил клиенту %d номер %d на %d суток.\n",
           client_index + 1, room_index + 1, clients[client_index].stay_days);
    wake_client(client_index);
}

void administrator(void){
    while(1){
        wait_event(events[ADMIN_EVENT], 0);
        wait_event(events[ROOM_LOCK], 0);
        if(hotel->stopping){
            post_event(events[ROOM_LOCK]);
            break;
        }
        while(hotel->head < hotel->tail){
            int free_room = find_free_room(hotel->rooms, hotel->room_count);
            if(free_room == -1)
                break;
            int client_index = hotel->queue[hotel->head];
            ++hotel->head;
            check_in(hotel->clients, hotel->rooms, client_index, free_room, hotel->current_time);
        }
        hotel->admin_pending = 0;
        finish_work();
        post_event(events[ROOM_LOCK]);
    }
}

void client(int client_index){
    struct Client *guest = &hotel->clients[client_index];
    while(1){
        wait_event(events[client_index], 0);
        wait_event(events[ROOM_LOCK], 0);
        if(hotel->stopping){
            post_event(events[ROOM_LOCK]);
            break;
        }
        if(guest->state == NOT_ARRIVED){
            printf("Прибыл клиент %d и запрашивает номер на %d суток.\n",
                   client_index + 1, guest->stay_days);
            hotel->queue[hotel->tail] = client_index;
            ++hotel->tail;
            ++hotel->accepted;
            guest->state = WAITING;
            printf("Клиент %d поставлен в очередь.\n", client_index + 1);
            wake_admin();
        }
        else if(guest->state == ASSIGNED){
            guest->state = LIVING;
            printf("Клиент %d заселился в номер %d на %d суток.\n",
                   client_index + 1, guest->room_num + 1, guest->stay_days);
        }
        else if(guest->state == LIVING){
            int days_lived = hotel->current_time - guest->check_in_time;
            printf("Клиент %d завершил %d суток проживания из %d.\n",
                   client_index + 1, days_lived, guest->stay_days);
            if(days_lived == guest->stay_days){
                printf("Клиент %d выехал из номера %d.\n", client_index + 1, guest->room_num + 1);
                hotel->rooms[guest->room_num] = -1;
                guest->room_num = -1;
                guest->state = LEFT;
                ++hotel->completed;
                wake_admin();
            }
        }
        else if(guest->state == REJECTED){
            printf("Клиент %d не принят: прибытие на сутках %d, приём закрыт.\n",
                   client_index + 1, guest->arrival_time);
        }
        int finished = guest->state == LEFT || guest->state == REJECTED;
        guest->work_pending = 0;
        finish_work();
        post_event(events[ROOM_LOCK]);
        if(finished)
            break;
    }
}

// сигналы остановки обрабатывает только родитель
void prepare_child(const sigset_t *old_mask){
    struct sigaction action = {0};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1 ||
       sigprocmask(SIG_SETMASK, old_mask, NULL) == -1){
        perror("Настройка сигналов ребёнка");
        _exit(1);
    }
}

void stop_children(void){
    if(child_count == 0)
        return;
    if(!failed && wait_event(events[ROOM_LOCK], 0)){
        hotel->stopping = 1;
        post_event(events[ADMIN_EVENT]);
        for(int i = 0; i < hotel->client_count; ++i)
            post_event(events[i]);
        post_event(events[ROOM_LOCK]);
    }
    // при сбое не ждём блокировку, которую мог удерживать ребёнок
    while(1){
        check_children();
        int remaining = 0;
        for(int i = 0; i < child_count; ++i){
            if(children[i] > 0){
                ++remaining;
                if(failed)
                    kill(children[i], SIGKILL);
            }
        }
        if(remaining == 0)
            break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
}

int write_results(int fd){
    int occupied = 0;
    for(int room = 0; room < hotel->room_count; ++room){
        if(hotel->rooms[room] != -1)
            ++occupied;
    }
    const char *status = "завершено";
    if(failed)
        status = "остановлено из-за ошибки";
    else if(stop_requested)
        status = "прервано пользователем";
    return dprintf(fd,
        "Моделирование %s на сутках: %d\n"
        "Всего приняли клиентов: %d.\n"
        "Полностью завершили проживание: %d.\n"
        "Не успели принять клиентов: %d.\n"
        "Ожидают заселения: %d.\n"
        "Занятых номеров: %d.\n",
        status, hotel->current_time, hotel->accepted, hotel->completed,
        hotel->client_count - hotel->accepted, hotel->tail - hotel->head, occupied) >= 0;
}

int main(void)
{
    int room_count, client_count;
    int arrival_min, arrival_max;
    int reception_end, delay_seconds;
    struct Client clients[MAX_CLIENTS] = {0};
    owner_pid = getpid();
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    action.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1 ||
       sigaction(SIGCHLD, &action, NULL) == -1){
        perror("sigaction");
        return 1;
    }

    printf("Гостиница: процессы, неименованные семафоры и общая память POSIX.\n");
    printf("Вводите каждое число с новой строки.\n");
    printf("Стратегия: FIFO по порядку постановки в очередь, в том числе при одновременном прибытии.\n");
    printf("Зона ожидания вмещает всех клиентов. Приём ведётся до указанного момента, не включая его.\n");
    if(!read_int("Количество номеров: ", 1, MAX_ROOMS, &room_count) ||
       !read_int("Количество клиентов: ", 1, MAX_CLIENTS, &client_count) ||
       !read_int("Минимальный интервал прибытия: ", 0, MAX_DAYS, &arrival_min) ||
       !read_int("Максимальный интервал прибытия: ", arrival_min, MAX_DAYS, &arrival_max) ||
       !read_int("Момент прекращения приёма: ", 1, MAX_DAYS, &reception_end) ||
       !read_int("Задержка между условными сутками (0-5 секунд): ", 0, 5, &delay_seconds)){
        return stop_requested ? 128 + stop_requested : 1;
    }
    srand(1);
    int last_arrival = 0;
    for(int i = 0; i < client_count; ++i){
        clients[i].room_num = -1;
        clients[i].check_in_time = -1;
        clients[i].state = NOT_ARRIVED;
        printf("Клиент %d. ", i + 1);
        if(!read_int("Срок проживания: ", 1, MAX_DAYS, &clients[i].stay_days))
            return stop_requested ? 128 + stop_requested : 1;
        last_arrival += arrival_min + rand() % (arrival_max - arrival_min + 1);
        clients[i].arrival_time = last_arrival;
    }
    // выводим накопленный текст до fork
    printf("\n");
    if(stop_requested)
        return 128 + stop_requested;

    if(!create_memory()){
        close_resources();
        return 1;
    }
    hotel->room_count = room_count;
    hotel->client_count = client_count;
    memcpy(hotel->clients, clients, sizeof(clients));
    for(int i = 0; i < room_count; ++i)
        hotel->rooms[i] = -1;
    for(int i = 0; i < client_count && !failed; ++i){
        if(!create_semaphore(i, 0))
            failed = 1;
    }
    if(!failed && (!create_semaphore(ADMIN_EVENT, 0) ||
                   !create_semaphore(DAY_DONE, 0) || !create_semaphore(ROOM_LOCK, 1)))
        failed = 1;

    sigset_t signals, old_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int mask_set = sigprocmask(SIG_BLOCK, &signals, &old_mask) == 0;
    if(!mask_set){
        perror("sigprocmask");
        failed = 1;
    }
    // администратор получает индекс -1
    for(int i = -1; i < client_count && !failed && !stop_requested; ++i){
        pid_t pid = fork();
        if(pid == -1){
            perror("fork");
            failed = 1;
            break;
        }
        if(pid == 0){
            prepare_child(&old_mask);
            if(i == -1)
                administrator();
            else
                client(i);
            close_resources();
            _exit(failed ? 1 : 0);
        }
        children[child_count++] = pid;
    }
    if(mask_set && sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1){
        perror("sigprocmask");
        failed = 1;
    }

    // родитель отсчитывает сутки и будит участников
    while(!failed && !stop_requested){
        if(!wait_event(events[ROOM_LOCK], 1))
            break;
        printf("\nСутки %d\n", hotel->current_time);
        if(hotel->current_time == reception_end)
            printf("Приём новых клиентов закрыт. Очередь продолжает обслуживаться.\n");
        for(int i = 0; i < client_count; ++i){
            if(hotel->clients[i].state == NOT_ARRIVED){
                if(hotel->current_time >= reception_end){
                    hotel->clients[i].state = REJECTED;
                    wake_client(i);
                }
                else if(hotel->clients[i].arrival_time == hotel->current_time)
                    wake_client(i);
            }
            else if(hotel->clients[i].state == LIVING)
                wake_client(i);
        }
        wake_admin();
        post_event(events[ROOM_LOCK]);
        if(!wait_event(events[DAY_DONE], 1))
            break;

        if(!wait_event(events[ROOM_LOCK], 1))
            break;
        int finished = 1;
        for(int i = 0; i < client_count; ++i){
            if(hotel->clients[i].state != LEFT && hotel->clients[i].state != REJECTED){
                finished = 0;
                break;
            }
        }
        post_event(events[ROOM_LOCK]);
        if(finished)
            break;

        // делим задержку, чтобы вовремя заметить сбой ребёнка
        for(int part = 0; part < delay_seconds * 10 && !stop_requested && !failed; ++part){
            if(child_changed)
                check_children();
            struct timespec delay = {0, 100000000};
            while(!stop_requested && !failed && nanosleep(&delay, &delay) == -1){
                if(errno != EINTR){
                    ipc_error("nanosleep");
                    break;
                }
                if(child_changed)
                    check_children();
            }
        }
        if(!stop_requested && !failed){
            if(!wait_event(events[ROOM_LOCK], 1))
                break;
            ++hotel->current_time;
            post_event(events[ROOM_LOCK]);
        }
    }

    stop_children();
    printf("\n");
    if(!write_results(STDOUT_FILENO))
        ipc_error("Ошибка вывода результата");
    int log_fd = creat("hotel.log", 0644);
    if(log_fd == -1)
        ipc_error("Не удалось создать hotel.log");
    else{
        if(!write_results(log_fd))
            ipc_error("Ошибка записи лога");
        if(close(log_fd) == -1)
            ipc_error("Ошибка закрытия лога");
    }
    close_resources();
    if(failed)
        return 1;
    return stop_requested ? 128 + stop_requested : 0;
}
