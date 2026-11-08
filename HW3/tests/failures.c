#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdarg.h>
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
