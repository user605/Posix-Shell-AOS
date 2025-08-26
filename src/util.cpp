#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace posix_shell {
    std::string currentDirectory() {
        std::vector<char> buffer(256);
        while (!getcwd(buffer.data(), buffer.size())) {
            if (errno != ERANGE)
                throw std::runtime_error(std::string("getcwd: ") + std::strerror(errno));
            buffer.resize(buffer.size() * 2);
        }
        return buffer.data();
    }

    std::string abbreviate(const std::string &path, const std::string &home) {
        if (path == home)
            return "~";
        if (home == "/" && !path.empty() && path[0] == '/')
            return "~" + path;
        if (path.compare(0, home.size() + 1, home + "/") == 0)
            return "~" + path.substr(home.size());
        return path;
    }

    std::string expandHome(const std::string &path, const std::string &home) {
        if (path == "~")
            return home;
        if (path.compare(0, 2, "~/") == 0)
            return home + path.substr(1);
        return path;
    }

    std::string trim(const std::string &value) {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return {};
        return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    }

    std::string readLink(const std::string &path) {
        std::vector<char> buffer(256);
        for (;;) {
            const ssize_t length = readlink(path.c_str(), buffer.data(), buffer.size());
            if (length < 0)
                return {};
            if (static_cast<std::size_t>(length) < buffer.size()) {
                return std::string(buffer.data(), static_cast<std::size_t>(length));
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    bool writeAll(int fd, const std::string &text) {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const ssize_t count = write(fd, text.data() + offset, text.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    std::vector<std::string> directoryEntries(const std::string &path) {
        DIR *dir = opendir(path.c_str());
        if (!dir)
            throw std::runtime_error(path + ": " + std::strerror(errno));
        std::vector<std::string> result;
        errno = 0;
        while (const dirent *entry = readdir(dir)) {
            result.emplace_back(entry->d_name);
            errno = 0;
        }
        const int saved = errno;
        closedir(dir);
        if (saved)
            throw std::runtime_error(path + ": " + std::strerror(saved));
        std::sort(result.begin(), result.end());
        return result;
    }

    void reportError(const std::string &context) {
        const int saved = errno;
        std::cerr << context << ": " << std::strerror(saved) << '\n';
    }
} // namespace posix_shell
