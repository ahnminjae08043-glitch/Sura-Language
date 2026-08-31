/* Process entry and exit for the Sura OS doom port. */

#include <stdlib.h>

#include "sura_syscall.h"

#define SURA_MAX_ATEXIT 32

static void (*atexit_funcs[SURA_MAX_ATEXIT])(void);
static int atexit_count;

int main(int argc, char **argv);

int atexit(void (*func)(void))
{
    if (atexit_count >= SURA_MAX_ATEXIT) {
        return -1;
    }
    atexit_funcs[atexit_count++] = func;
    return 0;
}

void exit(int code)
{
    while (atexit_count > 0) {
        atexit_funcs[--atexit_count]();
    }
    sura_exit((uint64_t)code);
    for (;;) {
    }
}

void abort(void)
{
    sura_exit(98);
    for (;;) {
    }
}

void _start(void)
{
    /*
     * Skip Doom's attract-mode demo.  With no command-line arguments the
     * title screen eventually starts a prerecorded demo, which looks like
     * unsolicited keyboard input.  Start the shareware episode directly.
     */
    static char *argv[] = {"doom", "-warp", "1", "1", 0};
    exit(main(4, argv));
}
