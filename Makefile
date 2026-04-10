CXX      = g++
CXXFLAGS = -std=c++17 -Wall -O2
INCLUDES =
LIBS     =

# Platform detection
ifeq ($(OS),Windows_NT)
    TARGET     = SuraEngine.exe
    RM         = del /Q
    # Windows: use MSYS2 paths if available
    ifneq ($(wildcard C:/msys64/mingw64/bin/g++.exe),)
        CXX      = C:/msys64/mingw64/bin/g++.exe
        INCLUDES = -IC:/msys64/mingw64/include
    endif
else
    UNAME_S := $(shell uname -s)
    TARGET  = SuraEngine
    RM      = rm -f
    ifeq ($(UNAME_S),Darwin)
        # macOS: use Homebrew paths if available
        CXXFLAGS += -stdlib=libc++
    endif
    ifeq ($(UNAME_S),Linux)
        # Linux: standard paths
        LIBS += -lpthread
    endif
endif

# Architecture detection
UNAME_M := $(shell uname -m 2>/dev/null || echo x86_64)
ifeq ($(UNAME_M),aarch64)
    CXXFLAGS += -march=armv8-a
endif
ifeq ($(UNAME_M),arm64)
    CXXFLAGS += -march=armv8-a
endif

HEADERS = platform.hpp lexer.hpp ast.hpp value.hpp parser.hpp \
          compiler.hpp jit.hpp typechecker.hpp jit_op.hpp \
          jit_compiler.hpp jit_vm.hpp
SOURCES = main.cpp gc.cpp

.PHONY: all clean run test bench dump repl

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $(TARGET) $(LIBS)
	@echo Build complete: $(TARGET)

clean:
	$(RM) $(TARGET)

run: $(TARGET)
	./$(TARGET)

repl: $(TARGET)
	./$(TARGET) --repl

test: $(TARGET)
	./$(TARGET) test_jit.sura

bench: $(TARGET)
	./$(TARGET) --bench test_jit.sura

dump: $(TARGET)
	./$(TARGET) --dump test_jit.sura

strict-test: $(TARGET)
	./$(TARGET) --strict test_type_error.sura