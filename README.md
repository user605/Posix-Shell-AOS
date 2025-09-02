# POSIX shell — AOS Assignment 2

A modular C++17 shell implementing all twelve requirements in
`AOS_Assignment_2_M25_V1.pdf`. It targets Linux: process inspection uses `/proc`.
The implementation uses POSIX system calls and the C++ standard library, with
no `system`, `popen`, `pclose`, curses, readline or `std::filesystem` dependency.

## Build and run

You need a C++17 compiler and GNU Make. No extra libraries are needed to build
the shell. The terminal tests use the Linux `libutil` library.

```sh
make
./posix-shell
```

`./a.out` is an equivalent entry point. `make clean` removes compiled files.

Every revision includes a `compile_commands.json` captured with Bear from that
revision's shell build and, where present, its test build. It records this
checkout's absolute directory. After moving or cloning the repository, install
Bear and regenerate it from the new checkout:

```sh
make clean
bear -- make all build/test-shell
```

Commands can also be supplied through standard input:

```sh
printf 'echo hello; pwd\n' | ./posix-shell
```

The directory where the shell starts is its home directory, regardless of the
account's home directory. The prompt is `username@hostname:directory> `, with
that launch directory abbreviated as `~` and its descendants as `~/...`.
Other directories use absolute paths. Username and hostname are discovered at
runtime. `PWD` and `OLDPWD` environment variables are never used.

## Commands and operators

| Requirement | Behavior |
| --- | --- |
| Prompt and parsing | Dynamic prompt, spaces/tabs, semicolon-separated commands; tokenization uses `strtok`. |
| `cd`, `echo`, `pwd` | Implemented internally. `cd` accepts `.`, `..`, `-`, `~`, `~/path` or no argument. More than one argument prints `Invalid arguments`. `echo` joins arguments with spaces. |
| `ls` | Internal listing with `-a`, `-l`, `-al`, `-la`, flags anywhere, and multiple file/directory operands. Long output includes permissions, links, owner, group, size, modification time, symlink targets and directory totals. |
| External commands | `fork`/`execvp`, arguments, foreground waiting, multiple background jobs with PID output, and exit notifications. |
| `pinfo [pid]` | PID, process state, `+` for a terminal foreground group, virtual memory in bytes, and executable path. Defaults to the shell's PID. |
| `search name` | Prints `True` if a file or directory is found recursively below the current directory, otherwise `False`. Includes hidden entries. |
| `<`, `>`, `>>` | Input, truncating output and appending output; combinable. New output files request mode `0644`, subject to the inherited umask. |
| `\|` | Any number of concurrent pipeline stages, including built-ins. |
| Pipes and redirection | Redirection can occur in any stage and overrides that stage's pipe endpoint. Redirections are applied from left to right. |
| Signals | Ctrl-C interrupts the foreground process group. Ctrl-Z stops it and returns the prompt. These keys leave an idle shell alive. Ctrl-D on an empty prompt logs out. |
| Completion | TAB completes built-ins, executable names from `PATH`, and local files/directories. Extends the common prefix; lists ambiguous matches when no further prefix is available. |
| History | Stores the last 20 input lines across sessions; `history` prints the latest 10 and `history N` prints up to N (0–20). Consecutive duplicate lines are skipped. Up/down recall commands for editing. |

Additional job-control commands make stopped jobs usable:

```text
jobs            list active jobs and their process groups
bg              continue the most recent job in the background
fg              continue the most recent job in the foreground
bg %1           select job number 1
fg 12345        select the job with process-group PID 12345
exit            leave the shell
```

An `&` terminates and backgrounds the preceding pipeline. It does not block
later commands on the same line. Background jobs print `[job_number] pid`.
Background built-ins run in a child, so `cd &` cannot change the parent shell's
directory. A `cd` in a pipeline likewise affects only that stage.

```text
ls -l dir1 -a dir2
echo hello > output.txt; echo again >> output.txt
sort < input.txt | head -7 | tail -5 > result.txt
sleep 10 & echo ready
pinfo
search result.txt
history 15
```

## Terminal and persistence details

