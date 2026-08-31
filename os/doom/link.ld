ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R X */
    rodata PT_LOAD FLAGS(4); /* R   */
    data   PT_LOAD FLAGS(6); /* R W */
}

SECTIONS
{
    /*
     * PML4 slot 0 contains the Sura kernel's identity map.  Keeping Doom in
     * slot 2 lets the process page table share that kernel slot as
     * supervisor-only while mapping the game as ordinary Ring-3 memory.
     */
    . = 0x0000010000400000;

    .text : ALIGN(4096) {
        *(.text*)
        *(.ltext*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata*)
        *(.lrodata*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data*)
        *(.ldata*)
    } :data

    .bss : {
        *(COMMON)
        *(.bss*)
        *(.lbss*)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
