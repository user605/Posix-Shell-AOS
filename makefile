CXX ?= g++
CLANG_FORMAT ?= clang-format
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?=
SOURCES := $(wildcard src/*.cpp)
FORMAT_FILES := $(wildcard src/*.cpp include/*.hpp tests/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean test sanitize package format format-check lint
all: posix-shell a.out

posix-shell: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ -o $@

a.out: posix-shell
	ln -sf posix-shell a.out

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

build/test-shell: tests/test_shell.cpp src/parser.cpp src/history.cpp src/util.cpp src/editor.cpp include/*.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) tests/test_shell.cpp src/parser.cpp src/history.cpp src/util.cpp src/editor.cpp -lutil -o $@

test: all build/test-shell
	./build/test-shell "$(CURDIR)/posix-shell"

clean:
	rm -rf build posix-shell a.out

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

lint: format-check

-include $(DEPS)
