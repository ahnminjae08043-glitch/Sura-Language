param(
    [string]$Clang = "C:\msys64\mingw64\bin\clang.exe",
    [string]$Lld = "C:\msys64\mingw64\bin\ld.lld.exe",
    [string]$OutDir = "build"
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$doomSrc = "..\..\third_party\doomgeneric\doomgeneric"
if (-not (Test-Path $doomSrc)) { throw "doomgeneric source not found: $doomSrc" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$cflags = @(
    "--target=x86_64-unknown-none-elf",
    "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
    "-fno-asynchronous-unwind-tables", "-fno-builtin", "-mno-red-zone",
    # Sura OS keeps its low identity-map slot supervisor-only.  Doom is linked
    # in PML4 slot 2, so the freestanding C code must use full 64-bit addresses.
    "-mcmodel=large",
    "-O2", "-g0",
    "-Wall", "-Wno-unused-variable", "-Wno-unused-but-set-variable",
    "-isystem", "include",
    "-I", $doomSrc,
    "-I", "."
)

$doomObjects = @(
    "dummy", "am_map", "doomdef", "doomstat", "dstrings", "d_event",
    "d_items", "d_iwad", "d_loop", "d_main", "d_mode", "d_net",
    "f_finale", "f_wipe", "g_game", "hu_lib", "hu_stuff", "info",
    "i_cdmus", "i_endoom", "i_joystick", "i_scale", "i_sound", "i_system",
    "i_timer", "memio", "m_argv", "m_bbox", "m_cheat", "m_config",
    "m_controls", "m_fixed", "m_menu", "m_misc", "m_random", "p_ceilng",
    "p_doors", "p_enemy", "p_floor", "p_inter", "p_lights", "p_map",
    "p_maputl", "p_mobj", "p_plats", "p_pspr", "p_saveg", "p_setup",
    "p_sight", "p_spec", "p_switch", "p_telept", "p_tick", "p_user",
    "r_bsp", "r_data", "r_draw", "r_main", "r_plane", "r_segs", "r_sky",
    "r_things", "sha1", "sounds", "statdump", "st_lib", "st_stuff",
    "s_sound", "tables", "v_video", "wi_stuff", "w_checksum", "w_file",
    "w_main", "w_wad", "z_zone", "w_file_stdc", "i_input", "i_video",
    "doomgeneric"
)

$localSources = @(
    @{ src = "doomgeneric_sura.c"; obj = "doomgeneric_sura" },
    @{ src = "libc\libc.c";        obj = "sura_libc" },
    @{ src = "libc\malloc.c";      obj = "sura_malloc" },
    @{ src = "libc\printf.c";      obj = "sura_printf" },
    @{ src = "libc\file.c";        obj = "sura_file" },
    @{ src = "libc\crt.c";         obj = "sura_crt" }
)

$objects = @()

foreach ($name in $doomObjects) {
    $src = Join-Path $doomSrc "$name.c"
    $obj = Join-Path $OutDir "$name.o"
    & $Clang @cflags -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $name.c" }
    $objects += $obj
}

foreach ($entry in $localSources) {
    $obj = Join-Path $OutDir ($entry.obj + ".o")
    & $Clang @cflags -c $entry.src -o $obj
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($entry.src)" }
    $objects += $obj
}

$wadObj = Join-Path $OutDir "wad.o"
& $Clang --target=x86_64-unknown-none-elf -c wad.S -o $wadObj
if ($LASTEXITCODE -ne 0) { throw "assemble failed: wad.S" }
$objects += $wadObj

$elf = Join-Path $OutDir "doom.elf"
# The MSYS2 clang driver is hosted as MinGW and may route -fuse-ld=lld through
# collect2/lld-link even for an ELF triple.  Invoke the GNU-compatible LLD
# driver directly so the linker script and ELF emulation are unambiguous.
& $Lld -m elf_x86_64 -static -T link.ld `
    -z max-page-size=4096 -z noexecstack --gc-sections `
    -o $elf @objects
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Host "Built $elf"
& $Clang --version | Select-Object -First 1
Get-Item $elf | Select-Object Name, Length
