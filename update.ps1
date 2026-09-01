$content = Get-Content -Path "jit_compiler.hpp" -Raw -Encoding UTF8
$content = $content -replace "\buint8_t\b", "uint16_t"
$content = $content -replace "next_reg >= 255", "next_reg >= 65535"
$content = $content -replace "최대 255개", "최대 65535개"
Set-Content -Path "jit_compiler.hpp" -Value $content -Encoding UTF8

$content2 = Get-Content -Path "jit_vm.hpp" -Raw -Encoding UTF8
$content2 = $content2 -replace "Value registers\[256\];", "std::vector<Value> registers;"
$content2 = $content2 -replace "uint8_t a = inst.a, b = inst.b, c = inst.c;", "uint16_t a = inst.a, b = inst.b, c = inst.c;"
$content2 = $content2 -replace "for \(int i=0; i<256; \+\+i\) frame.registers\[i\] = Value::nil\(\);", "frame.registers.resize(chunk.max_regs > 0 ? chunk.max_regs : 256, Value::nil());"
$content2 = $content2 -replace "for \(int j=0; j<256; \+\+j\) new_frame.registers\[j\] = Value::nil\(\);", "new_frame.registers.resize(chunk.max_regs > 0 ? chunk.max_regs : 256, Value::nil());"
Set-Content -Path "jit_vm.hpp" -Value $content2 -Encoding UTF8
