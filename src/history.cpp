#include "history.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace posix_shell {
    History::History(std::string path) : path_(std::move(path)) {
        load();
    }

    void History::load() {
        entries_.clear();
        std::ifstream input(path_);
        std::string line;
        while (std::getline(input, line)) {
            line = trim(line);
            if (!line.empty() && (entries_.empty() || entries_.back() != line))
                entries_.push_back(line);
        }
        if (entries_.size() > 20)
            entries_.erase(entries_.begin(), entries_.end() - 20);
    }

    void History::add(const std::string &value) {
        const std::string line = trim(value);
        if (line.empty())
            return;
        // A separate lock inode keeps concurrent sessions serialized even though
        // the data file is replaced atomically by rename().
        const int lockFd = open((path_ + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        struct flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (lockFd < 0) {
            reportError("history");
        } else {
            while (fcntl(lockFd, F_SETLKW, &lock) < 0) {
                if (errno == EINTR)
                    continue;
                reportError("history lock");
                close(lockFd);
                return;
            }
            load();
        }
        if (entries_.empty() || entries_.back() != line)
            entries_.push_back(line);
        if (entries_.size() > 20)
            entries_.erase(entries_.begin());

        if (lockFd >= 0) {
            std::string temporary = path_ + ".tmp.XXXXXX";
            std::vector<char> name(temporary.begin(), temporary.end());
            name.push_back('\0');
            const int fd = mkstemp(name.data());
            if (fd < 0) {
                reportError("history");
            } else {
                std::string data;
                for (const auto &entry : entries_)
                    data += entry + "\n";
                if (!writeAll(fd, data) || fsync(fd) < 0 ||
                    rename(name.data(), path_.c_str()) < 0) {
                    reportError("history");
                }
                close(fd);
                unlink(name.data());
            }
            close(lockFd);
        }
    }
} // namespace posix_shell
