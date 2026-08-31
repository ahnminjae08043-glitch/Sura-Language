/* Memory-backed FILE implementation. The only readable file is the WAD
 * embedded into the executable image (see wad.S). Writes are discarded. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

extern const unsigned char sura_wad_start[];
extern const unsigned char sura_wad_end[];

struct FILE {
    const unsigned char *data;
    size_t size;
    size_t pos;
    int used;
};

#define SURA_MAX_FILES 8
static struct FILE sura_files[SURA_MAX_FILES];

static int name_is_wad(const char *path)
{
    static const char embedded_name[] = "doom1.wad";
    size_t n = strlen(path);
    size_t name_len = sizeof(embedded_name) - 1;
    if (n < name_len ||
        strcasecmp(path + n - name_len, embedded_name) != 0) {
        return 0;
    }
    return n == name_len || path[n - name_len - 1] == '/' ||
           path[n - name_len - 1] == '\\';
}

FILE *fopen(const char *path, const char *mode)
{
    int i;
    if (mode == NULL || mode[0] != 'r' || !name_is_wad(path)) {
        return NULL;
    }
    for (i = 0; i < SURA_MAX_FILES; i++) {
        if (!sura_files[i].used) {
            sura_files[i].data = sura_wad_start;
            sura_files[i].size = (size_t)(sura_wad_end - sura_wad_start);
            sura_files[i].pos = 0;
            sura_files[i].used = 1;
            return &sura_files[i];
        }
    }
    return NULL;
}

int fclose(FILE *f)
{
    if (f && f != stdout && f != stderr) {
        f->used = 0;
    }
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t want;
    size_t left;
    if (f == NULL || f == stdout || f == stderr || size == 0) {
        return 0;
    }
    want = size * nmemb;
    left = f->size - f->pos;
    if (want > left) {
        want = left;
    }
    memcpy(ptr, f->data + f->pos, want);
    f->pos += want;
    return want / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    (void)ptr;
    (void)f;
    return nmemb * (size ? 1 : 0);
}

int fseek(FILE *f, long offset, int whence)
{
    long base;
    if (f == NULL || f == stdout || f == stderr) {
        return -1;
    }
    if (whence == SEEK_SET) {
        base = 0;
    } else if (whence == SEEK_CUR) {
        base = (long)f->pos;
    } else {
        base = (long)f->size;
    }
    base += offset;
    if (base < 0 || (size_t)base > f->size) {
        return -1;
    }
    f->pos = (size_t)base;
    return 0;
}

long ftell(FILE *f)
{
    if (f == NULL || f == stdout || f == stderr) {
        return -1;
    }
    return (long)f->pos;
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

int feof(FILE *f)
{
    if (f == NULL || f == stdout || f == stderr) {
        return 1;
    }
    return f->pos >= f->size;
}

int fgetc(FILE *f)
{
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) {
        return EOF;
    }
    return c;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0;
    while (i + 1 < size) {
        int c = fgetc(f);
        if (c == EOF) {
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (i == 0) {
        return NULL;
    }
    s[i] = 0;
    return s;
}

int remove(const char *path)
{
    (void)path;
    return -1;
}

int rename(const char *oldp, const char *newp)
{
    (void)oldp;
    (void)newp;
    return -1;
}
