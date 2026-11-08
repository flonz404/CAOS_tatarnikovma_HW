#include "hotel.h"
#include <stdarg.h>

struct SharedState *hotel = NULL;
sem_t *events[SEM_COUNT];
char session[SESSION_LENGTH + 1];
int is_manager = 0;
int registered = 0;
int role_index = -1;
int failed = 0;
pid_t manager_pid;
volatile sig_atomic_t stop_requested = 0;
volatile sig_atomic_t failure_requested = 0;
static int memory_created = 0;
static int created[SEM_COUNT];
static char memory_name[32];

void handle_signal(int signal_number){
    if(signal_number == SIGUSR1)
        failure_requested = 1;
    else
        stop_requested = signal_number;
}

int prepare_program(const char *name){
    size_t length = strlen(name);
    if(length == 0 || length > SESSION_LENGTH){
        fprintf(stderr, "Имя гостиницы должно содержать от 1 до 12 символов.\n");
        return 0;
    }
    for(size_t i = 0; i < length; ++i){
        char c = name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_')){
            fprintf(stderr, "В имени гостиницы допустимы латинские буквы, цифры и _.\n");
            return 0;
        }
    }
    strcpy(session, name);
    snprintf(memory_name, sizeof(memory_name), "/h3i_%s_memory", session);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1 ||
       sigaction(SIGUSR1, &action, NULL) == -1){
        perror("sigaction");
        return 0;
    }
    return 1;
}

int parse_int(const char *text, int min, int max, int *value){
    char *end;
    long number = strtol(text, &end, 10);
    if(end == text || *end != '\0' || number < min || number > max){
        fprintf(stderr, "Ошибка: нужно целое число от %d до %d.\n", min, max);
        return 0;
    }
    *value = (int)number;
    return 1;
}

int read_int(const char *text, int min, int max, int *value){
    char line[128];
    if(stop_requested)
        return 0;
    printf("%s", text);
    if(scanf("%127s", line) != 1){
        if(!stop_requested)
            printf("Ошибка чтения\n");
        return 0;
    }
    int next = getchar();
    if(next != EOF && !isspace((unsigned char)next)){
        printf("Ошибка: слишком длинное число.\n");
        return 0;
    }
    return parse_int(line, min, max, value) && !stop_requested;
}

void ipc_error(const char *text){
    perror(text);
    failed = 1;
}

double current_seconds(void){
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) == -1){
        ipc_error("clock_gettime");
        return 0;
    }
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

void semaphore_name(char name[32], int index){
    snprintf(name, 32, "/h3i_%s_%d", session, index);
}

int create_memory(void){
    int fd = shm_open(memory_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if(fd == -1){
        if(errno == EEXIST)
            fprintf(stderr, "Гостиница с именем %s уже существует.\n", session);
        else
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
            hotel->version = MEMORY_VERSION;
            hotel->manager_pid = getpid();
        }
    }
    if(close(fd) == -1){
        perror("close shared memory");
        ok = 0;
    }
    return ok;
}

int create_events(void){
    // блокировку ROOM_LOCK создаём последней, когда гостиница готова
    for(int i = 0; i < SEM_COUNT; ++i){
        if(i >= hotel->client_count && i < ADMIN_EVENT)
            continue;
        if(stop_requested)
            return 0;
        char name[32];
        semaphore_name(name, i);
        sem_t *event = sem_open(name, O_CREAT | O_EXCL, 0600, (unsigned int)(i == ROOM_LOCK));
        if(event == SEM_FAILED){
            perror("sem_open");
            return 0;
        }
        events[i] = event;
        created[i] = 1;
    }
    return 1;
}

void post_event(sem_t *event){
    if(sem_post(event) == -1)
        ipc_error("sem_post");
}

