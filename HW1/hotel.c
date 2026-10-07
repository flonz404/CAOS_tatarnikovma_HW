#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_ROOMS 100
#define MAX_CLIENTS 100
#define MAX_DAYS 10000

enum ClientStatus {
    NOT_ARRIVED,
    WAITING,
    LIVING,
    LEFT
};

struct Client {
    int arrival_time;          // Момент прибытия клиента
    int stay_days;             // Заказанный срок проживания
    int check_in_time;         // Момент фактического заселения
    int room_num;              // Индекс номера, -1 - отсутствие номера
    enum ClientStatus state;  // Текущее состояние клиента
};

//вспомогательная функция считывания числа с проверкой диапазона и корректности введенных символов.
int read_int(const char *text, int min, int max, int *value)
{
    char line[128];
    char *end;

    printf("%s", text);

    if (scanf("%127s", line) != 1) {
        printf("Ошибка чтения\n");
        return 0;
    }

    long number = strtol(line, &end, 10);

    if (end == line || *end != '\0' ||
        number < min || number > max) {
        printf("Ошибка: нужно целое число от %d до %d.\n", min, max);
        return 0;
    }

    *value = (int)number;
    return 1;
}

//функция поиска свободной комнаты (возвращает -1 при отсутствии таковой)
int find_free_room(int rooms[], int room_count){
    for(int i = 0; i < room_count; ++i){
        if(rooms[i] == -1)
            return i;
    }
    return -1;
}

//заселение client_index клиента в room_index комнату на current_time сутки)
void check_in(struct Client clients[], int rooms[], int client_index,
        int room_index, int current_time){
    clients[client_index].room_num = room_index;
    clients[client_index].check_in_time = current_time;
    clients[client_index].state = LIVING;
    rooms[room_index] = client_index;
    printf("Администратор заселил гостя №%d в номер %d на %d суток.\n",
           client_index + 1, room_index + 1,
           clients[client_index].stay_days);
}

//основной блок
int main(void)
{
    int room_count, client_count;
    int arrival_min, arrival_max;
    int reception_end;
    int delay_seconds;

    printf("Вводите каждое число с новой строки.\n");
    printf("Номеров и клиентов: от 1 до 100. Время: от 1 до %d суток.\n", MAX_DAYS);

    //считываем все данные с помощью безопасной функции
    if (!read_int("Количество номеров: ", 1, MAX_ROOMS, &room_count)) {
        return 1;
    }
    if (!read_int("Количество клиентов: ", 1, MAX_CLIENTS, &client_count)) {
        return 1;
    }
    if (!read_int("Минимальный интервал прибытия: ", 0, MAX_DAYS, &arrival_min)) {
        return 1;
    }
    if (!read_int("Максимальный интервал прибытия: ", arrival_min, MAX_DAYS, &arrival_max)) {
        return 1;
    }
    if (!read_int("Момент прекращения приёма: ", 1, MAX_DAYS, &reception_end)) {
        return 1;
    }
    if(!read_int("Задержка между условными сутками: ", 0, 5, &delay_seconds)){
        return 1;
    }

    struct Client clients[MAX_CLIENTS] = {0};
    int rooms[MAX_ROOMS];

    // В начале все номера свободны, ни один клиент ещё не прибыл.
    for (int i = 0; i < room_count; ++i) {
        rooms[i] = -1;
    }

    //записываем известные сведения о клиентах
    for (int i = 0; i < client_count; ++i) {
        clients[i].arrival_time = -1;
        clients[i].room_num = -1;
        clients[i].state = NOT_ARRIVED;
        clients[i].check_in_time = -1;

        if (!read_int("Срок проживания: ", 1, MAX_DAYS, &clients[i].stay_days)) {
            return 1;
        }
    }

    int queue[MAX_CLIENTS] = {0};
    int head = 0;
    int tail = 0;

    srand(1); //запускаем генератор рандомных чиел для интервалов

    int last_arrival = 0;
    //считаем инетрвалы для каждого
    for(int i = 0; i < client_count; ++i){
        last_arrival += arrival_min + (rand() % (arrival_max - arrival_min + 1));
        clients[i].arrival_time = last_arrival;
    }

    int current_time = 0;
    int next_client = 0;

    //основной цикл - проход по времени + заселение уже пришедших после конца смены
    while (1) {
        printf("\nСутки %d\n", current_time);
        //выселение завершивших проживание клиентов
        for(int room = 0; room < room_count; ++room){
            int client_index = rooms[room];
            if(client_index == -1)
                continue;
            int days_lived = current_time - clients[client_index].check_in_time;
            printf("Клиент %d завершил %d суток проживания из %d\n", client_index + 1, days_lived, clients[client_index].stay_days);
            if(days_lived == clients[client_index].stay_days){
                rooms[room] = -1;
                clients[client_index].room_num = -1;
                clients[client_index].state = LEFT;
                printf("Клиент %d выехал с %d комнаты\n", client_index + 1, room + 1);
            }
        }
        //заселение людей из очереди
        while(head < tail){
            int free_room = find_free_room(rooms, room_count);
            if(free_room == -1)
                break;
            int client_index = queue[head];
            check_in(clients, rooms, client_index, free_room, current_time);
            printf("Заселили клиента %d из очереди в номер %d\n", client_index + 1, free_room + 1);
            ++head;
        }
        //заселение пришедших
        while (current_time < reception_end &&
               next_client < client_count &&
               clients[next_client].arrival_time == current_time) {
            printf("Прибыл клиент %d и запрашивает номер на %d суток.\n", next_client + 1, clients[next_client].stay_days);
            int free_room = find_free_room(rooms, room_count);
            if (free_room != -1){
                check_in(clients, rooms, next_client, free_room, current_time);
            }
            else{
                queue[tail] = next_client;
                ++tail;
                clients[next_client].state = WAITING;
                printf("Клиент №%d ожидает заселения в очереди\n", next_client + 1);
            }
            ++next_client;
        }
        int occupied = 0;
        for(int room = 0; room < room_count; ++room){
            if(rooms[room] != -1){
                ++occupied;
            }
        }

        if((current_time >= reception_end || next_client == client_count) && occupied == 0 && head == tail)
            break;

        sleep(delay_seconds);
        ++current_time;
    }

    printf("\nМоделирование завершено на сутках: %d\n", current_time);
    printf("Всего приняли клиентов: %d\n", next_client);
    printf("Не успели принять клиентов: %d\n", client_count - next_client);

    int log_fd = creat("hotel.log", 0644);

    if (log_fd == -1) {
        perror("Не удалось создать hotel.log");
        return 1;
    }

    if(dprintf(log_fd, "Моделирование завершено на сутках: %d\n", current_time) < 0){
        perror("Ошибка записи лога");
        close(log_fd);
        return 1;
    }
    if(dprintf(log_fd, "Всего приняли клиентов: %d.\n", next_client) < 0){
        perror("Ошибка записи лога");
        close(log_fd);
        return 1;
    }
    if(dprintf(log_fd, "Не успели принять клиентов: %d.\n", client_count - next_client) < 0){
        perror("Ошибка записи лога");
        close(log_fd);
        return 1;
    }
    if(close(log_fd) == -1){
        perror("Не удалось закрыть hotel.log");
        return 1;
    }

    return 0;
}
