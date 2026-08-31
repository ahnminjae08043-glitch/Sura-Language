/* Bump allocator with a free-list-less realloc. Doom performs almost all of
 * its allocation once at startup (zone memory), so this is sufficient. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/*
 * Doomgeneric reserves a 6 MiB zone by default.  Twelve MiB leaves room for
 * the 640x400 conversion buffer and startup allocations without forcing the
 * kernel to create more than twelve thousand 4 KiB mappings at launch.
 */
#define SURA_HEAP_BYTES (12u * 1024u * 1024u)

static unsigned char sura_heap[SURA_HEAP_BYTES]
    __attribute__((aligned(4096)));
static size_t sura_heap_used;

typedef struct {
    size_t size;
    size_t magic;
} sura_alloc_header;

#define SURA_ALLOC_MAGIC 0x53555241u

void *malloc(size_t size)
{
    sura_alloc_header *header;
    size_t total = (size + 15u) & ~(size_t)15u;
    total += sizeof(sura_alloc_header);
    if (sura_heap_used + total > SURA_HEAP_BYTES) {
        return NULL;
    }
    header = (sura_alloc_header *)(sura_heap + sura_heap_used);
    header->size = size;
    header->magic = SURA_ALLOC_MAGIC;
    sura_heap_used += total;
    return header + 1;
}

void free(void *ptr)
{
    (void)ptr;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void *realloc(void *ptr, size_t size)
{
    sura_alloc_header *header;
    void *fresh;
    size_t old;
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        return NULL;
    }
    header = (sura_alloc_header *)ptr - 1;
    old = header->magic == SURA_ALLOC_MAGIC ? header->size : 0;
    fresh = malloc(size);
    if (fresh && old) {
        memcpy(fresh, ptr, old < size ? old : size);
    }
    return fresh;
}
