#include "hotel.h"

#define MAX_CONNECTIONS (MAX_CLIENTS + 8)
#define OUTPUT_SIZE (MESSAGE_SIZE * (MAX_CLIENTS * 8 + 8))

enum ClientStatus { NOT_ARRIVED, WAITING, ASSIGNED, LIVING, LEFT, NOT_ACCEPTED };

struct Client {
    int arrival_time, stay_days;
    int check_in_time, room_num;
    enum ClientStatus state;
    int connection;
    int work_pending;
};

struct Connection {
    int fd, client_index, closing;
    unsigned char input[MESSAGE_SIZE], output[OUTPUT_SIZE];
    size_t received, sent, output_size;
    double connected_at;
};

struct Client clients[MAX_CLIENTS];
struct Connection connections[MAX_CONNECTIONS];
int rooms[MAX_ROOMS], queue[MAX_CLIENTS];
int room_count, client_count, reception_end, delay_seconds;
int head = 0, tail = 0, joined_count = 0, current_time = 0;
int started = 0, failed = 0;
int observer = -1;

void close_connection(int index){
    struct Connection *connection = &connections[index];
    if(connection->fd != -1)
        close(connection->fd);
    connection->fd = -1;
    if(index == observer)
        observer = -1;
}

void connection_lost(int index){
    int guest = connections[index].client_index;
    if(guest >= 0 && clients[guest].state != LEFT && clients[guest].state != NOT_ACCEPTED){
        fprintf(stderr, "Ошибка: потеряна связь с клиентом %d. Моделирование остановлено.\n", guest + 1);
        failed = 1;
    }
    close_connection(index);
}

void queue_message(int index, int type, int day, int value, int extra){
    struct Connection *connection = &connections[index];
    if(connection->fd == -1)
        return;
    if(connection->output_size + MESSAGE_SIZE > OUTPUT_SIZE && connection->sent > 0){
        connection->output_size -= connection->sent;
        memmove(connection->output, connection->output + connection->sent, connection->output_size);
        connection->sent = 0;
    }
    if(connection->output_size + MESSAGE_SIZE > OUTPUT_SIZE){
        if(index == observer){
            fprintf(stderr, "Наблюдатель отключён: не успевает получать события.\n");
            close_connection(index);
            return;
        }
        fprintf(stderr, "Ошибка: переполнен буфер отправки.\n");
        failed = 1;
        return;
    }
    struct Message message = {(uint32_t)type, (uint32_t)day, (uint32_t)value, (uint32_t)extra};
    encode_message(connection->output + connection->output_size, message);
    connection->output_size += MESSAGE_SIZE;
}

void observe(int type, int value, int extra){
    if(observer != -1)
        queue_message(observer, type, current_time, value, extra);
}

int find_free_room(int rooms[], int room_count){
    for(int i = 0; i < room_count; ++i){
        if(rooms[i] == -1)
            return i;
    }
    return -1;
}

void check_in(int client_index, int room_index){
    struct Client *guest = &clients[client_index];
    guest->room_num = room_index;
    guest->check_in_time = current_time;
    guest->state = ASSIGNED;
    guest->work_pending = 1;
    rooms[room_index] = client_index;
    printf("Сутки %d: администратор назначил клиенту %d номер %d на %d суток.\n",
           current_time, client_index + 1, room_index + 1, guest->stay_days);
    queue_message(guest->connection, ROOM, current_time, room_index + 1, guest->stay_days);
    observe(ROOM, client_index + 1, room_index + 1);
}

void settle_queue(void){
    while(head < tail){
        int free_room = find_free_room(rooms, room_count);
        if(free_room == -1)
            break;
        check_in(queue[head++], free_room);
    }
}

void protocol_error(int index){
    if(connections[index].client_index >= 0){
        fprintf(stderr, "Ошибка: неверное сообщение от клиента %d.\n", connections[index].client_index + 1);
        failed = 1;
    }
    else{
        queue_message(index, ERROR_MESSAGE, current_time, 1, 0);
        connections[index].closing = 1;
    }
}

