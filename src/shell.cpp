#include "shell.hpp"
#include "util.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace posix_shell {
    Shell::Shell() : home_(currentDirectory()), history_(home_ + "/.posix_history") {}

    std::string Shell::prompt() const {
        const passwd *user = getpwuid(getuid());
        char host[256]{};
        if (gethostname(host, sizeof(host) - 1) < 0)
            std::strcpy(host, "unknown");
        return std::string(user ? user->pw_name : "unknown") + "@" + host + ":" +
               abbreviate(currentDirectory(), home_) + "> ";
    }

    int Shell::execute(const Pipeline &pipeline) {
        if (pipeline.commands.size() != 1 || pipeline.background ||
            !pipeline.commands[0].redirects.empty()) {
            std::cerr
                << "shell: pipelines, redirection and background jobs are not implemented yet\n";
            return 2;
        }
        const Command &command = pipeline.commands[0];
        if (isBuiltin(command.args[0]))
            return builtin(command);
        const pid_t child = fork();
        if (child < 0) {
            reportError("fork");
            return 1;
        }
        if (child == 0) {
            std::vector<char *> args;
            for (const auto &arg : command.args)
                args.push_back(const_cast<char *>(arg.c_str()));
            args.push_back(nullptr);
            execvp(args[0], args.data());
            const int saved = errno;
            reportError(command.args[0]);
            _exit(saved == ENOENT ? 127 : 126);
        }
        int result = 0;
        while (waitpid(child, &result, 0) < 0) {
            if (errno == EINTR)
                continue;
            reportError("waitpid");
            return 1;
        }
        return WIFEXITED(result) ? WEXITSTATUS(result) : 128 + WTERMSIG(result);
    }

    int Shell::run() {
        std::string line;
        while (!exiting_) {
            if (isatty(STDIN_FILENO))
                std::cout << prompt() << std::flush;
            if (!std::getline(std::cin, line))
                break;
            history_.add(line);
            try {
                for (const auto &pipeline : parse(line)) {
                    status_ = execute(pipeline);
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
