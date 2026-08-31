/* Minimal freestanding libc for the Sura OS doom port. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

int errno;

/* ---------- memory ---------- */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) {
            return (void *)p;
        }
        p++;
    }
    return NULL;
}

/* ---------- strings ---------- */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) {
        p++;
    }
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != 0) {
    }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && *src) {
        *d++ = *src++;
        n--;
    }
    while (n--) {
        *d++ = 0;
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src) {
        *d++ = *src++;
    }
    *d = 0;
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) {
            return (char *)s;
        }
        s++;
    }
    if ((char)c == 0) {
        return (char *)s;
    }
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;
    while (*s) {
        if (*s == (char)c) {
            found = s;
        }
        s++;
    }
    if ((char)c == 0) {
        return (char *)s;
    }
    return (char *)found;
}

char *strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) {
        return (char *)hay;
    }
    while (*hay) {
        if (strncmp(hay, needle, nl) == 0) {
            return (char *)hay;
        }
        hay++;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) {
        n++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) {
        n++;
    }
    return n;
}

char *strtok(char *str, const char *delim)
{
    static char *saved;
    char *token;
    if (str == NULL) {
        str = saved;
    }
    if (str == NULL) {
        return NULL;
    }
    str += strspn(str, delim);
    if (*str == 0) {
        saved = NULL;
        return NULL;
    }
    token = str;
    str += strcspn(str, delim);
    if (*str) {
        *str = 0;
        saved = str + 1;
    } else {
        saved = NULL;
    }
    return token;
}

char *strerror(int errnum)
{
    (void)errnum;
    return "error";
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a &&
           tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ---------- ctype ---------- */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
           c == '\f';
}
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int isgraph(int c) { return c > 0x20 && c < 0x7f; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

/* ---------- conversions ---------- */

long strtol(const char *s, char **endptr, int base)
{
    long value = 0;
    int negative = 0;
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0 && s[0] == '0') {
        base = 8;
        s++;
    } else if (base == 0) {
        base = 10;
    }
    for (;;) {
        int digit;
        char c = *s;
        if (isdigit((unsigned char)c)) {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = c - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * base + digit;
        s++;
    }
    if (endptr) {
        *endptr = (char *)s;
    }
    return negative ? -value : value;
}

unsigned long strtoul(const char *s, char **endptr, int base)
{
    return (unsigned long)strtol(s, endptr, base);
}

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

double atof(const char *s)
{
    double value = 0.0;
    double frac = 0.0;
    double scale = 0.1;
    int negative = 0;
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        negative = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (isdigit((unsigned char)*s)) {
        value = value * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            frac += (*s - '0') * scale;
            scale *= 0.1;
            s++;
        }
    }
    value += frac;
    return negative ? -value : value;
}

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* ---------- misc ---------- */

static unsigned int rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7fff);
}

void srand(unsigned int seed)
{
    rand_state = seed;
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int system(const char *cmd)
{
    (void)cmd;
    return -1;
}

int mkdir(const char *path, unsigned int mode)
{
    (void)path;
    (void)mode;
    return -1;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    /* Simple in-place insertion sort; adequate for small arrays. */
    unsigned char tmp[256];
    unsigned char *b = base;
    size_t i;
    if (size > sizeof(tmp)) {
        return;
    }
    for (i = 1; i < nmemb; i++) {
        size_t j = i;
        memcpy(tmp, b + i * size, size);
        while (j > 0 && compar(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

/* ---------- math ---------- */

double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }
double floor(double x)
{
    long long i = (long long)x;
    if (x < 0 && x != (double)i) {
        i--;
    }
    return (double)i;
}
double ceil(double x)
{
    long long i = (long long)x;
    if (x > 0 && x != (double)i) {
        i++;
    }
    return (double)i;
}
double sqrt(double x)
{
    double guess;
    int i;
    if (x <= 0) {
        return 0;
    }
    guess = x;
    for (i = 0; i < 32; i++) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}
double pow(double x, double y)
{
    /* Integer exponents only; sufficient for this port. */
    long long n = (long long)y;
    double r = 1.0;
    int neg = n < 0;
    if (neg) {
        n = -n;
    }
    while (n--) {
        r *= x;
    }
    return neg ? 1.0 / r : r;
}
