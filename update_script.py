import re

with open('jit_compiler.hpp', 'r', encoding='utf-8') as f:
    text = f.read()

# Replace all empty string arguments with -1
text = text.replace(', "", ln', ', -1, ln')

# Replace specific property string arguments with chunk.add_string(...)
replacements = [
    (r'segs\[j\]\.text, line\)', r'chunk.add_string(segs[j].text), line)'),
    (r', name, ln\)', r', chunk.add_string(name), ln)'),
    (r', da->prop, ln\)', r', chunk.add_string(da->prop), ln)'),
    (r', ce->name, ln\)', r', chunk.add_string(ce->name), ln)'),
    (r', mc->method, ln\)', r', chunk.add_string(mc->method), ln)'),
    (r', sc->method, ln\)', r', chunk.add_string(sc->method), ln)'),
    (r', ne->class_name, ln\)', r', chunk.add_string(ne->class_name), ln)'),
    (r', a->name, ln\)', r', chunk.add_string(a->name), ln)'), # For ASSIGN
    (r', ip->name, ln\)', r', chunk.add_string(ip->name), ln)'),
    (r', da->obj_name, ln\)', r', chunk.add_string(da->obj_name), ln)'),
    (r', ia->name, ln\)', r', chunk.add_string(ia->name), ln)'),
    (r', fd->name, ln\)', r', chunk.add_string(fd->name), ln)'),
    (r', cd->name, ln\)', r', chunk.add_string(cd->name), ln)'),
    (r', c->cmd \+ std::string\(1, \'\\0\'\) \+ ident_info, ln\)', r', chunk.add_string(c->cmd + std::string(1, \'\\0\') + ident_info), ln)'),
    (r'static_cast<const UseStmt\*>\(s\)->lib, ln\)', r'chunk.add_string(static_cast<const UseStmt*>(s)->lib), ln)'),
]

for pattern, repl in replacements:
    text = re.sub(pattern, repl, text)

with open('jit_compiler.hpp', 'w', encoding='utf-8') as f:
    f.write(text)

with open('jit_vm.hpp', 'r', encoding='utf-8') as f:
    vm_text = f.read()

# Replace inst.str_op with chunk.get_string(inst.str_idx)
vm_text = vm_text.replace('inst.str_op', 'chunk.get_string(inst.str_idx)')
# Replace size_t ip = 0; with size_t ip = 0; \n    const JitChunk* chunk = nullptr; already there...
# Wait, some places might need `auto& str_op = chunk.get_string(inst.str_idx);`

with open('jit_vm.hpp', 'w', encoding='utf-8') as f:
    f.write(vm_text)

print("Update Done!")
