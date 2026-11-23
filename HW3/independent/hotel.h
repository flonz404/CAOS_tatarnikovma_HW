#ifndef HOTEL_SHARED_H
#define HOTEL_SHARED_H

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MAX_ROOMS 100
#define MAX_CLIENTS 100
#define MAX_DAYS 10000
#define ADMIN_EVENT MAX_CLIENTS
#define DAY_DONE (MAX_CLIENTS + 1)
#define JOIN_EVENT (MAX_CLIENTS + 2)
#define ROOM_LOCK (MAX_CLIENTS + 3)
#define SEM_COUNT (MAX_CLIENTS + 4)
#define MAX_MESSAGES 1024
#define SESSION_LENGTH 12
#define MEMORY_VERSION 1

enum ClientStatus {
    NOT_ARRIVED,
    WAITING,
    ASSIGNED,
    LIVING,
    LEFT,
    REJECTED
};

struct Client {
    int arrival_time;          // время прибытия
    int stay_days;             // срок проживания
    int check_in_time;         // время заселения
    int room_num;              // индекс номера, -1 если не назначен
    enum ClientStatus state;
    int work_pending;          // есть необработанное событие
};

// здесь нет указателей: каждый процесс подключает память по своему адресу
struct SharedState {
    int version;
    pid_t manager_pid;
    struct Client clients[MAX_CLIENTS];
    int rooms[MAX_ROOMS];
    int queue[MAX_CLIENTS];
    int room_count, client_count;
    int head, tail;
    int current_time;
    int accepted, completed;
    int started, stopping;
    int admin_pending, pending_work;
    pid_t participants[MAX_CLIENTS + 1];
    int finished[MAX_CLIENTS + 1];
    int joined_count;
    int message_count;
    char messages[MAX_MESSAGES][256];
};

extern struct SharedState *hotel;
extern sem_t *events[SEM_COUNT];
extern char session[SESSION_LENGTH + 1];
extern int is_manager, registered, role_index, failed;
extern pid_t manager_pid;
extern volatile sig_atomic_t stop_requested, failure_requested;

int prepare_program(const char *name);
int parse_int(const char *text, int min, int max, int *value);
int read_int(const char *text, int min, int max, int *value);
double current_seconds(void);
void ipc_error(const char *text);
int create_memory(void);
int create_events(void);
int connect_hotel(int index);
void close_resources(void);
int finish_participant(void);
int wait_event(sem_t *event, double timeout, int interruptible);
void post_event(sem_t *event);
void log_event(const char *format, ...);
void flush_events(void);
void wake_client(int client_index);
void wake_admin(void);
void finish_work(void);
int find_free_room(int rooms[], int room_count);
void check_in(int client_index, int room_index);
int write_results(int fd);

#endif
