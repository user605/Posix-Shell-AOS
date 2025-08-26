#pragma once

#include "history.hpp"
#include "parser.hpp"
#include <string>

namespace posix_shell {
    class Shell {
        public:
        Shell();
        int run();

        private:
        int execute(const Pipeline &pipeline);
        int builtin(const Command &command, bool child = false);
        bool isBuiltin(const std::string &name) const;
        std::string prompt() const;

        std::string home_;
        std::string previous_;
        History history_;
        bool exiting_ = false;
        int status_ = 0;
    };
} // namespace posix_shell
