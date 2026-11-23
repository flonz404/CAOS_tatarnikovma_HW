#include "hotel.h"

void administrator(void){
    while(!failed && !stop_requested){
        if(!wait_event(events[ADMIN_EVENT], 0, 1) ||
           !wait_event(events[ROOM_LOCK], 5, 1))
            break;
        if(hotel->stopping){
            post_event(events[ROOM_LOCK]);
            break;
        }
        while(hotel->head < hotel->tail){
            int free_room = find_free_room(hotel->rooms, hotel->room_count);
            if(free_room == -1)
                break;
            int client_index = hotel->queue[hotel->head++];
            check_in(client_index, free_room);
        }
        hotel->admin_pending = 0;
        finish_work();
        post_event(events[ROOM_LOCK]);
    }
}

int main(int argc, char *argv[]){
    if(argc != 2){
        fprintf(stderr, "Использование: %s имя_гостиницы\n", argv[0]);
        return 1;
    }
    if(!prepare_program(argv[1]))
        return 1;
    if(connect_hotel(ADMIN_EVENT)){
        printf("Администратор подключён к гостинице %s.\n", session);
        administrator();
    }
    else if(!stop_requested)
        failed = 1;
    return finish_participant();
}
