#include "hotel.h"

pid_t participants[MAX_CLIENTS + 1];
int participant_done[MAX_CLIENTS + 1];

// вызывается с захваченным ROOM_LOCK
void check_participants(void){
    for(int i = 0; i <= MAX_CLIENTS; ++i){
        participants[i] = hotel->participants[i];
        participant_done[i] = hotel->finished[i];
        if(participants[i] > 0 && !participant_done[i] &&
           kill(participants[i], 0) == -1 && errno == ESRCH){
            fprintf(stderr, "Ошибка: участник %ld завершился без подтверждения.\n", (long)participants[i]);
            hotel->finished[i] = 1;
            participant_done[i] = 1;
            failed = 1;
        }
    }
}

int wait_participants(int timeout){
    double end = current_seconds() + timeout;
    int previous_count = -1;
    while(!failed && !stop_requested && !failure_requested){
        if(!wait_event(events[ROOM_LOCK], 5, 1))
            return 0;
        check_participants();
        int count = hotel->joined_count;
        int ready = count == hotel->client_count + 1;
        if(ready && !failed)
            hotel->started = 1;
        post_event(events[ROOM_LOCK]);
        if(count != previous_count){
            printf("Подключено участников: %d из %d.\n", count, hotel->client_count + 1);
            previous_count = count;
        }
        if(ready)
            return !failed && !stop_requested && !failure_requested;
        double remaining = end - current_seconds();
        if(remaining <= 0 || !wait_event(events[JOIN_EVENT], remaining, 1)){
            if(!stop_requested){
                fprintf(stderr, "Ошибка: подключились не все участники гостиницы.\n");
                failed = 1;
            }
            return 0;
        }
    }
    return 0;
}

void stop_participants(void){
    if(wait_event(events[ROOM_LOCK], 5, 0)){
        hotel->stopping = 1;
        check_participants();
        flush_events();
        post_event(events[ROOM_LOCK]);
    }
    // при потере блокировки просим участников завершиться по сигналу
    if(failed){
        for(int i = 0; i <= MAX_CLIENTS; ++i){
            if(participants[i] > 0 && !participant_done[i])
                kill(participants[i], SIGTERM);
        }
    }
    post_event(events[ADMIN_EVENT]);
    for(int i = 0; i < hotel->client_count; ++i)
        post_event(events[i]);

    double end = current_seconds() + 6;
    while(current_seconds() < end){
        if(sem_trywait(events[ROOM_LOCK]) == 0){
            check_participants();
            int remaining = 0;
            for(int i = 0; i <= MAX_CLIENTS; ++i){
                if(participants[i] > 0 && !participant_done[i])
                    ++remaining;
            }
            post_event(events[ROOM_LOCK]);
            if(remaining == 0)
                return;
        }
        else if(errno != EAGAIN && errno != EINTR){
            ipc_error("sem_trywait");
            return;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    fprintf(stderr, "Ошибка: не все участники подтвердили завершение.\n");
    failed = 1;
}

void run_hotel(int reception_end, int delay_seconds){
    while(!failed && !stop_requested && !failure_requested){
        if(!wait_event(events[ROOM_LOCK], 5, 1))
            break;
        check_participants();
        printf("\nСутки %d\n", hotel->current_time);
        if(hotel->current_time == reception_end)
            printf("Приём новых клиентов закрыт. Очередь продолжает обслуживаться.\n");
        for(int i = 0; i < hotel->client_count; ++i){
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
        if(!wait_event(events[DAY_DONE], 10, 1) || !wait_event(events[ROOM_LOCK], 5, 1))
            break;
        flush_events();
        check_participants();
        int finished = 1;
        for(int i = 0; i < hotel->client_count; ++i){
            if(hotel->clients[i].state != LEFT && hotel->clients[i].state != REJECTED){
                finished = 0;
                break;
            }
        }
        post_event(events[ROOM_LOCK]);
        if(finished)
            break;

        for(int part = 0; part < delay_seconds * 10 && !stop_requested && !failed && !failure_requested; ++part){
            struct timespec delay = {0, 100000000};
            while(nanosleep(&delay, &delay) == -1 && !stop_requested && !failure_requested){
                if(errno != EINTR){
                    ipc_error("nanosleep");
                    break;
                }
            }
        }
        if(!failed && !stop_requested && !failure_requested){
            if(!wait_event(events[ROOM_LOCK], 5, 1))
                break;
            ++hotel->current_time;
            post_event(events[ROOM_LOCK]);
        }
    }
}

int main(int argc, char *argv[]){
    if(argc < 2 || argc > 3){
        fprintf(stderr, "Использование: %s имя_гостиницы [ожидание_подключения_в_секундах]\n", argv[0]);
        return 1;
    }
    is_manager = 1;
    manager_pid = getpid();
    int startup_timeout = 60;
    if(!prepare_program(argv[1]) ||
       (argc == 3 && !parse_int(argv[2], 1, 300, &startup_timeout)))
        return 1;
    int room_count, client_count, arrival_min, arrival_max, reception_end, delay_seconds;
    struct Client clients[MAX_CLIENTS] = {0};
    printf("Гостиница: независимо запускаемые участники.\n");
    printf("Вводите каждое число с новой строки.\n");
    printf("Стратегия: FIFO. Зона ожидания вмещает всех клиентов.\n");
    if(!read_int("Количество номеров: ", 1, MAX_ROOMS, &room_count) ||
       !read_int("Количество клиентов: ", 1, MAX_CLIENTS, &client_count) ||
       !read_int("Минимальный интервал прибытия: ", 0, MAX_DAYS, &arrival_min) ||
       !read_int("Максимальный интервал прибытия: ", arrival_min, MAX_DAYS, &arrival_max) ||
       !read_int("Момент прекращения приёма: ", 1, MAX_DAYS, &reception_end) ||
       !read_int("Задержка между условными сутками (0-5 секунд): ", 0, 5, &delay_seconds))
        return stop_requested ? 128 + stop_requested : 1;
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
    if(!create_events()){
        close_resources();
        return stop_requested && !failed ? 128 + stop_requested : 1;
    }
    printf("Гостиница %s готова к подключениям.\n", session);
    printf("Запустите администратора и %d клиентов. Ожидание: %d секунд.\n", client_count, startup_timeout);
    if(wait_participants(startup_timeout))
        run_hotel(reception_end, delay_seconds);
    if(failure_requested)
        failed = 1;
    stop_participants();
    if(failure_requested)
        failed = 1;

    char log_name[64];
    snprintf(log_name, sizeof(log_name), "hotel_%s.log", session);
    int log_fd = creat(log_name, 0644);
    if(log_fd == -1)
        ipc_error("Не удалось создать лог гостиницы");
    if(wait_event(events[ROOM_LOCK], 5, 0)){
        flush_events();
        printf("\n");
        if(!write_results(STDOUT_FILENO))
            ipc_error("Вывод результата");
        if(log_fd != -1 && !write_results(log_fd))
            ipc_error("Запись лога");
        // держим блокировку до удаления имён, чтобы закрыть новые подключения
    }
    else if(log_fd != -1){
        if(dprintf(log_fd, "Моделирование остановлено из-за ошибки: общая память недоступна.\n") < 0)
            ipc_error("Запись лога");
    }
    if(log_fd != -1 && close(log_fd) == -1)
        ipc_error("Закрытие лога");
    close_resources();
    return failed ? 1 : (stop_requested ? 128 + stop_requested : 0);
}
