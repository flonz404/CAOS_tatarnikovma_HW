#include "hotel.h"

enum ClientStatus { NOT_ARRIVED, REQUEST_SENT, WAITING, LIVING, LEAVING };

int client(int fd, int number){
    if(!send_message(fd, (struct Message){HELLO, 0, (uint32_t)number, 0}))
        return 0;
    struct Message message;
    if(!receive_message(fd, &message))
        return 0;
    if(message.type == ERROR_MESSAGE){
        fprintf(stderr, "Сервер отклонил подключение: неверный или занятый номер клиента, либо модель уже запущена.\n");
        return 0;
    }
    if(message.type != WELCOME || message.day > MAX_CLIENTS * MAX_DAYS ||
       message.value < 1 || message.value > MAX_DAYS || message.extra < 1 || message.extra > MAX_DAYS){
        fprintf(stderr, "Ошибка: неверные начальные данные от сервера.\n");
        return 0;
    }
    int arrival_time = (int)message.day;
    int stay_days = (int)message.value;
    int reception_end = (int)message.extra;
    int check_in_time = -1, room_num = -1, current_time = -1;
    enum ClientStatus state = NOT_ARRIVED;
    printf("Клиент %d подключён. Прибытие: сутки %d. Проживание: %d суток.\n", number, arrival_time, stay_days);
    while(!stop_requested && receive_message(fd, &message)){
        if(message.type == STOP){
            fprintf(stderr, "Сервер остановил моделирование на сутках %u.\n", message.day);
            return 0;
        }
        if(message.type == TICK && message.day == (uint32_t)(current_time + 1) &&
           message.value == 0 && message.extra == 0 &&
           (state == NOT_ARRIVED || state == WAITING || state == LIVING)){
            current_time = (int)message.day;
            if(state == NOT_ARRIVED && current_time == arrival_time){
                printf("Сутки %d: клиент %d прибыл и запрашивает номер на %d суток.\n", current_time, number, stay_days);
                state = REQUEST_SENT;
                if(!send_message(fd, (struct Message){REQUEST, message.day, (uint32_t)stay_days, 0}))
                    return 0;
            }
            else if(state == LIVING){
                int days_lived = current_time - check_in_time;
                printf("Сутки %d: клиент %d завершил %d суток проживания из %d.\n", current_time, number, days_lived, stay_days);
                if(!send_message(fd, (struct Message){PROGRESS, message.day, (uint32_t)days_lived, (uint32_t)stay_days}))
                    return 0;
                if(days_lived == stay_days){
                    state = LEAVING;
                    if(!send_message(fd, (struct Message){CHECKOUT, message.day, (uint32_t)room_num, 0}))
                        return 0;
                }
            }
            else if(!send_message(fd, (struct Message){READY, message.day, 0, 0}))
                return 0;
        }
        else if(message.type == QUEUED && state == REQUEST_SENT && message.day == (uint32_t)current_time &&
                message.value >= 1 && message.value <= MAX_CLIENTS && message.extra == 0){
            state = WAITING;
            printf("Сутки %d: клиент %d ожидает заселения, место в очереди при постановке: %u.\n", current_time, number, message.value);
        }
        else if(message.type == ROOM && state == WAITING && message.day == (uint32_t)current_time &&
                message.value >= 1 && message.value <= MAX_ROOMS && message.extra == (uint32_t)stay_days){
            room_num = (int)message.value;
            check_in_time = current_time;
            state = LIVING;
            printf("Сутки %d: клиент %d получил назначение и заселился в номер %d на %d суток.\n",
                   current_time, number, room_num, stay_days);
            if(!send_message(fd, (struct Message){CHECKED_IN, message.day, message.value, message.extra}))
                return 0;
        }
        else if(message.type == DEPARTED && state == LEAVING && message.day == (uint32_t)current_time &&
                message.value == (uint32_t)room_num && message.extra == 0){
            printf("Сутки %d: клиент %d выехал из номера %d, сервер подтвердил выезд.\n", current_time, number, room_num);
            return 1;
        }
        else if(message.type == REJECTED && state == NOT_ARRIVED && message.day == (uint32_t)reception_end &&
                message.value == (uint32_t)arrival_time && arrival_time >= reception_end && message.extra == 0){
            printf("Сутки %u: клиент %d не принят, приём закрыт. Запланированное прибытие: сутки %d.\n",
                   message.day, number, arrival_time);
            return 1;
        }
        else{
            fprintf(stderr, "Ошибка: неожиданное сообщение сервера.\n");
            return 0;
        }
    }
    return 0;
}

int main(int argc, char *argv[]){
    if(argc != 4){
        fprintf(stderr, "Использование: %s IP_сервера порт номер_клиента\n", argv[0]);
        return 1;
    }
    struct sockaddr_in address;
    int number;
    if(!prepare_program() || !make_address(argv[1], argv[2], &address) ||
       !parse_int(argv[3], 1, MAX_CLIENTS, &number))
        return 1;
    int fd = connect_server(&address);
    if(fd == -1)
        return stop_requested ? 128 + stop_requested : 1;
    int ok = client(fd, number);
    close(fd);
    if(stop_requested)
        printf("Клиент %d остановлен пользователем.\n", number);
    return stop_requested ? 128 + stop_requested : (ok ? 0 : 1);
}
