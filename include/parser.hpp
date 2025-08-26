#pragma once

#include <string>
#include <vector>

namespace posix_shell {

    enum class RedirectKind { Input, Output, Append };
    struct Redirect {
        RedirectKind kind;
        std::string path;
    };
    struct Command {
        std::vector<std::string> args;
        std::vector<Redirect> redirects;
    };
    struct Pipeline {
        std::vector<Command> commands;
        bool background = false;
        std::string description;
    };

    // Operators are always special, as specified by the assignment. Quotes in
    // ordinary arguments are retained literally; there is no expansion language.
    std::vector<Pipeline> parse(const std::string &line);

} // namespace posix_shell
