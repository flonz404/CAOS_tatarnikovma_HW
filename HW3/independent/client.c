#include "hotel.h"

void client(int client_index){
    struct Client *guest = &hotel->clients[client_index];
    while(!failed && !stop_requested){
        if(!wait_event(events[client_index], 0, 1) ||
           !wait_event(events[ROOM_LOCK], 5, 1))
            break;
        if(hotel->stopping){
            post_event(events[ROOM_LOCK]);
            break;
        }
        if(guest->state == NOT_ARRIVED){
            log_event("Прибыл клиент %d и запрашивает номер на %d суток.\n",
                      client_index + 1, guest->stay_days);
            hotel->queue[hotel->tail++] = client_index;
            ++hotel->accepted;
            guest->state = WAITING;
            log_event("Клиент %d поставлен в очередь.\n", client_index + 1);
            wake_admin();
        }
        else if(guest->state == ASSIGNED){
            guest->state = LIVING;
            log_event("Клиент %d заселился в номер %d на %d суток.\n",
                      client_index + 1, guest->room_num + 1, guest->stay_days);
        }
        else if(guest->state == LIVING){
            int days_lived = hotel->current_time - guest->check_in_time;
            log_event("Клиент %d завершил %d суток проживания из %d.\n",
                      client_index + 1, days_lived, guest->stay_days);
            if(days_lived == guest->stay_days){
                log_event("Клиент %d выехал из номера %d.\n", client_index + 1, guest->room_num + 1);
                hotel->rooms[guest->room_num] = -1;
                guest->room_num = -1;
                guest->state = LEFT;
                ++hotel->completed;
                wake_admin();
            }
        }
        else if(guest->state == REJECTED){
            log_event("Клиент %d не принят: прибытие на сутках %d, приём закрыт.\n",
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

int main(int argc, char *argv[]){
    if(argc != 3){
        fprintf(stderr, "Использование: %s имя_гостиницы номер_клиента\n", argv[0]);
        return 1;
    }
    int number;
    if(!prepare_program(argv[1]) || !parse_int(argv[2], 1, MAX_CLIENTS, &number))
        return 1;
    if(connect_hotel(number - 1)){
        printf("Клиент %d подключён к гостинице %s.\n", number, session);
        client(number - 1);
    }
    else if(!stop_requested)
        failed = 1;
    return finish_participant();
}