void handle_message(int index, struct Message message){
    struct Connection *connection = &connections[index];
    if(index == observer){
        protocol_error(index);
        return;
    }
    if(connection->client_index == -1){
        if(message.type == WATCH && message.day == 0 && message.value == 0 && message.extra == 0){
            if(started || observer != -1){
                queue_message(index, ERROR_MESSAGE, current_time, 3, 0);
                connection->closing = 1;
                return;
            }
            observer = index;
            connection->client_index = -2;
            queue_message(index, WATCH_READY, reception_end, room_count, client_count);
            for(int i = 0; i < client_count; ++i){
                if(clients[i].connection != -1)
                    observe(HELLO, i + 1, 0);
            }
            printf("Наблюдатель подключён.\n");
            return;
        }
        if(message.type != HELLO || message.day != 0 || message.extra != 0 ||
           message.value < 1 || message.value > (uint32_t)client_count){
            protocol_error(index);
            return;
        }
        int guest = (int)message.value - 1;
        if(started || clients[guest].connection != -1){
            queue_message(index, ERROR_MESSAGE, current_time, 2, 0);
            connection->closing = 1;
            return;
        }
        connection->client_index = guest;
        clients[guest].connection = index;
        ++joined_count;
        printf("Подключился клиент %d. Подключено: %d из %d.\n", guest + 1, joined_count, client_count);
        queue_message(index, WELCOME, clients[guest].arrival_time, clients[guest].stay_days, reception_end);
        observe(HELLO, guest + 1, 0);
        return;
    }

    int client_index = connection->client_index;
    struct Client *guest = &clients[client_index];
    if(!started || message.day != (uint32_t)current_time || guest->work_pending == 0){
        protocol_error(index);
        return;
    }
    if(message.type == READY &&
       ((guest->state == NOT_ARRIVED && current_time < guest->arrival_time) || guest->state == WAITING) &&
       message.value == 0 && message.extra == 0){
        guest->work_pending = 0;
    }
    else if(message.type == REQUEST && guest->state == NOT_ARRIVED &&
            current_time == guest->arrival_time && current_time < reception_end &&
            message.value == (uint32_t)guest->stay_days && message.extra == 0){
        printf("Сутки %d: получен запрос клиента %d на %d суток.\n", current_time, client_index + 1, guest->stay_days);
        observe(REQUEST, client_index + 1, guest->stay_days);
        queue[tail++] = client_index;
        guest->state = WAITING;
        guest->work_pending = 0;
        printf("Сутки %d: клиент %d поставлен в очередь.\n", current_time, client_index + 1);
        queue_message(index, QUEUED, current_time, tail - head, 0);
        observe(QUEUED, client_index + 1, tail - head);
    }
    else if(message.type == CHECKED_IN && guest->state == ASSIGNED &&
            message.value == (uint32_t)(guest->room_num + 1) && message.extra == (uint32_t)guest->stay_days){
        guest->state = LIVING;
        guest->work_pending = 0;
        observe(CHECKED_IN, client_index + 1, guest->room_num + 1);
    }
    else if(message.type == PROGRESS && guest->state == LIVING && guest->work_pending == 1 &&
            message.value == (uint32_t)(current_time - guest->check_in_time) &&
            message.extra == (uint32_t)guest->stay_days){
        guest->work_pending = message.value == (uint32_t)guest->stay_days ? 2 : 0;
        observe(PROGRESS, client_index + 1, (int)message.value);
    }
    else if(message.type == CHECKOUT && guest->state == LIVING && guest->work_pending == 2 &&
            message.value == (uint32_t)(guest->room_num + 1) && message.extra == 0){
        int room = guest->room_num;
        rooms[room] = -1;
        guest->room_num = -1;
        guest->state = LEFT;
        guest->work_pending = 0;
        printf("Сутки %d: получен выезд клиента %d, номер %d освобождён.\n", current_time, client_index + 1, room + 1);
        queue_message(index, DEPARTED, current_time, room + 1, 0);
        observe(CHECKOUT, client_index + 1, room + 1);
        connection->closing = 1;
    }
    else
        protocol_error(index);
}

