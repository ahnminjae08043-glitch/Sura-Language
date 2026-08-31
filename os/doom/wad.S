    .section .rodata
    .global sura_wad_start
    .balign 16
sura_wad_start:
    .incbin "../../third_party/doom1.wad"
    .global sura_wad_end
sura_wad_end:
