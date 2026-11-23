#ifndef HOTEL_NETWORK_H
#define HOTEL_NETWORK_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#define MAX_ROOMS 100
#define MAX_CLIENTS 100
#define MAX_DAYS 10000
#define MESSAGE_SIZE 16
#define NETWORK_TIMEOUT 10

enum MessageType {
    HELLO = 1,
    WELCOME,
    TICK,
    READY,
    REQUEST,
    QUEUED,
    ROOM,
    CHECKED_IN,
    PROGRESS,
    CHECKOUT,
    DEPARTED,
    REJECTED,
    STOP,
    ERROR_MESSAGE
};

struct Message {
    uint32_t type, day, value, extra;
};

extern volatile sig_atomic_t stop_requested;

int prepare_program(void);
int parse_int(const char *text, int min, int max, int *value);
int read_int(const char *text, int min, int max, int *value);
double current_seconds(void);
int make_address(const char *ip, const char *port, struct sockaddr_in *address);
int prepare_socket(int fd);
void encode_message(unsigned char data[MESSAGE_SIZE], struct Message message);
struct Message decode_message(const unsigned char data[MESSAGE_SIZE]);
int wait_socket(int fd, short events, double deadline);
int send_message(int fd, struct Message message);
int receive_message(int fd, struct Message *message);

#endif