void write_connection(int index){
    struct Connection *connection = &connections[index];
    while(connection->sent < connection->output_size){
        ssize_t count = send(connection->fd, connection->output + connection->sent,
                             connection->output_size - connection->sent, 0);
        if(count > 0)
            connection->sent += (size_t)count;
        else if(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        else{
            connection_lost(index);
            return;
        }
    }
    connection->sent = connection->output_size = 0;
    if(connection->closing)
        close_connection(index);
}

void read_connection(int index){
    struct Connection *connection = &connections[index];
    // сообщение TCP может прийти по частям или вместе со следующим
    for(int part = 0; part < 32 && !failed && !connection->closing; ++part){
        ssize_t count = recv(connection->fd, connection->input + connection->received,
                             MESSAGE_SIZE - connection->received, 0);
        if(count > 0){
            connection->received += (size_t)count;
            if(connection->received == MESSAGE_SIZE){
                connection->received = 0;
                handle_message(index, decode_message(connection->input));
            }
        }
        else if(count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        else{
            connection_lost(index);
            return;
        }
    }
}

void accept_connections(int listener){
    for(int count = 0; count < MAX_CONNECTIONS; ++count){
        int fd = accept(listener, NULL, NULL);
        if(fd == -1){
            if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR){
                perror("accept");
                failed = 1;
            }
            return;
        }
        int index = 0;
        while(index < MAX_CONNECTIONS && connections[index].fd != -1)
            ++index;
        if(index == MAX_CONNECTIONS || !prepare_socket(fd)){
            close(fd);
            continue;
        }
        connections[index] = (struct Connection){.fd = fd, .client_index = -1,
                                                .connected_at = current_seconds()};
    }
}

void service_network(int listener, int timeout){
    struct pollfd sockets[MAX_CONNECTIONS + 1];
    sockets[0] = (struct pollfd){listener, POLLIN, 0};
    for(int i = 0; i < MAX_CONNECTIONS; ++i){
        struct Connection *connection = &connections[i];
        short events = connection->closing ? 0 : POLLIN;
        if(connection->sent < connection->output_size)
            events |= POLLOUT;
        sockets[i + 1] = (struct pollfd){connection->fd, events, 0};
    }
    int result = poll(sockets, MAX_CONNECTIONS + 1, timeout);
    if(result == -1){
        if(errno != EINTR){
            perror("poll");
            failed = 1;
        }
        return;
    }
    for(int i = 0; i < MAX_CONNECTIONS; ++i){
        short events = sockets[i + 1].revents;
        if(events & POLLOUT)
            write_connection(i);
        if(connections[i].fd != -1 && !connections[i].closing &&
           (events & (POLLIN | POLLHUP)))
            read_connection(i);
        if(connections[i].fd != -1 && (events & (POLLERR | POLLNVAL)))
            connection_lost(i);
        if(connections[i].fd != -1 && connections[i].client_index == -1 &&
           current_seconds() - connections[i].connected_at >= NETWORK_TIMEOUT)
            close_connection(i);
    }
    if(listener != -1 && (sockets[0].revents & POLLIN))
        accept_connections(listener);
}

int output_pending(int include_observer){
    for(int i = 0; i < MAX_CONNECTIONS; ++i){
        if((include_observer || i != observer) && connections[i].fd != -1 && connections[i].output_size != 0)
            return 1;
    }
    return 0;
}

int work_pending(void){
    for(int i = 0; i < client_count; ++i){
        if(clients[i].work_pending)
            return 1;
    }
    return 0;
}

int all_finished(void){
    for(int i = 0; i < client_count; ++i){
        if(clients[i].state != LEFT && clients[i].state != NOT_ACCEPTED)
            return 0;
    }
    return 1;
}

void start_day(void){
    observe(TICK, 0, 0);
    if(current_time == reception_end){
        printf("Сутки %d: приём закрыт, очередь продолжает обслуживаться.\n", current_time);
        observe(RECEPTION_CLOSED, 0, 0);
    }
    for(int i = 0; i < client_count; ++i){
        struct Client *guest = &clients[i];
        if(guest->state == NOT_ARRIVED && current_time >= reception_end){
            guest->state = NOT_ACCEPTED;
            queue_message(guest->connection, REJECTED, current_time, guest->arrival_time, 0);
            connections[guest->connection].closing = 1;
            printf("Сутки %d: клиент %d не принят, приём закрыт.\n", current_time, i + 1);
            observe(REJECTED, i + 1, guest->arrival_time);
        }
        else if(guest->state == NOT_ARRIVED || guest->state == LIVING || guest->state == WAITING){
            guest->work_pending = 1;
            queue_message(guest->connection, TICK, current_time, 0, 0);
        }
    }
}

int read_settings(void){
    int arrival_min, arrival_max;
    printf("Гостиница: сервер-администратор. Стратегия FIFO.\n");
    printf("Вводите каждое число с новой строки.\n");
    if(!read_int("Количество номеров: ", 1, MAX_ROOMS, &room_count) ||
       !read_int("Количество клиентов: ", 1, MAX_CLIENTS, &client_count) ||
       !read_int("Минимальный интервал прибытия: ", 0, MAX_DAYS, &arrival_min) ||
       !read_int("Максимальный интервал прибытия: ", arrival_min, MAX_DAYS, &arrival_max) ||
       !read_int("Момент прекращения приёма: ", 1, MAX_DAYS, &reception_end) ||
       !read_int("Задержка между условными сутками (0-5 секунд): ", 0, 5, &delay_seconds))
        return 0;
    for(int i = 0; i < room_count; ++i)
        rooms[i] = -1;
    srand(1);
    int last_arrival = 0;
    for(int i = 0; i < client_count; ++i){
        printf("Клиент %d. ", i + 1);
        if(!read_int("Срок проживания: ", 1, MAX_DAYS, &clients[i].stay_days))
            return 0;
        last_arrival += arrival_min + rand() % (arrival_max - arrival_min + 1);
        clients[i].arrival_time = last_arrival;
        clients[i].check_in_time = clients[i].room_num = clients[i].connection = -1;
    }
    printf("\n");
    return 1;
}

int main(int argc, char *argv[]){
    if(argc < 3 || argc > 4){
        fprintf(stderr, "Использование: %s IP порт [ожидание_подключения_в_секундах]\n", argv[0]);
        return 1;
    }
    struct sockaddr_in address;
    int startup_timeout = 60;
    if(!prepare_program() || !make_address(argv[1], argv[2], &address) ||
       (argc == 4 && !parse_int(argv[3], 1, 300, &startup_timeout)))
        return 1;
    if(!read_settings())
        return stop_requested ? 128 + stop_requested : 1;
    for(int i = 0; i < MAX_CONNECTIONS; ++i)
        connections[i].fd = -1;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if(listener == -1){
        perror("socket");
        return 1;
    }
    int enabled = 1;
    if(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1 ||
       !prepare_socket(listener) || bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
       listen(listener, MAX_CONNECTIONS) == -1){
        perror("Запуск сервера");
        close(listener);
        return 1;
    }
    printf("Сервер готов: %s:%s. Ожидание %d клиентов: %d секунд.\n", argv[1], argv[2], client_count, startup_timeout);
    double startup_deadline = current_seconds() + startup_timeout;
    double day_deadline = 0, next_day = 0;
    int day_complete = 0;
    while(!failed && !stop_requested){
        double now = current_seconds();
        if(!started && joined_count == client_count){
            started = 1;
            printf("Все клиенты подключились. Моделирование началось.\n");
            start_day();
            day_deadline = now + NETWORK_TIMEOUT;
        }
        if(!started && now >= startup_deadline){
            fprintf(stderr, "Ошибка: подключились не все клиенты.\n");
            failed = 1;
            break;
        }
        if(started){
            if(!work_pending())
                settle_queue();
            if(!work_pending() && !output_pending(0)){
                if(all_finished())
                    break;
                if(!day_complete){
                    day_complete = 1;
                    next_day = now + delay_seconds;
                }
                if(now >= next_day){
                    ++current_time;
                    day_complete = 0;
                    start_day();
                    day_deadline = now + NETWORK_TIMEOUT;
                }
            }
            if(!day_complete && now >= day_deadline){
                fprintf(stderr, "Ошибка: клиент не ответил за 10 секунд.\n");
                failed = 1;
                break;
            }
        }
        service_network(listener, day_complete || !started ? 50 : 100);
    }
    close(listener);
    if(failed || stop_requested){
        for(int i = 0; i < MAX_CONNECTIONS; ++i){
            if(connections[i].fd != -1 && !connections[i].closing){
                queue_message(i, STOP, current_time, stop_requested, failed);
                connections[i].closing = 1;
            }
        }
        printf("Моделирование остановлено на сутках: %d.\n", current_time);
    }
    else{
        printf("Обслуживание завершено на сутках: %d.\n", current_time);
        int completed = 0;
        for(int i = 0; i < client_count; ++i)
            completed += clients[i].state == LEFT;
        observe(FINISHED, completed, client_count - completed);
        if(observer != -1)
            connections[observer].closing = 1;
    }
    double deadline = current_seconds() + 2;
    while(output_pending(1) && current_seconds() < deadline)
        service_network(-1, 50);
    for(int i = 0; i < MAX_CONNECTIONS; ++i)
        close_connection(i);
    return failed ? 1 : (stop_requested ? 128 + stop_requested : 0);
}