int wait_event(sem_t *event, double timeout, int interruptible){
    double end = current_seconds() + timeout;
    while(1){
        if(interruptible){
            if(failure_requested)
                failed = 1;
            if(failed || stop_requested)
                return 0;
            if(!is_manager && registered && kill(manager_pid, 0) == -1 && errno == ESRCH){
                fprintf(stderr, "Ошибка: координатор гостиницы завершился.\n");
                failed = 1;
                return 0;
            }
        }
        if(timeout > 0 && current_seconds() >= end){
            fprintf(stderr, "Ошибка: истекло время ожидания участника гостиницы.\n");
            failed = 1;
            return 0;
        }
#ifdef __APPLE__
        if(sem_trywait(event) == 0)
            return 1;
        if(errno == EAGAIN){
            struct timespec pause = {0, 1000000};
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
        if(errno != EINTR){
            ipc_error("sem_wait");
            return 0;
        }
    }
}

int connect_hotel(int index){
    char name[32];
    semaphore_name(name, ROOM_LOCK);
    sem_t *lock = sem_open(name, 0);
    if(lock == SEM_FAILED){
        fprintf(stderr, "Гостиница %s не готова. Сначала запустите координатора.\n", session);
        return 0;
    }
    events[ROOM_LOCK] = lock;
    if(!wait_event(lock, 5, 1))
        return 0;
    int ok = 0;
    int fd = shm_open(memory_name, O_RDWR, 0);
    if(fd == -1)
        perror("shm_open");
    else{
        struct stat info;
        if(fstat(fd, &info) == -1)
            perror("fstat");
        else if(info.st_size < (off_t)sizeof(struct SharedState))
            fprintf(stderr, "Ошибка: неверный размер общей памяти.\n");
        else{
            void *address = mmap(NULL, sizeof(struct SharedState), PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
            if(address == MAP_FAILED)
                perror("mmap");
            else{
                hotel = address;
                ok = 1;
            }
        }
        if(close(fd) == -1){
            perror("close shared memory");
            ok = 0;
        }
    }
    if(ok && (hotel->version != MEMORY_VERSION || hotel->client_count < 1 ||
              hotel->client_count > MAX_CLIENTS || hotel->manager_pid <= 0)){
        fprintf(stderr, "Ошибка: неверные данные гостиницы.\n");
        ok = 0;
    }
    if(ok && ((index != ADMIN_EVENT && (index < 0 || index >= hotel->client_count)) ||
              hotel->started || hotel->stopping || hotel->participants[index] != 0)){
        fprintf(stderr, "Ошибка: участник уже подключён, номер клиента неверен или приём подключений закрыт.\n");
        ok = 0;
    }
    if(ok){
        manager_pid = hotel->manager_pid;
        if(kill(manager_pid, 0) == -1){
            fprintf(stderr, "Ошибка: координатор гостиницы недоступен.\n");
            ok = 0;
        }
    }
    for(int i = 0; i < ROOM_LOCK && ok; ++i){
        if(i >= hotel->client_count && i < ADMIN_EVENT)
            continue;
        if(i < ADMIN_EVENT && index != ADMIN_EVENT && i != index)
            continue;
        semaphore_name(name, i);
        sem_t *event = sem_open(name, 0);
        if(event == SEM_FAILED){
            perror("sem_open");
            ok = 0;
        }
        else
            events[i] = event;
    }
    if(ok && !stop_requested){
        role_index = index;
        registered = 1;
        hotel->participants[index] = getpid();
        ++hotel->joined_count;
        post_event(events[JOIN_EVENT]);
    }
    else
        ok = 0;
    post_event(lock);
    return ok && !failed;
}

void close_resources(void){
    for(int i = 0; i < SEM_COUNT; ++i){
        if(events[i]){
            if(sem_close(events[i]) == -1)
                ipc_error("sem_close");
            events[i] = NULL;
        }
        if(is_manager && created[i]){
            char name[32];
            semaphore_name(name, i);
            if(sem_unlink(name) == -1)
                ipc_error("sem_unlink");
            created[i] = 0;
        }
    }
    if(hotel && munmap(hotel, sizeof(*hotel)) == -1)
        ipc_error("munmap");
    hotel = NULL;
    if(is_manager && memory_created){
        if(shm_unlink(memory_name) == -1)
            ipc_error("shm_unlink");
        memory_created = 0;
    }
}

int finish_participant(void){
    if(failure_requested)
        failed = 1;
    if(registered){
        if(failed || stop_requested){
            int signal_number = failed ? SIGUSR1 : stop_requested;
            if(kill(manager_pid, signal_number) == -1 && errno != ESRCH)
                ipc_error("kill");
        }
        // координатор ждёт эту отметку перед удалением имён ресурсов
        if(wait_event(events[ROOM_LOCK], 5, 0)){
            hotel->finished[role_index] = 1;
            post_event(events[ROOM_LOCK]);
        }
    }
    close_resources();
    return failed ? 1 : (stop_requested ? 128 + stop_requested : 0);
}

// события записываются под ROOM_LOCK, а выводит их координатор
void log_event(const char *format, ...){
    if(hotel->message_count == MAX_MESSAGES){
        errno = ENOSPC;
        ipc_error("Слишком много событий за сутки");
        return;
    }
    va_list args;
    va_start(args, format);
    int length = vsnprintf(hotel->messages[hotel->message_count], 256, format, args);
    va_end(args);
    if(length < 0 || length >= 256){
        errno = EOVERFLOW;
        ipc_error("Слишком длинное событие");
        return;
    }
    ++hotel->message_count;
}

void flush_events(void){
    for(int i = 0; i < hotel->message_count; ++i){
        if(fputs(hotel->messages[i], stdout) == EOF)
            ipc_error("Вывод событий");
    }
    hotel->message_count = 0;
}

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

int find_free_room(int rooms[], int room_count){
    for(int i = 0; i < room_count; ++i){
        if(rooms[i] == -1)
            return i;
    }
    return -1;
}

void check_in(int client_index, int room_index){
    struct Client *guest = &hotel->clients[client_index];
    guest->room_num = room_index;
    guest->check_in_time = hotel->current_time;
    guest->state = ASSIGNED;
    hotel->rooms[room_index] = client_index;
    log_event("Администратор назначил клиенту %d номер %d на %d суток.\n",
              client_index + 1, room_index + 1, guest->stay_days);
    wake_client(client_index);
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
