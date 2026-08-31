/* doomgeneric platform implementation for Sura OS.
 *
 * The standalone gate and desktop host provide five syscalls: millisecond
 * ticks, Set-1-compatible keyboard event polling, framebuffer blit, serial
 * output, and process exit.
 */

#include <stdint.h>
#include <stdio.h>

#include "doomgeneric.h"
#include "doomkeys.h"

#include "libc/sura_syscall.h"

#define KEYQUEUE_SIZE 32

static unsigned short key_queue[KEYQUEUE_SIZE];
static unsigned int key_queue_write;
static unsigned int key_queue_read;
static int extended_prefix;

static unsigned char scancode_to_doomkey(unsigned char code, int extended)
{
    if (extended) {
        switch (code) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_FIRE;      /* right ctrl */
        case 0x38: return KEY_RALT;
        case 0x1C: return KEY_ENTER;
        default: return 0;
        }
    }
    switch (code) {
    case 0x01: return KEY_ESCAPE;
    case 0x1C: return KEY_ENTER;
    case 0x0F: return KEY_TAB;
    case 0x0E: return KEY_BACKSPACE;
    case 0x1D: return KEY_FIRE;          /* left ctrl */
    case 0x39: return KEY_USE;           /* space */
    case 0x2A: return KEY_RSHIFT;        /* left shift */
    case 0x36: return KEY_RSHIFT;        /* right shift */
    case 0x38: return KEY_LALT;
    case 0x3A: return KEY_CAPSLOCK;
    case 0x0C: return KEY_MINUS;
    case 0x0D: return KEY_EQUALS;
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    default: break;
    }
    {
        static const char row_digits[] = "1234567890";
        static const char row_q[] = "qwertyuiop";
        static const char row_a[] = "asdfghjkl";
        static const char row_z[] = "zxcvbnm";
        if (code >= 0x02 && code <= 0x0B) {
            return (unsigned char)row_digits[code - 0x02];
        }
        if (code >= 0x10 && code <= 0x19) {
            return (unsigned char)row_q[code - 0x10];
        }
        if (code >= 0x1E && code <= 0x26) {
            return (unsigned char)row_a[code - 0x1E];
        }
        if (code >= 0x2C && code <= 0x32) {
            return (unsigned char)row_z[code - 0x2C];
        }
        if (code == 0x33) {
            return ',';
        }
        if (code == 0x34) {
            return '.';
        }
        if (code == 0x35) {
            return '/';
        }
        if (code == 0x28) {
            return '\'';
        }
        if (code == 0x27) {
            return ';';
        }
    }
    return 0;
}

static void pump_key_events(void)
{
    for (;;) {
        uint64_t raw = sura_key_poll();
        unsigned char code;
        int pressed;
        unsigned char doom_key;
        if (raw == 0xFFFFFFFFFFFFFFFFull) {
            break;
        }
        code = (unsigned char)(raw & 0xFF);
        if (code == 0xE0) {
            extended_prefix = 1;
            continue;
        }
        pressed = (code & 0x80) == 0;
        code &= 0x7F;
        doom_key = scancode_to_doomkey(code, extended_prefix);
        extended_prefix = 0;
        if (doom_key == 0) {
            continue;
        }
        key_queue[key_queue_write % KEYQUEUE_SIZE] =
            (unsigned short)((pressed << 8) | doom_key);
        key_queue_write++;
    }
}

void DG_Init(void)
{
    printf("SURA_DOOM_DG_INIT\r\n");
}

void DG_DrawFrame(void)
{
    pump_key_events();
    sura_blit(DG_ScreenBuffer, DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms)
{
    uint64_t end = sura_ticks_ms() + ms;
    while (sura_ticks_ms() < end) {
        __asm__ volatile("pause");
    }
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)sura_ticks_ms();
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
    unsigned short entry;
    pump_key_events();
    if (key_queue_read == key_queue_write) {
        return 0;
    }
    entry = key_queue[key_queue_read % KEYQUEUE_SIZE];
    key_queue_read++;
    *pressed = entry >> 8;
    *doomKey = entry & 0xFF;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    printf("SURA_DOOM_MAIN\r\n");
    doomgeneric_Create(argc, argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
