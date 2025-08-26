#include "shell.hpp"
#include "util.hpp"

#include <algorithm>
#include <iostream>
#include <unistd.h>

namespace posix_shell {
    bool Shell::isBuiltin(const std::string &name) const {
        return name == "cd" || name == "echo" || name == "pwd" || name == "history" ||
               name == "exit";
    }

    int Shell::builtin(const Command &command, bool child) {
        const auto &args = command.args;
        const auto &name = args[0];
        if (name == "cd") {
            if (args.size() > 2) {
                std::cerr << "Invalid arguments\n";
                return 1;
            }
            const bool previous = args.size() == 2 && args[1] == "-";
            if (previous && previous_.empty()) {
                std::cerr << "cd: no previous directory\n";
                return 1;
            }
            const std::string target = args.size() == 1 ? home_
                                       : previous       ? previous_
                                                        : expandHome(args[1], home_);
            const std::string before = currentDirectory();
            if (chdir(target.c_str()) < 0) {
                reportError("cd: " + target);
                return 1;
            }
            previous_ = before;
            if (previous)
                std::cout << currentDirectory() << '\n';
            return 0;
        }
        if (name == "pwd") {
            if (args.size() != 1) {
                std::cerr << "pwd: Invalid arguments\n";
                return 1;
            }
            std::cout << currentDirectory() << '\n';
            return 0;
        }
        if (name == "echo") {
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (i > 1)
                    std::cout << ' ';
                std::cout << args[i];
            }
            std::cout << '\n';
            return 0;
        }
        if (name == "history") {
            std::size_t count = 10;
            try {
                if (args.size() > 2)
                    throw std::invalid_argument("arguments");
                if (args.size() == 2) {
                    if (args[1].empty() ||
                        args[1].find_first_not_of("0123456789") != std::string::npos)
                        throw std::invalid_argument("count");
                    count = std::stoul(args[1]);
                    if (count > 20)
                        throw std::out_of_range("count");
                }
            } catch (const std::exception &) {
                std::cerr << "history: expected a count from 0 to 20\n";
                return 1;
            }
            const auto &entries = history_.entries();
            count = std::min(count, entries.size());
            for (std::size_t i = entries.size() - count; i < entries.size(); ++i)
                std::cout << entries[i] << '\n';
            return 0;
        }
        if (name == "exit") {
            if (args.size() != 1) {
                std::cerr << "exit: Invalid arguments\n";
                return 1;
            }
            if (!child)
                exiting_ = true;
            return 0;
        }
        return 1;
    }
} // namespace posix_shell
