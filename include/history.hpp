#pragma once

#include <string>
#include <vector>

namespace posix_shell {
    class History {
        public:
        explicit History(std::string path);
        void add(const std::string &line);
        const std::vector<std::string> &entries() const {
            return entries_;
        }

        private:
        void load();
        std::string path_;
        std::vector<std::string> entries_;
    };
} // namespace posix_shell
