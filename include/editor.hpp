#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace posix_shell {
    std::vector<std::string> completions(const std::string &line, std::size_t cursor,
                                         const std::string &home);
    std::optional<std::string> readInteractiveLine(const std::string &prompt,
                                                   const std::vector<std::string> &history,
                                                   const std::string &home, int signalFd,
                                                   const std::function<bool()> &onEvent);
} // namespace posix_shell
