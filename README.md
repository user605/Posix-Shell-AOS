# POSIX shell — AOS Assignment 2

A modular C++17 implementation of `AOS_Assignment_2_M25_V1.pdf`.

Build with `make` and start `./posix-shell` (or `./a.out`). The launch directory
is the shell's home directory. The shell also accepts commands on standard input.

The implementation currently supports `cd`, `pwd`, `echo`, `ls -al`, `pinfo`,
recursive `search`, persistent `history`, `exit`, semicolon-separated commands
and foreground executables. The parser
recognizes the remaining operators; execution support is being added next.

`src/parser.cpp` tokenizes with `strtok`. `src/builtins.cpp` implements built-ins;
`src/shell.cpp` runs commands; `src/history.cpp` stores up to 20 commands across
sessions. `src/util.cpp` supplies POSIX path and directory helpers.
`src/inspection.cpp` implements directory listings, `/proc` inspection and search.

Formatting is defined in `.clang-format` (verified with clang-format 23.1.5).
Run `make lint` or `make format-check` to check every C++ source and header,
including tests when present. Run `make format` to apply the style. These
targets check formatting; compiler warnings are not treated as lint failures.

Namespaces and class access labels are indented in four-space steps, so nested
closing braces align with their own opening scopes rather than with outer scopes.
