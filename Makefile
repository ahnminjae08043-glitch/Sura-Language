CXX      = C:/msys64/mingw64/bin/g++.exe
CXXFLAGS = -std=c++17 -Wall -O2
INCLUDES = -IC:/msys64/mingw64/include
LIBS     = -LC:/msys64/mingw64/lib -lsfml-graphics -lsfml-window -lsfml-system

# 플랫폼별 출력 파일 이름
ifeq ($(OS),Windows_NT)
    TARGET     = SuraEngine2.exe
    JIT_TARGET = SuraJIT.exe
    RM         = del /Q
else
    TARGET     = SuraEngine2
    JIT_TARGET = SuraJIT
    RM         = rm -f
endif

HEADERS     = platform.hpp lexer.hpp ast.hpp value.hpp parser.hpp compiler.hpp jit.hpp typechecker.hpp interpreter.hpp
JIT_HEADERS = platform.hpp lexer.hpp ast.hpp value.hpp parser.hpp jit.hpp

.PHONY: all clean run jit jit-run jit-test

all: $(TARGET) $(JIT_TARGET)

# ── 풀 엔진 (SFML 포함) ─────────────────────────────────────────
$(TARGET): main2.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) main2.cpp -o $(TARGET) $(LIBS)
	@echo 빌드 완료: $(TARGET)

# ── JIT 컴파일러 (SFML 불필요, 독립 실행) ────────────────────────
jit: $(JIT_TARGET)

$(JIT_TARGET): sura_jit.cpp $(JIT_HEADERS)
	$(CXX) $(CXXFLAGS) sura_jit.cpp -o $(JIT_TARGET)
	@echo JIT 빌드 완료: $(JIT_TARGET)

clean:
	$(RM) $(TARGET) $(JIT_TARGET)

run: $(TARGET)
	./$(TARGET)

# ── JIT 테스트 ────────────────────────────────────────────────────
jit-run: $(JIT_TARGET)
	./$(JIT_TARGET) --repl

jit-test: $(JIT_TARGET)
	./$(JIT_TARGET) test_jit.sura

jit-bench: $(JIT_TARGET)
	./$(JIT_TARGET) --bench test_jit.sura

jit-dump: $(JIT_TARGET)
	./$(JIT_TARGET) --dump test_jit.sura
