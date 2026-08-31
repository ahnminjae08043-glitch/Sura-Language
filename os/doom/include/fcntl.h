#ifndef SURA_FCNTL_H
#define SURA_FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_NONBLOCK 04000

int open(const char *path, int flags, ...);
int close(int fd);

#endif
