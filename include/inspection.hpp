#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

namespace posix_shell {
    int listFiles(const std::vector<std::string> &args, const std::string &home);
    int processInfo(const std::vector<std::string> &args, const std::string &home, pid_t shellPid);
    int searchFiles(const std::vector<std::string> &args);
} // namespace posix_shell
