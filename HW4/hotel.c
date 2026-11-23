#include "hotel.h"

volatile sig_atomic_t stop_requested = 0;

void handle_signal(int signal_number){
    stop_requested = signal_number;
}

int prepare_program(void){
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT, &action, NULL) == -1 ||
       sigaction(SIGTERM, &action, NULL) == -1){
        perror("sigaction");
        return 0;
    }
    action.sa_handler = SIG_IGN;
    if(sigaction(SIGPIPE, &action, NULL) == -1){
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
            fprintf(stderr, "Ошибка чтения.\n");
        return 0;
    }
    int next = getchar();
    if(next != EOF && !isspace((unsigned char)next)){
        fprintf(stderr, "Ошибка: слишком длинное число.\n");
        return 0;
    }
    return parse_int(line, min, max, value) && !stop_requested;
}

double current_seconds(void){
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC, &now) == -1){
        perror("clock_gettime");
        exit(1);
    }
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

int make_address(const char *ip, const char *port, struct sockaddr_in *address){
    int number;
    if(!parse_int(port, 1, 65535, &number))
        return 0;
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)number);
    if(inet_pton(AF_INET, ip, &address->sin_addr) != 1){
        fprintf(stderr, "Ошибка: нужен IPv4-адрес, например 127.0.0.1.\n");
        return 0;
    }
    return 1;
}

int prepare_socket(int fd){
    int flags = fcntl(fd, F_GETFL);
    int enabled = 1;
    if(flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1 ||
       setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == -1){
        perror("Настройка сокета");
        return 0;
    }
    return 1;
}

void encode_message(unsigned char data[MESSAGE_SIZE], struct Message message){
    // передаём числа в сетевом порядке байтов, без выравнивания структуры
    uint32_t fields[4] = {htonl(message.type), htonl(message.day),
                          htonl(message.value), htonl(message.extra)};
    memcpy(data, fields, MESSAGE_SIZE);
}

struct Message decode_message(const unsigned char data[MESSAGE_SIZE]){
    uint32_t fields[4];
    memcpy(fields, data, MESSAGE_SIZE);
    return (struct Message){ntohl(fields[0]), ntohl(fields[1]),
                            ntohl(fields[2]), ntohl(fields[3])};
}

int wait_socket(int fd, short events, double deadline){
    while(!stop_requested){
        double remaining = deadline - current_seconds();
        if(remaining <= 0){
            fprintf(stderr, "Ошибка: истекло время ожидания сети.\n");
            return 0;
        }
        struct pollfd socket_event = {fd, events, 0};
        int result = poll(&socket_event, 1, remaining > 0.1 ? 100 : 1);
        if(result > 0)
            return 1;
        if(result == -1 && errno != EINTR){
            perror("poll");
            return 0;
        }
    }
    return 0;
}

int transfer_message(int fd, unsigned char data[MESSAGE_SIZE], int sending){
    size_t done = 0;
    double deadline = current_seconds() + (sending ? NETWORK_TIMEOUT : 360);
    while(done < MESSAGE_SIZE && !stop_requested){
        if(!wait_socket(fd, sending ? POLLOUT : POLLIN, deadline))
            return 0;
        ssize_t count = sending ? send(fd, data + done, MESSAGE_SIZE - done, 0) :
                                  recv(fd, data + done, MESSAGE_SIZE - done, 0);
        if(count > 0){
            // после первого байта ждём остаток сообщения не более 10 секунд
            if(done == 0 && !sending)
                deadline = current_seconds() + NETWORK_TIMEOUT;
            done += (size_t)count;
        }
        else if(count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)){
            fprintf(stderr, "Ошибка: связь с сервером потеряна.\n");
            return 0;
        }
    }
    return done == MESSAGE_SIZE;
}

int send_message(int fd, struct Message message){
    unsigned char data[MESSAGE_SIZE];
    encode_message(data, message);
    return transfer_message(fd, data, 1);
}

int receive_message(int fd, struct Message *message){
    unsigned char data[MESSAGE_SIZE];
    if(!transfer_message(fd, data, 0))
        return 0;
    *message = decode_message(data);
    return 1;
}
