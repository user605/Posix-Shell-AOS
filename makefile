CXX ?= g++
CLANG_FORMAT ?= clang-format
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow
LDFLAGS ?=
SOURCES := $(wildcard src/*.cpp)
FORMAT_FILES := $(wildcard src/*.cpp include/*.hpp tests/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)
SANFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie

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

build/sanitize/posix-shell: $(SOURCES) include/*.hpp
	@mkdir -p build/sanitize
	$(CXX) $(CPPFLAGS) $(SANFLAGS) $(SOURCES) $(LDFLAGS) -no-pie -o $@

build/sanitize/test-shell: tests/test_shell.cpp src/parser.cpp src/history.cpp src/util.cpp src/editor.cpp include/*.hpp
	@mkdir -p build/sanitize
	$(CXX) $(CPPFLAGS) $(SANFLAGS) tests/test_shell.cpp src/parser.cpp src/history.cpp src/util.cpp src/editor.cpp $(LDFLAGS) -no-pie -lutil -o $@

sanitize: build/sanitize/posix-shell build/sanitize/test-shell
	ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 ./build/sanitize/test-shell "$(CURDIR)/build/sanitize/posix-shell"

package:
	@case '$(ROLL)' in ''|-*|*[!a-zA-Z0-9_-]*) printf 'Usage: make package ROLL=your_roll_number\n'; exit 2;; esac
	@mkdir -p build/package
	@task_stage=$$(mktemp -d "$(CURDIR)/build/package/stage.XXXXXX") && \
	mkdir "$$task_stage/$(ROLL)_Assignment2" && \
	cp -R .clang-format README.md makefile include src tests "$$task_stage/$(ROLL)_Assignment2/" && \
	(cd "$$task_stage" && zip -qr submission.zip "$(ROLL)_Assignment2") && \
	mv "$$task_stage/submission.zip" "$(ROLL)_Assignment2.zip" && \
	rm -rf "$$task_stage"

clean:
	rm -rf build posix-shell a.out

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

lint: format-check

-include $(DEPS)
