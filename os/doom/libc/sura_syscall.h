#ifndef SURA_SYSCALL_H
#define SURA_SYSCALL_H

#include <stdint.h>

/* Syscall ABI for the Sura OS doom gate.
 * number in rax, args in rdi, rsi, rdx, r10, r8; result in rax.
 * Entered through the DPL-3 software interrupt gate 0x80.
 */
#define SURA_SYS_TICKS_MS   100
#define SURA_SYS_KEY_POLL   101
#define SURA_SYS_BLIT       102
#define SURA_SYS_SERIAL     103
#define SURA_SYS_EXIT       104

static inline uint64_t sura_syscall3(uint64_t num, uint64_t a0, uint64_t a1,
                                     uint64_t a2)
{
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = 0;
    register uint64_t r8 __asm__("r8") = 0;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(num), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sura_syscall0(uint64_t num)
{
    return sura_syscall3(num, 0, 0, 0);
}

static inline uint64_t sura_ticks_ms(void)
{
    return sura_syscall0(SURA_SYS_TICKS_MS);
}

static inline uint64_t sura_key_poll(void)
{
    return sura_syscall0(SURA_SYS_KEY_POLL);
}

static inline void sura_blit(const void *pixels, uint64_t w, uint64_t h)
{
    sura_syscall3(SURA_SYS_BLIT, (uint64_t)pixels, w, h);
}

static inline void sura_serial_write(const char *data, uint64_t len)
{
    sura_syscall3(SURA_SYS_SERIAL, (uint64_t)data, len, 0);
}

static inline void sura_exit(uint64_t code)
{
    sura_syscall3(SURA_SYS_EXIT, code, 0, 0);
    for (;;) {
    }
}

#endif
