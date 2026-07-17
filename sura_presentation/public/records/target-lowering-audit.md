# Sura Target Lowering Audit

Generated UTC: 2026-07-16T15:11:24.7981994Z
Status: INCOMPLETE
AST nodes: 43
JS full/partial/missing/ignored: 41/0/0/2
WASM full/partial/missing/ignored: 27/14/0/2

## Pipeline
- JS frontend: AST JSON full-target path plus source-line compatibility
- WASM frontend: AST JSON frontend for source and AST JSON input
- AST JSON export: True
- JS AST JSON input: True
- JS AST import expansion: True
- JS full AST target smoke: True
- WASM AST JSON input: True
- WASM AST import expansion: True
- WASM WAT instruction-boundary guard: True
- Full AST/bytecode frontend: True

## Partial Or Missing Nodes

| Node | Kind | JS | WASM | Next action |
| --- | --- | --- | --- | --- |
| NUM_LIT | expr | full | partial | extend f64 lowering from tagged Value boundaries into general numeric expressions, operators, locals, arrays, and exported functions |
| BIN_OP | expr | full | partial | extend tagged f64 numbers into general arithmetic, comparison, conversion, stringification, collection, and export paths, then finish remaining fully dynamic operator semantics |
| DOT_ACCESS | expr | full | partial | add general object/property lowering |
| INDEX | expr | full | partial | add general array/dict/string index lowering |
| CALL | expr | full | partial | lower general calls through bytecode or a Value ABI |
| METHOD_CALL | expr | full | partial | add general method dispatch lowering |
| SUPER_CALL | expr | full | partial | extend remaining super dispatch over the full dynamic Value runtime |
| NEW_EXPR | expr | full | partial | extend object allocation, inheritance, and constructor dispatch to the full dynamic Value runtime |
| FUNC_DEF | stmt | full | partial | add full function body and Value ABI lowering |
| CLASS_DEF | stmt | full | partial | add inherited class layout, methods, and dynamic dispatch lowering |
| THROW | stmt | full | partial | finish fully general object-valued cross-function exception semantics beyond statically discoverable registered classes in WASM |
| TRY | stmt | full | partial | complete fully general instance exception semantics in WASM |
| FUNC_EXPR | expr | full | partial | add capturing closures and fully indirect function-value invocation beyond lifted non-capturing function values |
| STR_INTERP | expr | full | partial | extend string interpolation over the full dynamic Value/string runtime |