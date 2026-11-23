#include "hotel.h"

int watch_hotel(int fd){
    if(!send_message(fd, (struct Message){WATCH, 0, 0, 0}))
        return 0;
    struct Message message;
    if(!receive_message(fd, &message))
        return 0;
    if(message.type != WATCH_READY || message.value < 1 || message.value > MAX_ROOMS ||
       message.extra < 1 || message.extra > MAX_CLIENTS){
        fprintf(stderr, "Наблюдатель не подключён. Подключите одного наблюдателя до начала моделирования.\n");
        return 0;
    }
    int client_count = (int)message.extra;
    uint32_t stays[MAX_CLIENTS] = {0};
    printf("Наблюдатель подключён. Номеров: %u. Клиентов: %u. Приём до суток: %u.\n",
           message.value, message.extra, message.day);
    while(!stop_requested && receive_message(fd, &message)){
        uint32_t number = message.value, value = message.extra;
        if(message.type <= REJECTED && message.type != TICK &&
           (number < 1 || number > (uint32_t)client_count)){
            fprintf(stderr, "Ошибка: неверный номер клиента в событии.\n");
            return 0;
        }
        switch(message.type){
            case HELLO:
                printf("Подключился клиент %u.\n", number);
                break;
            case TICK:
                printf("\nСутки %u\n", message.day);
                break;
            case REQUEST:
                stays[number - 1] = value;
                printf("Клиент %u прибыл и запрашивает номер на %u суток.\n", number, value);
                break;
            case QUEUED:
                printf("Клиент %u поставлен в очередь, место при постановке: %u.\n", number, value);
                break;
            case ROOM:
                printf("Администратор назначил клиенту %u номер %u на %u суток.\n", number, value, stays[number - 1]);
                break;
            case CHECKED_IN:
                printf("Клиент %u заселился в номер %u.\n", number, value);
                break;
            case PROGRESS:
                printf("Клиент %u завершил %u суток проживания из %u.\n", number, value, stays[number - 1]);
                break;
            case CHECKOUT:
                printf("Клиент %u выехал, номер %u освобождён.\n", number, value);
                break;
            case REJECTED:
                printf("Клиент %u не принят: прибытие на сутках %u, приём закрыт.\n", number, value);
                break;
            case RECEPTION_CLOSED:
                printf("Приём закрыт. Очередь продолжает обслуживаться.\n");
                break;
            case FINISHED:
                printf("\nМоделирование завершено на сутках: %u.\n", message.day);
                printf("Завершили проживание: %u. Не приняты: %u.\n", number, value);
                return 1;
            case STOP:
                printf("Моделирование остановлено на сутках %u: %s.\n", message.day,
                       value ? "ошибка участника или сети" : "сигнал завершения");
                return 0;
            default:
                fprintf(stderr, "Ошибка: неожиданное сообщение сервера.\n");
                return 0;
        }
    }
    return 0;
}

int main(int argc, char *argv[]){
    if(argc != 3){
        fprintf(stderr, "Использование: %s IP_сервера порт\n", argv[0]);
        return 1;
    }
    struct sockaddr_in address;
    if(!prepare_program() || !make_address(argv[1], argv[2], &address))
        return 1;
    int fd = connect_server(&address);
    if(fd == -1)
        return stop_requested ? 128 + stop_requested : 1;
    int ok = watch_hotel(fd);
    close(fd);
    return stop_requested ? 128 + stop_requested : (ok ? 0 : 1);
}