The editor uses `termios` and `poll`, restoring terminal settings before running
programs and on logout. Up stops at the oldest retained command; down returns
to the newest command and then to the draft you were typing. Recalled lines
can be changed before execution. Left/right, Home/End, Delete, Backspace,
Ctrl-A/E, Ctrl-U/K and Ctrl-L also work. Long input scrolls horizontally.
Unique directory completions end with `/`; completion does not insert a space.

History lives in `.posix_history` in the launch directory. It is updated after
each nonempty input line, including `history` and `exit`. A separate
`.posix_history.lock` and atomic file replacement serialize concurrent sessions.
Only the latest 20 entries are retained. History files request mode `0600`.

Each pipeline has its own process group. A launch gate delays execution until
the group and terminal assignment are ready, preventing foreground terminal
reads from racing the parent. The `SIGCHLD` handler writes to a nonblocking
self-pipe; the main loop reaps children and redraws typed input after a job
notification. The handler uses no C++ streams or containers. Logout sends
`SIGHUP` and `SIGCONT` to remaining jobs.

## Assignment scope

As allowed by the specification, quotes are retained literally: `echo "hello"`
prints `"hello"`. There is no quote grouping, escape processing, environment
variable expansion, wildcard expansion, command substitution, `&&`/`||`, or
stderr redirection. Operator characters are always special. Words may contain
ordinary hyphens, as required for flags and executable names. `~` and `~/...`
refer to the shell's launch directory.

`ls` prints one entry per line without colors. It supports the specified `-a`
and `-l` flags rather than the complete GNU `ls` option set. Recursive search
does not descend through directory symlinks, preventing cycles. In a script,
background jobs read `/dev/null` unless given explicit input redirection.
`pinfo` shows `[unavailable]` when a process has no readable executable link
(for example, a zombie). Permission, syntax and execution errors are reported
without ending the interactive session.

## Source layout

| File | Responsibility |
| --- | --- |
| `src/main.cpp` | Startup, locale, top-level error reporting |
| `src/parser.cpp`, `include/parser.hpp` | `strtok` tokenization and command/pipeline model |
| `src/shell.cpp`, `include/shell.hpp` | Prompt, process execution, redirection, pipelines, signals, job control |
| `src/builtins.cpp` | Built-in dispatch, `cd`, `pwd`, `echo`, `history`, `exit` |
| `src/inspection.cpp`, `include/inspection.hpp` | Internal `ls`, `/proc` process info, recursive search |
| `src/editor.cpp`, `include/editor.hpp` | Terminal editing and file/command completion |
| `src/history.cpp`, `include/history.hpp` | Bounded, locked, persistent history |
| `src/util.cpp`, `include/util.hpp` | POSIX paths, directory reads and descriptor writes |
| `tests/test_shell.cpp` | C++ parser, integration and pseudo-terminal tests; helper executable modes |
| `makefile` | Build, checks, sanitizers and submission packaging |

## Verification and submission

```sh
make test
make sanitize
make package ROLL=your_roll_number
```

The test suite checks command behavior, `ls -al` against the system listing,
redirection errors, a 100,000-line pipeline, a 36-stage pipeline, descriptor
cleanup, nested shells, concurrent history, completion, long editable input,
Ctrl-C/Ctrl-Z/Ctrl-D, `bg`/`fg`, and asynchronous job notifications. `sanitize`
runs the suite with AddressSanitizer and UndefinedBehaviorSanitizer in a
separate build directory.

Packaging requires `zip`. Replace `your_roll_number` with your actual roll
number. The resulting `your_roll_number_Assignment2.zip` contains a single
`your_roll_number_Assignment2/` directory with `README.md`, `makefile`, `src/`,
`include/` and `tests/`. Git metadata, binaries, runtime history and the supplied
assignment PDF are excluded. Unzip it, enter that directory, and run `make`.

The Git history was assembled as a repository import in November 2026. Author
and committer dates reconstruct a timeline before the September 2025 assignment
deadline; these timestamps are archival metadata rather than original
development records.

Formatting is defined in `.clang-format` (verified with clang-format 23.1.5).
Run `make lint` or `make format-check` to check every C++ source and header,
including tests when present. Run `make format` to apply the style. These
targets check formatting; compiler warnings are not treated as lint failures.

Namespaces and class access labels are indented in four-space steps, so nested
closing braces align with their own opening scopes rather than with outer scopes.
