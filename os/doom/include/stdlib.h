#ifndef SURA_STDLIB_H
#define SURA_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*func)(void));

int atoi(const char *s);
double atof(const char *s);
long strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

int abs(int v);
long labs(long v);

int rand(void);
void srand(unsigned int seed);
#define RAND_MAX 0x7fffffff

char *getenv(const char *name);
int system(const char *cmd);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif
