#include "shell.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pwd.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace posix_shell {
    namespace {
        volatile sig_atomic_t signalWriteFd = -1;
        volatile sig_atomic_t interrupted = 0;

        void signalHandler(int number) {
            const int saved = errno;
            if (number == SIGINT)
                interrupted = 1;
            if (signalWriteFd >= 0) {
                const char event = static_cast<char>(number);
                const ssize_t ignored = write(signalWriteFd, &event, 1);
                (void)ignored;
            }
            errno = saved;
        }

        void setSignal(int number, void (*handler)(int)) {
            struct sigaction action{};
            action.sa_handler = handler;
            sigemptyset(&action.sa_mask);
            if (sigaction(number, &action, nullptr) < 0)
                throw std::runtime_error("sigaction failed");
        }

        bool terminalGroup(pid_t group) {
            int result;
            do {
                result = tcsetpgrp(STDIN_FILENO, group);
            } while (result < 0 && errno == EINTR);
            if (result < 0)
                reportError("tcsetpgrp");
            return result == 0;
        }

        bool applyRedirects(const Command &command, const std::string &home) {
            for (const auto &redirect : command.redirects) {
                const std::string path = expandHome(redirect.path, home);
                const bool input = redirect.kind == RedirectKind::Input;
                const int flags =
                    input ? O_RDONLY
                          : O_WRONLY | O_CREAT |
                                (redirect.kind == RedirectKind::Append ? O_APPEND : O_TRUNC);
                const int fd = open(path.c_str(), flags | O_CLOEXEC, 0644);
                if (fd < 0) {
                    reportError(path);
                    return false;
                }
                const int target = input ? STDIN_FILENO : STDOUT_FILENO;
                if (fd != target) {
                    if (dup2(fd, target) < 0) {
                        reportError("dup2");
                        close(fd);
                        return false;
                    }
                    close(fd);
                } else {
                    fcntl(fd, F_SETFD, 0);
                }
            }
            return true;
        }

        // A standalone built-in runs in the parent. Save its descriptors so cd can
        // change the shell directory while redirection never leaks into later commands.
        class SavedDescriptors {
            public:
            explicit SavedDescriptors(bool needed) {
                if (!needed)
                    return;
                std::cout.flush();
                std::cerr.flush();
                input_ = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
                output_ = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
                if (input_ < 0 || output_ < 0) {
                    if (input_ >= 0)
                        close(input_);
                    if (output_ >= 0)
                        close(output_);
                    throw std::runtime_error("cannot save standard descriptors");
                }
            }
            ~SavedDescriptors() {
                std::cout.flush();
                std::cerr.flush();
                if (input_ >= 0) {
                    dup2(input_, STDIN_FILENO);
                    close(input_);
                }
                if (output_ >= 0) {
                    dup2(output_, STDOUT_FILENO);
                    close(output_);
                }
                std::cout.clear();
                std::cin.clear();
            }

            private:
            int input_ = -1, output_ = -1;
        };
    } // namespace

    Shell::Shell()
        : home_(currentDirectory()), history_(home_ + "/.posix_history"), shellPid_(getpid()),
          shellGroup_(getpgrp()), interactive_(isatty(STDIN_FILENO)) {
        initializeSignals();
    }

    Shell::~Shell() {
        shutdownJobs();
        signalWriteFd = -1;
        for (int fd : signalPipe_)
            if (fd >= 0)
                close(fd);
        if (interactive_) {
            terminalGroup(shellGroup_);
            tcsetattr(STDIN_FILENO, TCSADRAIN, &originalModes_);
        }
    }

    void Shell::initializeSignals() {
        if (interactive_) {
            setSignal(SIGTTIN, SIG_DFL);
            while (tcgetpgrp(STDIN_FILENO) != shellGroup_) {
                if (kill(-shellGroup_, SIGTTIN) < 0)
                    throw std::runtime_error("cannot acquire terminal");
            }
            shellGroup_ = shellPid_;
            if (setpgid(0, shellGroup_) < 0 && getpgrp() != shellGroup_)
                throw std::runtime_error("cannot create shell process group");
        }
        for (int signal : {SIGTTOU, SIGTTIN, SIGTSTP, SIGQUIT, SIGPIPE})
            setSignal(signal, SIG_IGN);
        if (interactive_) {
            if (!terminalGroup(shellGroup_) || tcgetattr(STDIN_FILENO, &originalModes_) < 0)
                throw std::runtime_error("cannot initialize terminal");
        }
        if (pipe(signalPipe_) < 0)
            throw std::runtime_error("cannot create signal pipe");
        for (int fd : signalPipe_) {
            if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
                throw std::runtime_error("cannot configure signal pipe");
        }
        signalWriteFd = signalPipe_[1];
        setSignal(SIGINT, signalHandler);
        setSignal(SIGCHLD, signalHandler);
    }

    std::string Shell::prompt() const {
        const passwd *user = getpwuid(getuid());
        char host[256]{};
        if (gethostname(host, sizeof(host) - 1) < 0)
            std::strcpy(host, "unknown");
        std::string directory;
        try {
            directory = abbreviate(currentDirectory(), home_);
        } catch (const std::exception &) {
            directory = "[unavailable]";
        }
        return std::string(user ? user->pw_name : "unknown") + "@" + host + ":" + directory + "> ";
    }

    bool Shell::done(const Job &job) {
        return std::all_of(job.processes.begin(), job.processes.end(),
                           [](const Process &p) { return p.done; });
    }

    bool Shell::stopped(const Job &job) {
        return !done(job) && std::all_of(job.processes.begin(), job.processes.end(),
                                         [](const Process &p) { return p.done || p.stopped; });
    }

    void Shell::updateProcess(pid_t pid, int status) {
        for (auto &job : jobs_) {
            for (auto &process : job.processes) {
                if (process.pid != pid)
                    continue;
                process.status = status;
                if (WIFSTOPPED(status))
                    process.stopped = true;
                else if (WIFCONTINUED(status))
                    process.stopped = false;
                else {
                    process.done = true;
                    process.stopped = false;
                }
                return;
            }
        }
    }

    void Shell::reapChildren(bool notify) {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
            updateProcess(pid, status);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (!it->background) {
                ++it;
                continue;
            }
            if (done(*it)) {
                if (notify) {
                    const int last = it->processes.back().status;
                    std::cout << '[' << it->id << "] " << it->group << ' '
                              << (WIFSIGNALED(last)                           ? "Terminated"
                                  : WIFEXITED(last) && WEXITSTATUS(last) != 0 ? "Failed"
                                                                              : "Done")
                              << " " << it->description << '\n';
                }
                it = jobs_.erase(it);
            } else {
                if (notify && stopped(*it) && !it->reportedStopped) {
                    std::cout << '[' << it->id << "] " << it->group << " Stopped "
                              << it->description << '\n';
                    it->reportedStopped = true;
                }
                if (!stopped(*it))
                    it->reportedStopped = false;
                ++it;
            }
        }
        std::cout.flush();
    }

    void Shell::processEvents() {
        char buffer[128];
        while (read(signalPipe_[0], buffer, sizeof(buffer)) > 0) {
        }
        reapChildren();
    }

    int Shell::waitForeground(int jobId, bool resume) {
        auto it = std::find_if(jobs_.begin(), jobs_.end(),
                               [=](const Job &job) { return job.id == jobId; });
        if (it == jobs_.end())
            return 1;
        Job &job = *it;
        job.background = false;
        if (interactive_) {
            terminalGroup(job.group);
            if (resume && job.hasModes)
                tcsetattr(STDIN_FILENO, TCSADRAIN, &job.modes);
        }
        if (resume) {
            if (kill(-job.group, SIGCONT) < 0)
                reportError("fg");
            for (auto &process : job.processes)
                process.stopped = false;
        }
        while (!done(job) && !stopped(job)) {
            int status;
            const pid_t pid = waitpid(-job.group, &status, WUNTRACED);
            if (pid > 0)
                updateProcess(pid, status);
            else if (pid < 0 && errno == EINTR) {
                if (interrupted) {
                    kill(-job.group, SIGINT);
                    interrupted = 0;
                }
            } else {
                if (pid < 0 && errno != ECHILD)
                    reportError("waitpid");
                break;
            }
        }
        if (interactive_) {
            if (stopped(job))
                job.hasModes = tcgetattr(STDIN_FILENO, &job.modes) == 0;
            terminalGroup(shellGroup_);
            tcsetattr(STDIN_FILENO, TCSADRAIN, &originalModes_);
        }
        const int result = job.processes.back().status;
        if (stopped(job)) {
            job.background = true;
            job.reportedStopped = true;
            std::cout << '[' << job.id << "] " << job.group << " Stopped " << job.description
                      << '\n';
        } else {
            jobs_.erase(it);
        }
        return WIFEXITED(result)     ? WEXITSTATUS(result)
               : WIFSIGNALED(result) ? 128 + WTERMSIG(result)
                                     : 128 + SIGTSTP;
    }

    int Shell::execute(const Pipeline &pipeline) {
        const auto &commands = pipeline.commands;
        if (commands.size() == 1 && isBuiltin(commands[0].args[0]) && !pipeline.background) {
            SavedDescriptors saved(!commands[0].redirects.empty());
            return applyRedirects(commands[0], home_) ? builtin(commands[0]) : 1;
        }
        std::cout.flush();
        std::cerr.flush();
        sigset_t blocked, previous;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGCHLD);
        sigprocmask(SIG_BLOCK, &blocked, &previous);
        // Keep children behind a launch gate until all stages have joined their
        // process group and the parent has assigned the foreground terminal.
        int gate[2];
        if (pipe(gate) < 0) {
            reportError("pipe");
            sigprocmask(SIG_SETMASK, &previous, nullptr);
            return 1;
        }
        fcntl(gate[0], F_SETFD, FD_CLOEXEC);
        fcntl(gate[1], F_SETFD, FD_CLOEXEC);
        Job job{nextJob_++, 0, {}, pipeline.description, pipeline.background, false, {}, false};
        int input = -1;
        bool failed = false;
        for (std::size_t i = 0; i < commands.size(); ++i) {
            int output[2]{-1, -1};
            if (i + 1 < commands.size() && pipe(output) < 0) {
                reportError("pipe");
                failed = true;
                break;
            }
            const pid_t pid = fork();
            if (pid == 0) {
                close(gate[1]);
                const pid_t group = job.group == 0 ? getpid() : job.group;
                if (setpgid(0, group) < 0)
                    _exit(1);
                for (int signal : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE})
                    setSignal(signal, SIG_DFL);
                sigprocmask(SIG_SETMASK, &previous, nullptr);
                close(signalPipe_[0]);
                close(signalPipe_[1]);
                if (input >= 0) {
                    if (dup2(input, STDIN_FILENO) < 0)
                        _exit(1);
                    close(input);
                }
                if (output[1] >= 0) {
                    if (dup2(output[1], STDOUT_FILENO) < 0)
                        _exit(1);
                    close(output[1]);
                }
                if (output[0] >= 0)
                    close(output[0]);
                char byte;
                while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {
                }
                close(gate[0]);
                const Command &command = commands[i];
                if (!applyRedirects(command, home_))
                    _exit(1);
                if (isBuiltin(command.args[0])) {
                    int result = 1;
                    try {
                        result = builtin(command, true);
                    } catch (const std::exception &error) {
                        std::cerr << command.args[0] << ": " << error.what() << '\n';
                    }
                    std::cout.flush();
                    std::cerr.flush();
                    _exit(result);
                }
                std::vector<std::string> expanded;
                for (const auto &arg : command.args)
                    expanded.push_back(expandHome(arg, home_));
                std::vector<char *> args;
                for (auto &arg : expanded)
                    args.push_back(arg.data());
                args.push_back(nullptr);
                execvp(args[0], args.data());
                const int saved = errno;
                reportError(command.args[0]);
                _exit(saved == ENOENT ? 127 : 126);
            }
            if (input >= 0) {
                close(input);
                input = -1;
            }
            if (output[1] >= 0)
                close(output[1]);
            if (pid < 0) {
                reportError("fork");
                if (output[0] >= 0)
                    close(output[0]);
                failed = true;
                break;
            }
            if (job.group == 0)
                job.group = pid;
            if (setpgid(pid, job.group) < 0 && errno != EACCES && errno != ESRCH)
                reportError("setpgid");
            job.processes.push_back({pid, 0, false, false});
            input = output[0];
        }
        if (input >= 0)
            close(input);
        close(gate[0]);
        if (failed) {
            if (job.group > 0)
                kill(-job.group, SIGKILL);
            close(gate[1]);
            for (const auto &process : job.processes)
                while (waitpid(process.pid, nullptr, 0) < 0 && errno == EINTR) {
                }
            sigprocmask(SIG_SETMASK, &previous, nullptr);
            return 1;
        }
        const int id = job.id;
        jobs_.push_back(std::move(job));
        if (!pipeline.background && interactive_)
            terminalGroup(jobs_.back().group);
        close(gate[1]);
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        if (pipeline.background) {
            std::cout << '[' << id << "] " << jobs_.back().group << '\n';
            return 0;
        }
        return waitForeground(id);
    }

    int Shell::jobBuiltin(const Command &command, bool child) {
        const auto &args = command.args;
        if (args[0] == "jobs") {
            if (args.size() != 1) {
                std::cerr << "jobs: Invalid arguments\n";
                return 1;
            }
            if (!child)
                reapChildren();
            for (const auto &job : jobs_) {
                std::cout << '[' << job.id << "] " << job.group << ' '
                          << (stopped(job) ? "Stopped" : "Running") << ' ' << job.description
                          << '\n';
            }
            return 0;
        }
        if (child) {
            std::cerr << args[0] << ": requires the parent shell\n";
            return 1;
        }
        if (args.size() > 2) {
            std::cerr << args[0] << ": Invalid arguments\n";
            return 1;
        }
        reapChildren();
        auto selected = jobs_.end();
        if (args.size() == 1) {
            if (!jobs_.empty())
                selected = jobs_.end() - 1;
        } else {
            try {
                const bool jobId = !args[1].empty() && args[1][0] == '%';
                const std::string number = args[1].substr(jobId ? 1 : 0);
                if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("job");
                const long value = std::stol(number);
                selected = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job &job) {
                    return jobId ? job.id == value : job.group == value;
                });
            } catch (const std::exception &) {
            }
        }
        if (selected == jobs_.end()) {
            std::cerr << args[0] << ": no such job\n";
            return 1;
        }
        if (args[0] == "fg")
            return waitForeground(selected->id, true);
        if (kill(-selected->group, SIGCONT) < 0) {
            reportError("bg");
            return 1;
        }
        for (auto &process : selected->processes)
            process.stopped = false;
        selected->reportedStopped = false;
        std::cout << '[' << selected->id << "] " << selected->group << ' ' << selected->description
                  << '\n';
        return 0;
    }

    void Shell::shutdownJobs() {
        // Logout signals both running and stopped groups, without waiting forever
        // for a program which deliberately ignores SIGHUP.
        for (const auto &job : jobs_) {
            if (!done(job)) {
                kill(-job.group, SIGHUP);
                kill(-job.group, SIGCONT);
            }
        }
        reapChildren(false);
    }

    int Shell::run() {
        std::string line;
        while (!exiting_) {
            processEvents();
            if (interrupted) {
                interrupted = 0;
                std::cin.clear();
            }
            if (interactive_)
                std::cout << prompt() << std::flush;
            if (!std::getline(std::cin, line))
                break;
            history_.add(line);
            try {
                for (const auto &pipeline : parse(line)) {
                    status_ = execute(pipeline);
                    reapChildren();
                    if (exiting_)
                        break;
                }
            } catch (const std::exception &error) {
                std::cerr << "shell: " << error.what() << '\n';
                status_ = 2;
            }
        }
        return status_;
    }
} // namespace posix_shell
