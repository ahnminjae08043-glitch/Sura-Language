CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -DNDEBUG -Wall
CPPFLAGS ?=
LDFLAGS ?=

# Portable is the release default. Use `make NATIVE=1` only for a local,
# machine-specific performance build that will not be redistributed.
NATIVE ?= 0
ifeq ($(NATIVE),1)
    CXXFLAGS += -march=native
endif

ifeq ($(OS),Windows_NT)
    EXEEXT = .exe
    PLATFORM_LIBS = -lgdi32
    # Zero the PE header timestamp. Without this, two builds of identical
    # source differ in exactly the 4 timestamp bytes, so a recorded release
    # SHA-256 cannot be reproduced by anyone rebuilding the tree.
    LDFLAGS += -Wl,--no-insert-timestamp
else
    EXEEXT =
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Linux)
        PLATFORM_LIBS = -ldl
    endif
endif

TARGET      = SuraLanguage$(EXEEXT)
PKG_TARGET  = surapkg$(EXEEXT)
JIT_TARGET  = SuraJIT$(EXEEXT)

ENGINE_SOURCES = main.cpp gc.cpp platform.cpp
ENGINE_HEADERS = $(wildcard *.hpp)
JIT_SOURCES    = sura_jit.cpp platform.cpp

.PHONY: all engine package clean run jit jit-run jit-test jit-bench jit-dump

all: engine package

engine: $(TARGET)

package: $(PKG_TARGET)

$(TARGET): $(ENGINE_SOURCES) $(ENGINE_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(ENGINE_SOURCES) -o $@ $(LDFLAGS) $(PLATFORM_LIBS)
	@echo Built $@

$(PKG_TARGET): surapkg.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) surapkg.cpp -o $@ $(LDFLAGS) $(PLATFORM_LIBS)
	@echo Built $@

# The standalone JIT driver is optional; SuraLanguage is the canonical runtime.
jit: $(JIT_TARGET)

$(JIT_TARGET): $(JIT_SOURCES) $(ENGINE_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(JIT_SOURCES) -o $@ $(LDFLAGS) $(PLATFORM_LIBS)
	@echo Built $@

clean:
	$(RM) $(TARGET) $(PKG_TARGET) $(JIT_TARGET)

run: $(TARGET)
	./$(TARGET)

jit-run: $(JIT_TARGET)
	./$(JIT_TARGET) --repl

jit-test: $(JIT_TARGET)
	./$(JIT_TARGET) test_jit.sura

jit-bench: $(JIT_TARGET)
	./$(JIT_TARGET) --bench test_jit.sura

jit-dump: $(JIT_TARGET)
	./$(JIT_TARGET) --dump test_jit.sura
