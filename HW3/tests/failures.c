#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Этот файл линкуется только с тестовой копией программы.
static int selected(const char *name){
    const char *value = getenv("HOTEL_TEST_FAILURE");
    return value != NULL && strcmp(value, name) == 0;
}

pid_t test_fork(void){
    static int calls = 0;
    if(++calls == 3 && selected("fork")){
        errno = EAGAIN;
        return -1;
    }
    return fork();
}

sem_t *test_sem_open(const char *name, int flags, ...){
    static int calls = 0;
    if(++calls == 3 && selected("sem_open")){
        errno = ENOSPC;
        return SEM_FAILED;
    }
    if(flags & O_CREAT){
        va_list args;
        va_start(args, flags);
        // В программе права передаются целочисленной константой 0600.
        int mode = va_arg(args, int);
        unsigned int value = va_arg(args, unsigned int);
        va_end(args);
        return sem_open(name, flags, mode, value);
    }
    return sem_open(name, flags);
}

int test_ftruncate(int fd, off_t length){
    if(selected("ftruncate")){
        errno = ENOSPC;
        return -1;
    }
    return ftruncate(fd, length);
}

void *test_mmap(void *address, size_t length, int protection, int flags, int fd, off_t offset){
    if(selected("mmap")){
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return mmap(address, length, protection, flags, fd, offset);
}

#ifdef __linux__
// Сохраняем адреса, чтобы проверить порядок освобождения неименованных семафоров.
static sem_t *initialized[103];
static int initialized_count = 0;
static pid_t semaphore_owner;

int test_sem_init(sem_t *event, int pshared, unsigned int value){
    static int calls = 0;
    if(++calls == 3 && selected("sem_init")){
        errno = ENOSPC;
        return -1;
    }
    if(pshared != 1 || initialized_count == 103){
        fprintf(stderr, "Ошибка проверки семафоров: неверная инициализация.\n");
        errno = EINVAL;
        return -1;
    }
    int result = sem_init(event, pshared, value);
    if(result == 0){
        semaphore_owner = getpid();
        initialized[initialized_count++] = event;
    }
    return result;
}

int test_sem_destroy(sem_t *event){
    for(int i = 0; i < initialized_count && getpid() == semaphore_owner; ++i){
        if(initialized[i] == event){
            int result = sem_destroy(event);
            if(result == 0)
                initialized[i] = initialized[--initialized_count];
            return result;
        }
    }
    fprintf(stderr, "Ошибка проверки семафоров: уничтожение чужого или не созданного семафора.\n");
    errno = EINVAL;
    return -1;
}

int test_munmap(void *address, size_t length){
    if(getpid() == semaphore_owner && initialized_count != 0){
        fprintf(stderr, "Ошибка проверки семафоров: память освобождается до sem_destroy.\n");
        errno = EBUSY;
        return -1;
    }
    return munmap(address, length);
}
#endif
