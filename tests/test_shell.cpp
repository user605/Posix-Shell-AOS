#include "editor.hpp"
#include "history.hpp"
#include "parser.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <poll.h>
#include <pty.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace posix_shell;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
    std::string shellBinary, testBinary;
    int passed = 0, failed = 0;

    void require(bool condition, const std::string &message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    void contains(const std::string &text, const std::string &part) {
        require(text.find(part) != std::string::npos, "missing [" + part + "] in:\n" + text);
    }
    std::string readFile(const std::string &path) {
        std::ifstream input(path);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    void putFile(const std::string &path, const std::string &text) {
        std::ofstream output(path);
        output << text;
        require(static_cast<bool>(output), "write fixture " + path);
    }
    void removeTree(const std::string &path) {
        struct stat info{};
        if (lstat(path.c_str(), &info) < 0)
            return;
        if (S_ISDIR(info.st_mode)) {
            for (const auto &name : directoryEntries(path)) {
                if (name != "." && name != "..")
                    removeTree(path + "/" + name);
            }
            rmdir(path.c_str());
        } else
            unlink(path.c_str());
    }
    struct Fixture {
        std::string path;
        Fixture() {
            char pattern[] = "/tmp/posix-shell-test.XXXXXX";
            const char *result = mkdtemp(pattern);
            require(result != nullptr, "mkdtemp");
            path = result;
        }
        ~Fixture() {
            removeTree(path);
        }
    };

    struct Result {
        int status;
        std::string output;
    };
    Result run(const std::string &cwd, const std::string &script, const std::string &binary = "",
               std::chrono::milliseconds timeout = 5000ms) {
        int input[2], output[2];
        require(pipe(input) == 0 && pipe(output) == 0, "pipe");
        const pid_t child = fork();
        require(child >= 0, "fork");
        if (child == 0) {
            setpgid(0, 0);
            chdir(cwd.c_str());
            dup2(input[0], STDIN_FILENO);
            dup2(output[1], STDOUT_FILENO);
            dup2(output[1], STDERR_FILENO);
            for (int fd : {input[0], input[1], output[0], output[1]})
                close(fd);
            setenv("LC_ALL", "C", 1);
            const std::string executable = binary.empty() ? shellBinary : binary;
            execl(executable.c_str(), executable.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        close(input[0]);
        close(output[1]);
        require(writeAll(input[1], script), "write script");
        close(input[1]);
        fcntl(output[0], F_SETFL, O_NONBLOCK);
        std::string text;
        bool complete = false, eof = false;
        int status = 0;
        const auto deadline = Clock::now() + timeout;
        while (!complete || !eof) {
            char buffer[4096];
            ssize_t count;
            while ((count = read(output[0], buffer, sizeof(buffer))) > 0)
                text.append(buffer, static_cast<std::size_t>(count));
            if (count == 0)
                eof = true;
            if (!complete)
                complete = waitpid(child, &status, WNOHANG) == child;
            if (complete && eof)
                break;
            if (Clock::now() >= deadline) {
                kill(-child, SIGKILL);
                kill(child, SIGKILL);
                waitpid(child, nullptr, 0);
                close(output[0]);
                throw std::runtime_error("shell timed out:\n" + text);
            }
            struct pollfd fd{output[0], POLLIN, 0};
            poll(&fd, 1, 10);
        }
        close(output[0]);
        return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), text};
    }

    struct Pty {
        int fd = -1;
        pid_t pid = -1;
        std::string pending;
        explicit Pty(const std::string &cwd, const std::string &extraPath = "") {
            struct winsize size{24, 100, 0, 0};
            pid = forkpty(&fd, nullptr, nullptr, &size);
            require(pid >= 0, "forkpty");
            if (pid == 0) {
                chdir(cwd.c_str());
                setenv("LC_ALL", "C", 1);
                if (!extraPath.empty())
                    setenv("PATH", (extraPath + ":/usr/bin:/bin").c_str(), 1);
                execl(shellBinary.c_str(), shellBinary.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
            fcntl(fd, F_SETFL, O_NONBLOCK);
            expect("> ");
        }
        ~Pty() {
            if (pid > 0) {
                // Give the shell a chance to HUP its jobs on an assertion failure.
                writeAll(fd, std::string(1, 3) + std::string(1, 4));
                for (int i = 0; i < 20; ++i) {
                    if (waitpid(pid, nullptr, WNOHANG) == pid) {
                        pid = -1;
                        break;
                    }
                    std::this_thread::sleep_for(10ms);
                }
            }
            if (pid > 0) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
            if (fd >= 0)
                close(fd);
        }
        void send(const std::string &value) {
            require(writeAll(fd, value), "pty write");
        }
        std::string expect(const std::string &needle, std::chrono::milliseconds timeout = 3000ms) {
            const auto deadline = Clock::now() + timeout;
            for (;;) {
                const auto found = pending.find(needle);
                if (found != std::string::npos) {
                    const auto end = found + needle.size();
                    std::string result = pending.substr(0, end);
                    pending.erase(0, end);
                    return result;
                }
                require(Clock::now() < deadline,
                        "terminal timed out waiting for [" + needle + "]:\n" + pending);
                struct pollfd input{fd, POLLIN, 0};
                poll(&input, 1, 20);
                char buffer[8192];
                const ssize_t count = read(fd, buffer, sizeof(buffer));
                if (count > 0)
                    pending.append(buffer, static_cast<std::size_t>(count));
                else if (count == 0 || (count < 0 && errno == EIO))
                    throw std::runtime_error("terminal closed waiting for " + needle + ":\n" +
                                             pending);
            }
        }
        void command(const std::string &text, const std::string &expected) {
            send(text + "\n");
            expect(expected);
            expect("> ");
        }
        void logout() {
            send(std::string(1, 4));
            const auto deadline = Clock::now() + 3000ms;
            int status = 0;
            for (;;) {
                if (waitpid(pid, &status, WNOHANG) == pid) {
                    pid = -1;
                    require(WIFEXITED(status), "shell did not exit normally");
                    return;
                }
                require(Clock::now() < deadline, "Ctrl-D did not exit");
                std::this_thread::sleep_for(10ms);
            }
        }
    };

    void test(const std::string &name, const std::function<void()> &action) {
        try {
            action();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }

    int helper(int argc, char **argv) {
        const std::string mode = argc > 2 ? argv[2] : "";
        if (mode == "wait") {
            std::cout << "READY " << getpid() << ' ' << getpgrp() << ' ' << tcgetpgrp(STDIN_FILENO)
                      << '\n'
                      << std::flush;
            for (;;)
                pause();
        }
        if (mode == "burst") {
            for (int i = 0; i < 100000; ++i)
                std::cout << "record\n";
            return 0;
        }
        if (mode == "exit")
            return argc > 3 ? std::atoi(argv[3]) : 0;
        if (mode == "fds") {
            for (const auto &name : directoryEntries("/proc/self/fd")) {
                if (name != "." && name != "..") {
                    const int fd = std::atoi(name.c_str());
                    // directoryEntries has already closed its own descriptor.
                    if (fd > 2 && fcntl(fd, F_GETFD) >= 0)
                        std::cout << fd << '\n';
                }
            }
            return 0;
        }
        return 1;
    }
} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--helper")
        return helper(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: test-shell /absolute/path/posix-shell\n";
        return 2;
    }
    shellBinary = argv[1];
    testBinary = readLink("/proc/self/exe");
    signal(SIGPIPE, SIG_IGN);

    test("parser: whitespace, operators, redirects, arbitrary pipeline", [] {
        const auto commands = parse(" \t echo  first>one>>two ;cat<one|cat|wc -l & pwd; ");
        require(commands.size() == 3, "pipeline count");
        require(commands[0].commands[0].args == std::vector<std::string>({"echo", "first"}),
                "arguments");
        require(commands[0].commands[0].redirects.size() == 2, "redirects");
        require(commands[1].background && commands[1].commands.size() == 3, "background pipeline");
        require(parse(" ; ; \t").empty(), "empty separators");
    });
    test("parser rejects malformed syntax", [] {
        for (const char *line : {"echo x |", "| echo", "echo >", "echo > | cat", "echo || cat", "&",
                                 "echo && pwd", "<file"}) {
            bool rejected = false;
            try {
                parse(line);
            } catch (const std::exception &) {
                rejected = true;
            }
            require(rejected, std::string("accepted malformed syntax: ") + line);
        }
    });
    test("home path abbreviation respects directory boundaries", [] {
        require(abbreviate("/tmp/home", "/tmp/home") == "~", "home");
        require(abbreviate("/tmp/home/dir", "/tmp/home") == "~/dir", "child");
        require(abbreviate("/tmp/home-other", "/tmp/home") == "/tmp/home-other", "sibling");
        require(abbreviate("/tmp", "/") == "~/tmp", "root home");
    });
    test("core built-ins, random whitespace and literal quotes", [] {
        Fixture f;
        mkdir((f.path + "/child").c_str(), 0755);
        const auto result = run(
            f.path,
            "\t pwd ; echo \t a  b\necho \"abc 'ac' abc\"\ncd child; pwd; cd -; cd ..; pwd; cd; pwd\ncd .; cd ~; cd a b\nexit\n");
        require(result.status == 0, "exit status");
        contains(result.output, f.path + "\na b\n\"abc 'ac' abc\"\n");
        contains(result.output, f.path + "/child\n" + f.path + "\n/tmp\n" + f.path + "\n");
        contains(result.output, "Invalid arguments");
    });
    test("ls matches system long listing, flags in any order and symlinks", [] {
        Fixture f;
        mkdir((f.path + "/dir").c_str(), 0755);
        putFile(f.path + "/dir/visible", "abc\n");
        putFile(f.path + "/dir/.hidden", "hidden\n");
        symlink("visible", (f.path + "/dir/link").c_str());
        const auto actual = run(f.path, "ls -al dir\n");
        const auto expected = run(f.path, "/bin/ls -al dir\n");
        require(actual.output == expected.output,
                "long listing differs:\n" + actual.output + "expected:\n" + expected.output);
        const auto mixed = run(f.path, "ls dir -l -a dir/visible\nls -la dir dir\n");
        contains(mixed.output, "dir/visible");
        contains(mixed.output, ".hidden");
        contains(mixed.output, "dir:\n");
        const auto shortList = run(f.path, "ls dir\n");
        require(shortList.output == "link\nvisible\n", "short listing");
    });
    test("recursive search includes hidden files and avoids symlink loops", [] {
        Fixture f;
        mkdir((f.path + "/deep").c_str(), 0755);
        mkdir((f.path + "/deep/nested").c_str(), 0755);
        putFile(f.path + "/deep/nested/.needle", "x");
        symlink("../..", (f.path + "/deep/nested/cycle").c_str());
        const auto result = run(
            f.path, "search .needle; search nested; search absent; search deep/nested/.needle\n");
        require(result.output == "True\nTrue\nFalse\nTrue\n", "search results:\n" + result.output);
    });
    test("pinfo prints shell identity, virtual memory and executable", [] {
        Fixture f;
        const auto result = run(f.path, "pinfo\npinfo 999999999\npinfo invalid\n");
        contains(result.output, "pid -- ");
        contains(result.output, "Process Status -- ");
        contains(result.output, "{Virtual Memory}");
        contains(result.output, "Executable Path -- " + shellBinary);
        contains(result.output, "cannot read process");
        contains(result.output, "invalid PID");
    });
    test("truncate, append, simultaneous redirection and permissions", [] {
        Fixture f;
        const auto result = run(
            f.path,
            "echo obsolete >out; echo replacement>out; echo extra>>out; cat<out>copy\necho restored\n");
        require(result.output == "restored\n", "stdout restoration");
        require(readFile(f.path + "/copy") == "replacement\nextra\n", "redirected contents");
        struct stat info{};
        require(stat((f.path + "/out").c_str(), &info) == 0 && (info.st_mode & 0777) == 0644,
                "output mode");
    });
    test("redirection failures restore parent descriptors and cwd", [] {
        Fixture f;
        const auto result =
            run(f.path, "cd / <missing; pwd; echo x >missing-dir/file; echo recovered\n");
        contains(result.output, "No such file or directory");
        contains(result.output, f.path + "\n");
        contains(result.output, "recovered\n");
    });
    test("all built-ins support pipelines and redirection", [] {
        Fixture f;
        mkdir((f.path + "/dir").c_str(), 0755);
        putFile(f.path + "/dir/needle", "needle\n");
        const auto result = run(
            f.path,
            "echo hello|tr a-z A-Z>out; cat out\npwd|cat\nls dir|cat\nsearch needle|cat\npinfo|cat\nhistory 1|cat\ncd /|cat; pwd\nexit|cat; echo parent-alive\n");
        contains(result.output, "HELLO\n" + f.path + "\nneedle\nTrue\n");
        contains(result.output, "Executable Path -- ");
        contains(result.output, "history 1|cat");
        contains(result.output, f.path + "\nparent-alive\n");
    });
    test("large pipelines execute concurrently without descriptor leaks", [] {
        Fixture f;
        std::string script = testBinary + " --helper burst|cat|cat|wc -l\n";
        for (int i = 0; i < 40; ++i)
            script += "echo repeated|cat>/dev/null\n";
        script += testBinary + " --helper fds\n";
        const auto result = run(f.path, script);
        require(result.output == "100000\n",
                "pipeline output or leaked descriptors:\n" + result.output);
    });
    test("redirects override pipeline ends and preserve redirect order", [] {
        Fixture f;
        putFile(f.path + "/input", "from-file\n");
        const auto result = run(f.path, "echo from-pipe|cat<input; echo left>first>second|wc -c\n");
        require(result.output == "from-file\n0\n", "redirect precedence:\n" + result.output);
        require(readFile(f.path + "/first").empty() && readFile(f.path + "/second") == "left\n",
                "redirect order");
    });
    test("external errors and pipeline status propagate", [] {
        Fixture f;
        require(run(f.path, "no_such_assignment_command\n").status == 127,
                "command not found status");
        require(run(f.path, testBinary + " --helper exit 7\n").status == 7, "program status");
        require(run(f.path, "echo okay|" + testBinary + " --helper exit 9\n").status == 9,
                "last pipeline stage status");
        const auto recovery = run(f.path, "echo broken |\necho recovered\n");
        contains(recovery.output, "expected a command");
        contains(recovery.output, "recovered\n");
        putFile(f.path + "/not-executable", "hello\n");
        require(run(f.path, "./not-executable\n").status == 126, "permission status");
    });
    test("background jobs return promptly, print PIDs and are reaped", [] {
        Fixture f;
        const auto result =
            run(f.path, "sleep 0.05 & sleep 0.08 & echo prompt-returned\nsleep 0.15\njobs\n");
        contains(result.output, "[1] ");
        contains(result.output, "[2] ");
        contains(result.output, "prompt-returned\n");
        contains(result.output, "Done sleep 0.05");
        contains(result.output, "Done sleep 0.08");
        require(result.output.find("Running") == std::string::npos, "finished jobs remain");
        const auto start = Clock::now();
        run(f.path, "sleep 10 &\nexit\n");
        require(Clock::now() - start < 1000ms, "background command blocked shell");
    });
    test("nested shells retain unread stdin and parent continues", [] {
        Fixture f;
        const auto result = run(f.path, shellBinary + "\necho nested\nexit\necho parent\nexit\n");
        require(result.output == "nested\nparent\n", "nested stdin ownership:\n" + result.output);
    });
    test("history persists, caps at twenty and skips consecutive duplicates", [] {
        Fixture f;
        std::string script;
        for (int i = 0; i < 25; ++i)
            script += "echo command" + std::to_string(i) + "\n";
        script += "echo command24\n";
        run(f.path, script);
        History saved(f.path + "/.posix_history");
        require(saved.entries().size() == 20, "history capacity");
        require(saved.entries().front() == "echo command5", "oldest history entry");
        require(saved.entries().back() == "echo command24", "newest history entry");
        const auto result = run(f.path, "history 3\n");
        require(result.output == "echo command23\necho command24\nhistory 3\n",
                "persisted history count");
        const auto defaults = run(f.path, "history\n");
        require(std::count(defaults.output.begin(), defaults.output.end(), '\n') == 10,
                "default history limit");
        const auto extended = run(f.path, "history 19\n");
        require(std::count(extended.output.begin(), extended.output.end(), '\n') == 19,
                "extended history count");
    });
    test("completion matches built-ins, PATH, hidden files and paths", [] {
        Fixture f;
        const auto before = currentDirectory();
        chdir(f.path.c_str());
        putFile("alpha.txt", "a");
        putFile("alnum.txt", "b");
        putFile(".hidden", "c");
        mkdir("folder", 0755);
        const auto command = completions("pinf", 4, f.path);
        const auto files = completions("cat al", 6, f.path);
        const auto directory = completions("cat fol", 7, f.path);
        const auto hidden = completions("cat .hi", 7, f.path);
        const auto path = completions("cat ~/alp", 9, f.path);
        chdir(before.c_str());
        require(command == std::vector<std::string>({"pinfo"}), "built-in completion");
        require(files == std::vector<std::string>({"alnum.txt", "alpha.txt"}), "file completion");
        require(directory == std::vector<std::string>({"folder/"}), "directory suffix");
        require(hidden == std::vector<std::string>({".hidden"}), "hidden completion");
        require(path == std::vector<std::string>({"~/alpha.txt"}), "home completion");
    });
    test("terminal: file/command completion, common prefix and suggestions", [] {
        Fixture f;
        putFile(f.path + "/alpha.txt", "alpha-content\n");
        putFile(f.path + "/alnum.txt", "alnum-content\n");
        symlink("/bin/echo", (f.path + "/zz_assignment_command").c_str());
        Pty p(f.path, f.path);
        p.command("cat alp\t", "alpha-content\r\n");
        p.send("cat a\t\t");
        p.expect("alnum.txt  alpha.txt");
        p.send(std::string(1, 21));
        p.command("zz_ass\t completion-from-path", "completion-from-path\r\n");
        p.command("pinf\t", "Executable Path -- ");
        p.logout();
    });
    test("terminal: editable up/down history and navigation bounds", [] {
        Fixture f;
        Pty p(f.path);
        p.command("echo first", "first\r\n");
        p.command("echo second", "second\r\n");
        p.command("\033[A!", "second!\r\n");
        p.command("\033[A\033[A\033[B?", "second!?\r\n");
        p.send("\033[A\033[A\033[A\033[A\033[A\n");
        p.expect("first\r\n");
        p.expect("> ");
        p.command("draft\033[A\033[B\033[B" + std::string(1, 21) + "echo down-ok", "down-ok\r\n");
        p.logout();
    });
    test("terminal: idle Ctrl-C/Ctrl-Z and Ctrl-D logout", [] {
        Fixture f;
        Pty p(f.path);
        p.send("echo discarded" + std::string(1, 3));
        p.expect("^C\r\n");
        p.expect("> ");
        p.command(std::string(1, 26) + "echo alive", "alive\r\n");
        p.logout();
    });
    test("terminal: foreground Ctrl-C interrupts child and preserves shell", [] {
        Fixture f;
        Pty p(f.path);
        p.send(testBinary + " --helper wait\n");
        p.expect("READY ");
        const std::string identity = p.expect("\r\n");
        std::istringstream fields(identity);
        pid_t child, group, foreground;
        fields >> child >> group >> foreground;
        require(child == group && group == foreground && group != p.pid,
                "terminal foreground ownership");
        p.send(std::string(1, 3));
        p.expect("> ");
        p.command("echo survived", "survived\r\n");
        p.logout();
    });
    test("terminal: Ctrl-Z stops pipeline; pinfo, bg and fg work", [] {
        Fixture f;
        Pty p(f.path);
        p.send(testBinary + " --helper wait | cat\n");
        p.expect("READY ");
        const std::string identity = p.expect("\r\n");
        std::istringstream fields(identity);
        pid_t child = 0;
        fields >> child;
        p.send(std::string(1, 26));
        p.expect("Stopped");
        p.expect("> ");
        p.command("jobs", "Stopped");
        p.command("pinfo " + std::to_string(child), "Process Status -- T");
        p.command("bg", "--helper wait | cat");
        p.command("jobs", "Running");
        p.send("fg\n");
        std::this_thread::sleep_for(80ms);
        p.send(std::string(1, 3));
        p.expect("> ");
        p.command("echo resumed", "resumed\r\n");
        p.logout();
    });
    test("terminal: async background completion preserves typed input", [] {
        Fixture f;
        Pty p(f.path);
        p.send("sleep 0.15 &\n");
        p.expect("[1] ");
        p.expect("> ");
        p.send("echo retained");
        p.expect("Done sleep 0.15");
        p.send("\n");
        p.expect("retained\r\n");
        p.expect("> ");
        p.logout();
    });
    test("terminal: foreground pinfo includes plus marker and nested shell runs", [] {
        Fixture f;
        Pty p(f.path);
        p.command("pinfo", "Process Status -- R+");
        p.send(shellBinary + "\n");
        p.expect("> ");
        p.command("echo nested-pty", "nested-pty\r\n");
        p.send(std::string(1, 4));
        p.expect("> ");
        p.command("echo parent-pty", "parent-pty\r\n");
        p.logout();
    });

    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
