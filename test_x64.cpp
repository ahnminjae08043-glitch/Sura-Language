#include <iostream>
#include <windows.h>

typedef int (*JitFunc)(int, int);

int main() {
    // x64 add rcx, rdx; mov rax, rcx; ret
    // 48 01 D1 (add rcx, rdx) -> wait, add rcx, rdx is 48 01 d1
    // 48 89 C8 (mov rax, rcx)
    // C3 (ret)
    unsigned char code[] = { 0x48, 0x01, 0xD1, 0x48, 0x89, 0xC8, 0xC3 };
    
    void* mem = VirtualAlloc(NULL, sizeof(code), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!mem) return 1;
    memcpy(mem, code, sizeof(code));
    
    JitFunc func = (JitFunc)mem;
    int res = func(10, 20);
    std::cout << "JIT output: " << res << std::endl;
    
    VirtualFree(mem, 0, MEM_RELEASE);
    return 0;
}
