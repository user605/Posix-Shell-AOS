#pragma once

#include <string>
#include <vector>

namespace posix_shell {
    std::string currentDirectory();
    std::string abbreviate(const std::string &path, const std::string &home);
    std::string expandHome(const std::string &path, const std::string &home);
    std::string trim(const std::string &value);
    std::string readLink(const std::string &path);
    bool writeAll(int fd, const std::string &text);
    std::vector<std::string> directoryEntries(const std::string &path);
    void reportError(const std::string &context);
} // namespace posix_shell
