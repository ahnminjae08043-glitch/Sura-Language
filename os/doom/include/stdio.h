#ifndef SURA_STDIO_H
#define SURA_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct FILE FILE;

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int fflush(FILE *f);
int feof(FILE *f);
int remove(const char *path);
int rename(const char *oldp, const char *newp);
char *fgets(char *s, int size, FILE *f);
int fgetc(FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int puts(const char *s);
int putchar(int c);

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int sscanf(const char *str, const char *fmt, ...);
int fscanf(FILE *f, const char *fmt, ...);

#endif
