#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#define MAX_ROOMS 100
#define MAX_CLIENTS 100
#define MAX_DAYS 10000

enum ClientStatus {
    NOT_ARRIVED,
    WAITING,
    ASSIGNED,
    LIVING,
    LEFT,
    REJECTED
};

struct Client {
    int arrival_time;          // Момент прибытия клиента
    int stay_days;             // Заказанный срок проживания
    int check_in_time;         // Момент фактического заселения
    int room_num;              // Индекс номера, -1 - отсутствие номера
    enum ClientStatus state;  // Текущее состояние клиента
    int index;                 // Аргумент, передаваемый потоку клиента
    int work_pending;          // Есть ли ещё не обработанное событие
    sem_t *event;              // Прибытие, назначение номера или новые сутки
};

// Эти данные используют несколько потоков. Работа с ними защищена мьютексом.
struct Client clients[MAX_CLIENTS];
int rooms[MAX_ROOMS];
int queue[MAX_CLIENTS];
int head = 0;
int tail = 0;
int room_count, client_count;
int current_time = 0;
int accepted = 0;
int completed = 0;
int stopping = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t *admin_event;
sem_t *day_done;
int admin_pending = 0;
int pending_work = 0;          // События, которые потоки ещё обрабатывают

// Сигналы получает только main. Обработчик не работает с мьютексом и выводом.
volatile sig_atomic_t stop_requested = 0;

void handle_signal(int signal_number){
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

    // scanf не должен принимать только начало слишком длинного числа.
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

// Именованные POSIX-семафоры работают и в Linux, и в macOS.
// Сразу удаляем имя: сам семафор живёт до закрытия последнего дескриптора.
sem_t *create_semaphore(int index){
    char name[32];
    snprintf(name, sizeof(name), "/hotel_%ld_%d", (long)getpid(), index);
    sem_t *semaphore = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if(semaphore == SEM_FAILED){
        perror("sem_open");
        return NULL;
    }
    if(sem_unlink(name) == -1){
        perror("sem_unlink");
        sem_close(semaphore);
        return NULL;
    }
    return semaphore;
}

void post_event(sem_t *event){
    if(sem_post(event) == -1){
        perror("sem_post");
        exit(1);
    }
}

int wait_event(sem_t *event, int interruptible){
    while(sem_wait(event) == -1){
        if(errno == EINTR){
            if(interruptible && stop_requested)
                return 0;
            continue;
        }
        perror("sem_wait");
        exit(1);
    }
    return 1;
}

// Следующие три функции вызываются только под мьютексом.
void wake_client(int client_index){
    if(!clients[client_index].work_pending){
        clients[client_index].work_pending = 1;
        ++pending_work;
        post_event(clients[client_index].event);
    }
}

void wake_admin(void){
    if(!admin_pending){
        admin_pending = 1;
        ++pending_work;
        post_event(admin_event);
    }
}

void finish_work(void){
    --pending_work;
    if(pending_work == 0)
        post_event(day_done);
}

// Администратор резервирует свободный номер, затем будит именно этого клиента.
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

void *administrator(void *argument){
    (void)argument;
    while(1){
        wait_event(admin_event, 0);
        pthread_mutex_lock(&mutex);
        if(stopping){
            pthread_mutex_unlock(&mutex);
            break;
        }

        while(head < tail){
            int free_room = find_free_room(rooms, room_count);
            if(free_room == -1)
                break;
            int client_index = queue[head];
            ++head;
            check_in(clients, rooms, client_index, free_room, current_time);
        }

        admin_pending = 0;
        finish_work();
        pthread_mutex_unlock(&mutex);
    }
    return NULL;
}

void *client(void *argument){
    int client_index = *(int *)argument;
    struct Client *guest = &clients[client_index];

    while(1){
        wait_event(guest->event, 0);
        pthread_mutex_lock(&mutex);
        if(stopping){
            pthread_mutex_unlock(&mutex);
            break;
        }

        if(guest->state == NOT_ARRIVED){
            printf("Прибыл клиент %d и запрашивает номер на %d суток.\n",
                   client_index + 1, guest->stay_days);
            queue[tail] = client_index;
            ++tail;
            ++accepted;
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
            int days_lived = current_time - guest->check_in_time;
            printf("Клиент %d завершил %d суток проживания из %d.\n",
                   client_index + 1, days_lived, guest->stay_days);
            if(days_lived == guest->stay_days){
                printf("Клиент %d выехал из номера %d.\n",
                       client_index + 1, guest->room_num + 1);
                rooms[guest->room_num] = -1;
                guest->room_num = -1;
                guest->state = LEFT;
                ++completed;
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
        pthread_mutex_unlock(&mutex);
        if(finished)
            break;
    }
    return NULL;
}

// Вызывается после pthread_join: к этому моменту общие данные уже не меняются.
int write_results(int fd, int failed){
    int occupied = 0;
    for(int room = 0; room < room_count; ++room){
        if(rooms[room] != -1)
            ++occupied;
    }
    const char *status = "завершено";
    if(stop_requested)
        status = "прервано пользователем";
    else if(failed)
        status = "остановлено из-за ошибки";

    return dprintf(fd,
        "Моделирование %s на сутках: %d\n"
        "Всего приняли клиентов: %d.\n"
        "Полностью завершили проживание: %d.\n"
        "Не успели принять клиентов: %d.\n"
        "Ожидают заселения: %d.\n"
        "Занятых номеров: %d.\n",
        status, current_time, accepted, completed,
        client_count - accepted, tail - head, occupied) >= 0;
}

int main(void)
{
    int arrival_min, arrival_max;
    int reception_end, delay_seconds;
    pthread_t admin_thread;
    pthread_t client_threads[MAX_CLIENTS];
    int admin_created = 0;
    int clients_created = 0;
    int failed = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1){
        perror("sigaction");
        return 1;
    }

    printf("Гостиница: многопоточная модель.\n");
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

    for(int room = 0; room < room_count; ++room)
        rooms[room] = -1;

    srand(1);
    int last_arrival = 0;
    for(int i = 0; i < client_count; ++i){
        clients[i].index = i;
        clients[i].room_num = -1;
        clients[i].check_in_time = -1;
        clients[i].state = NOT_ARRIVED;
        printf("Клиент %d. ", i + 1);
        if(!read_int("Срок проживания: ", 1, MAX_DAYS, &clients[i].stay_days))
            return stop_requested ? 128 + stop_requested : 1;
        last_arrival += arrival_min + rand() % (arrival_max - arrival_min + 1);
        clients[i].arrival_time = last_arrival;
    }

    admin_event = create_semaphore(MAX_CLIENTS);
    day_done = create_semaphore(MAX_CLIENTS + 1);
    if(!admin_event || !day_done)
        failed = 1;
    for(int i = 0; i < client_count && !failed; ++i){
        clients[i].event = create_semaphore(i);
        if(!clients[i].event)
            failed = 1;
    }

    // Рабочие потоки наследуют блокировку сигналов. После их создания
    // main снимает блокировку только у себя и отвечает за общую остановку.
    sigset_t signals, old_mask;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    int error = pthread_sigmask(SIG_BLOCK, &signals, &old_mask);
    int mask_set = error == 0;
    if(error != 0){
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        failed = 1;
    }

    if(!failed && !stop_requested){
        error = pthread_create(&admin_thread, NULL, administrator, NULL);
        if(error != 0){
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            failed = 1;
        }
        else
            admin_created = 1;
    }
    for(int i = 0; i < client_count && !failed && !stop_requested; ++i){
        error = pthread_create(&client_threads[i], NULL, client, &clients[i].index);
        if(error != 0){
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            failed = 1;
            break;
        }
        ++clients_created;
    }
    if(mask_set){
        error = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
        if(error != 0){
            fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
            failed = 1;
        }
    }

    // main задаёт ход условного времени. Клиенты и администратор сами
    // обрабатывают события и будят друг друга через семафоры.
    // Следующие сутки начинаются только после завершения текущих событий:
    // поэтому даже при задержке 0 ни один клиент не теряет сутки проживания.
    while(!failed && !stop_requested){
        pthread_mutex_lock(&mutex);
        printf("\nСутки %d\n", current_time);
        if(current_time == reception_end)
            printf("Приём новых клиентов закрыт. Очередь продолжает обслуживаться.\n");
        for(int i = 0; i < client_count; ++i){
            if(clients[i].state == NOT_ARRIVED){
                if(current_time >= reception_end){
                    clients[i].state = REJECTED;
                    wake_client(i);
                }
                else if(clients[i].arrival_time == current_time)
                    wake_client(i);
            }
            else if(clients[i].state == LIVING)
                wake_client(i);
        }
        wake_admin();
        pthread_mutex_unlock(&mutex);

        if(!wait_event(day_done, 1) || stop_requested)
            break;

        pthread_mutex_lock(&mutex);
        int finished = 1;
        for(int i = 0; i < client_count; ++i){
            if(clients[i].state != LEFT && clients[i].state != REJECTED){
                finished = 0;
                break;
            }
        }
        pthread_mutex_unlock(&mutex);
        if(finished)
            break;

        struct timespec delay = {delay_seconds, 0};
        while(!stop_requested && nanosleep(&delay, &delay) == -1){
            if(errno != EINTR){
                perror("nanosleep");
                failed = 1;
                break;
            }
        }
        if(!stop_requested && !failed)
            ++current_time;
    }

    // Будим также клиентов, ожидающих прибытия или свободного номера.
    // Нельзя вызывать pthread_join, оставив их навсегда ждать на семафорах.
    pthread_mutex_lock(&mutex);
    stopping = 1;
    if(admin_created)
        post_event(admin_event);
    for(int i = 0; i < clients_created; ++i)
        post_event(clients[i].event);
    pthread_mutex_unlock(&mutex);

    for(int i = 0; i < clients_created; ++i){
        error = pthread_join(client_threads[i], NULL);
        if(error != 0){
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            failed = 1;
        }
    }
    if(admin_created){
        error = pthread_join(admin_thread, NULL);
        if(error != 0){
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            failed = 1;
        }
    }

    printf("\n");
    if(!write_results(STDOUT_FILENO, failed)){
        perror("Ошибка вывода результата");
        failed = 1;
    }
    int log_fd = creat("hotel.log", 0644);
    if(log_fd == -1){
        perror("Не удалось создать hotel.log");
        failed = 1;
    }
    else{
        if(!write_results(log_fd, failed)){
            perror("Ошибка записи лога");
            failed = 1;
        }
        if(close(log_fd) == -1){
            perror("Ошибка закрытия лога");
            failed = 1;
        }
    }

    for(int i = 0; i < client_count; ++i){
        if(clients[i].event && sem_close(clients[i].event) == -1){
            perror("sem_close");
            failed = 1;
        }
    }
    if(admin_event && sem_close(admin_event) == -1){
        perror("sem_close");
        failed = 1;
    }
    if(day_done && sem_close(day_done) == -1){
        perror("sem_close");
        failed = 1;
    }
    pthread_mutex_destroy(&mutex);

    if(failed)
        return 1;
    return stop_requested ? 128 + stop_requested : 0;
}
