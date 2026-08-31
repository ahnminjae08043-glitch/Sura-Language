/* Minimal printf family. Output goes to the Sura OS serial syscall. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sura_syscall.h"

FILE *stdout = (FILE *)1;
FILE *stderr = (FILE *)2;
FILE *stdin = (FILE *)0;

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} sura_out;

static void out_char(sura_out *o, char c)
{
    if (o->buf && o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_str(sura_out *o, const char *s, int precision)
{
    if (s == NULL) {
        s = "(null)";
    }
    while (*s && precision != 0) {
        out_char(o, *s++);
        if (precision > 0) {
            precision--;
        }
    }
}

static void out_num(sura_out *o, unsigned long long value, int base,
                    int is_negative, int width, int zero_pad, int upper)
{
    char tmp[32];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value) {
        tmp[n++] = digits[value % (unsigned)base];
        value /= (unsigned)base;
    }
    if (is_negative) {
        out_char(o, '-');
        if (width > 0) {
            width--;
        }
    }
    while (width > n) {
        out_char(o, zero_pad ? '0' : ' ');
        width--;
    }
    while (n) {
        out_char(o, tmp[--n]);
    }
}

static void out_double(sura_out *o, double value, int precision)
{
    long long whole;
    double frac;
    int i;
    if (precision < 0) {
        precision = 6;
    }
    if (value < 0) {
        out_char(o, '-');
        value = -value;
    }
    whole = (long long)value;
    frac = value - (double)whole;
    out_num(o, (unsigned long long)whole, 10, 0, 0, 0, 0);
    if (precision > 0) {
        out_char(o, '.');
        for (i = 0; i < precision; i++) {
            frac *= 10.0;
            out_char(o, (char)('0' + ((long long)frac % 10)));
        }
    }
}

static int format(sura_out *o, const char *fmt, va_list ap)
{
    while (*fmt) {
        char c = *fmt++;
        int width = 0;
        int zero_pad = 0;
        int precision = -1;
        int is_long = 0;
        if (c != '%') {
            out_char(o, c);
            continue;
        }
        c = *fmt++;
        if (c == '%') {
            out_char(o, '%');
            continue;
        }
        if (c == '-') {
            c = *fmt++; /* left-justify ignored */
        }
        if (c == '0') {
            zero_pad = 1;
            c = *fmt++;
        }
        while (c >= '0' && c <= '9') {
            width = width * 10 + (c - '0');
            c = *fmt++;
        }
        if (c == '.') {
            precision = 0;
            c = *fmt++;
            while (c >= '0' && c <= '9') {
                precision = precision * 10 + (c - '0');
                c = *fmt++;
            }
        }
        while (c == 'l' || c == 'z' || c == 'h') {
            if (c == 'l' || c == 'z') {
                is_long++;
            }
            c = *fmt++;
        }
        switch (c) {
        case 'd':
        case 'i': {
            long long v = is_long ? va_arg(ap, long long) : va_arg(ap, int);
            int neg = v < 0;
            int number_width = width;
            int number_zero_pad = zero_pad;
            unsigned long long mag =
                neg ? (unsigned long long)(-v) : (unsigned long long)v;
            if (precision >= 0 && precision > number_width) {
                number_width = precision;
                number_zero_pad = 1;
            }
            out_num(o, mag, 10, neg, number_width, number_zero_pad, 0);
            break;
        }
        case 'u': {
            unsigned long long v = is_long ? va_arg(ap, unsigned long long)
                                           : va_arg(ap, unsigned int);
            int number_width = width;
            int number_zero_pad = zero_pad;
            if (precision >= 0 && precision > number_width) {
                number_width = precision;
                number_zero_pad = 1;
            }
            out_num(o, v, 10, 0, number_width, number_zero_pad, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v = is_long ? va_arg(ap, unsigned long long)
                                           : va_arg(ap, unsigned int);
            int number_width = width;
            int number_zero_pad = zero_pad;
            if (precision >= 0 && precision > number_width) {
                number_width = precision;
                number_zero_pad = 1;
            }
            out_num(o, v, 16, 0, number_width, number_zero_pad, c == 'X');
            break;
        }
        case 'p': {
            out_str(o, "0x", -1);
            out_num(o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16,
                    0, 0, 0, 0);
            break;
        }
        case 'c': {
            out_char(o, (char)va_arg(ap, int));
            break;
        }
        case 's': {
            out_str(o, va_arg(ap, const char *), precision);
            break;
        }
        case 'f':
        case 'g': {
            out_double(o, va_arg(ap, double), precision);
            break;
        }
        default:
            out_char(o, '%');
            out_char(o, c);
            break;
        }
    }
    if (o->buf && o->cap) {
        size_t end = o->len < o->cap - 1 ? o->len : o->cap - 1;
        o->buf[end] = 0;
    }
    return (int)o->len;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    sura_out o = {buf, size, 0};
    return format(&o, fmt, ap);
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)1 << 30, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

static void serial_out(const char *data, size_t len)
{
    sura_serial_write(data, len);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    (void)f;
    serial_out(buf, strlen(buf));
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    serial_out(s, strlen(s));
    serial_out("\r\n", 2);
    return 0;
}

int putchar(int c)
{
    char ch = (char)c;
    serial_out(&ch, 1);
    return c;
}

int fputs(const char *s, FILE *f)
{
    (void)f;
    serial_out(s, strlen(s));
    return 0;
}

int fputc(int c, FILE *f)
{
    (void)f;
    return putchar(c);
}

int sscanf(const char *str, const char *fmt, ...)
{
    (void)str;
    (void)fmt;
    return 0;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    (void)f;
    (void)fmt;
    return EOF;
}

void sura_assert_fail(const char *expr, const char *file, int line)
{
    printf("assert failed: %s (%s:%d)\r\n", expr, file, line);
    sura_exit(99);
}
