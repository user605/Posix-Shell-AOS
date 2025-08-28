#include "inspection.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <fstream>
#include <grp.h>
#include <iomanip>
#include <iostream>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace posix_shell {
    namespace {
        struct Entry {
            std::string name;
            std::string path;
            struct stat info{};
        };

        bool collate(const std::string &a, const std::string &b) {
            const int comparison = std::strcoll(a.c_str(), b.c_str());
            return comparison == 0 ? a < b : comparison < 0;
        }

        std::string permissions(mode_t mode) {
            std::string result = "----------";
            if (S_ISDIR(mode))
                result[0] = 'd';
            else if (S_ISLNK(mode))
                result[0] = 'l';
            else if (S_ISCHR(mode))
                result[0] = 'c';
            else if (S_ISBLK(mode))
                result[0] = 'b';
            else if (S_ISFIFO(mode))
                result[0] = 'p';
            else if (S_ISSOCK(mode))
                result[0] = 's';
            const mode_t bits[] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                                   S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
            const char *letters = "rwxrwxrwx";
            for (std::size_t i = 0; i < 9; ++i)
                if (mode & bits[i])
                    result[i + 1] = letters[i];
            if (mode & S_ISUID)
                result[3] = mode & S_IXUSR ? 's' : 'S';
            if (mode & S_ISGID)
                result[6] = mode & S_IXGRP ? 's' : 'S';
            if (mode & S_ISVTX)
                result[9] = mode & S_IXOTH ? 't' : 'T';
            return result;
        }

        std::string owner(uid_t id) {
            const passwd *entry = getpwuid(id);
            return entry ? entry->pw_name : std::to_string(id);
        }
        std::string group(gid_t id) {
            const struct group *entry = getgrgid(id);
            return entry ? entry->gr_name : std::to_string(id);
        }
        std::string sizeField(const struct stat &info) {
            if (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)) {
                return std::to_string(major(info.st_rdev)) + ", " +
                       std::to_string(minor(info.st_rdev));
            }
            return std::to_string(info.st_size);
        }

        void printEntries(const std::vector<Entry> &entries, bool longForm, bool directory) {
            if (!longForm) {
                for (const auto &entry : entries)
                    std::cout << entry.name << '\n';
                return;
            }
            std::size_t links = 1, users = 1, groups = 1, sizes = 1;
            blkcnt_t blocks = 0;
            for (const auto &entry : entries) {
                const auto &info = entry.info;
                links = std::max(links, std::to_string(info.st_nlink).size());
                users = std::max(users, owner(info.st_uid).size());
                groups = std::max(groups, group(info.st_gid).size());
                sizes = std::max(sizes, sizeField(info).size());
                blocks += info.st_blocks;
            }
            if (directory)
                std::cout << "total " << (blocks / 2) << '\n';
            const time_t now = time(nullptr);
            for (const auto &entry : entries) {
                const auto &info = entry.info;
                struct tm modified{};
                localtime_r(&info.st_mtime, &modified);
                char date[128]{};
                const bool recent = difftime(now, info.st_mtime) < 31556952.0 / 2 &&
                                    difftime(info.st_mtime, now) <= 0;
                strftime(date, sizeof(date), recent ? "%b %e %H:%M" : "%b %e  %Y", &modified);
                std::cout << permissions(info.st_mode) << ' ' << std::right
                          << std::setw(static_cast<int>(links)) << info.st_nlink << ' ' << std::left
                          << std::setw(static_cast<int>(users)) << owner(info.st_uid) << ' '
                          << std::setw(static_cast<int>(groups)) << group(info.st_gid) << ' '
                          << std::right << std::setw(static_cast<int>(sizes)) << sizeField(info)
                          << ' ' << date << ' ' << entry.name;
                if (S_ISLNK(info.st_mode))
                    std::cout << " -> " << readLink(entry.path);
                std::cout << '\n';
            }
        }
    } // namespace

    int listFiles(const std::vector<std::string> &args, const std::string &home) {
        bool all = false, longForm = false, options = true;
        std::vector<std::string> paths;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (options && args[i] == "--") {
                options = false;
                continue;
            }
            if (options && args[i].size() > 1 && args[i][0] == '-') {
                for (std::size_t j = 1; j < args[i].size(); ++j) {
                    if (args[i][j] == 'a')
                        all = true;
                    else if (args[i][j] == 'l')
                        longForm = true;
                    else {
                        std::cerr << "ls: invalid option -- '" << args[i][j] << "'\n";
                        return 2;
                    }
                }
            } else
                paths.push_back(expandHome(args[i], home));
        }
        if (paths.empty())
            paths.push_back(".");
        std::sort(paths.begin(), paths.end(), collate);
        std::vector<Entry> files;
        std::vector<std::string> directories;
        int status = 0;
        for (const auto &path : paths) {
            Entry entry{path, path, {}};
            if (lstat(path.c_str(), &entry.info) < 0) {
                reportError("ls: " + path);
                status = 1;
                continue;
            }
            if (!longForm && S_ISLNK(entry.info.st_mode)) {
                struct stat target{};
                if (stat(path.c_str(), &target) == 0 && S_ISDIR(target.st_mode))
                    entry.info = target;
            }
            if (S_ISDIR(entry.info.st_mode))
                directories.push_back(path);
            else
                files.push_back(entry);
        }
        printEntries(files, longForm, false);
        bool printed = !files.empty();
        for (const auto &path : directories) {
            if (printed)
                std::cout << '\n';
            if (paths.size() > 1)
                std::cout << path << ":\n";
            printed = true;
            std::vector<Entry> entries;
            try {
                auto names = directoryEntries(path);
                std::sort(names.begin(), names.end(), collate);
                for (const auto &name : names) {
                    if (!all && name[0] == '.')
                        continue;
                    Entry entry{name, path + "/" + name, {}};
                    if (lstat(entry.path.c_str(), &entry.info) < 0) {
                        reportError("ls: " + entry.path);
                        status = 1;
                        continue;
                    }
                    entries.push_back(std::move(entry));
                }
                printEntries(entries, longForm, true);
            } catch (const std::exception &error) {
                std::cerr << "ls: " << error.what() << '\n';
                status = 1;
            }
        }
        return status;
    }

    int processInfo(const std::vector<std::string> &args, const std::string &home, pid_t shellPid) {
        pid_t pid = shellPid;
        if (args.size() > 2) {
            std::cerr << "pinfo: Invalid arguments\n";
            return 1;
        }
        if (args.size() == 2) {
            try {
                if (args[1].empty() || args[1].find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("pid");
                const long number = std::stol(args[1]);
                if (number <= 0 || number > INT_MAX)
                    throw std::out_of_range("pid");
                pid = static_cast<pid_t>(number);
            } catch (const std::exception &) {
                std::cerr << "pinfo: invalid PID\n";
                return 1;
            }
        }
        const std::string prefix = "/proc/" + std::to_string(pid);
        std::ifstream input(prefix + "/stat");
        if (!input) {
            std::cerr << "pinfo: cannot read process " << pid << '\n';
            return 1;
        }
        std::string line;
        std::getline(input, line);
        // comm is parenthesized and may itself contain spaces and parentheses.
        const auto end = line.rfind(')');
        if (end == std::string::npos) {
            std::cerr << "pinfo: malformed /proc stat\n";
            return 1;
        }
        std::istringstream fields(line.substr(end + 1));
        char state = '?';
        long parent = 0, pgid = 0, session = 0, tty = 0, foreground = 0;
        fields >> state >> parent >> pgid >> session >> tty >> foreground;
        std::string ignored;
        for (int field = 9; field <= 22; ++field)
            fields >> ignored;
        unsigned long long memory = 0;
        if (!(fields >> memory)) {
            std::cerr << "pinfo: malformed /proc stat\n";
            return 1;
        }
        const std::string executable = readLink(prefix + "/exe");
        std::cout << "pid -- " << pid << '\n'
                  << "Process Status -- " << state << (tty != 0 && pgid == foreground ? "+" : "")
                  << '\n'
                  << "memory -- " << memory << " {Virtual Memory}\n"
                  << "Executable Path -- "
                  << (executable.empty() ? "[unavailable]" : abbreviate(executable, home)) << '\n';
        return 0;
    }

    int searchFiles(const std::vector<std::string> &args) {
        if (args.size() != 2) {
            std::cerr << "search: expected one file or directory name\n";
            return 1;
        }
        std::string target = args[1];
        if (target.compare(0, 2, "./") == 0)
            target.erase(0, 2);
        std::vector<std::string> pending{"."};
        int status = 0;
        while (!pending.empty()) {
            const std::string path = pending.back();
            pending.pop_back();
            try {
                for (const auto &name : directoryEntries(path)) {
                    if (name == "." || name == "..")
                        continue;
                    const std::string child = path + "/" + name;
                    if (name == target || child.substr(2) == target) {
                        std::cout << "True\n";
                        return 0;
                    }
                    struct stat info{};
                    if (lstat(child.c_str(), &info) < 0) {
                        reportError("search: " + child);
                        status = 1;
                        continue;
                    }
                    // Do not follow directory symlinks: cycles cannot recurse forever.
                    if (S_ISDIR(info.st_mode))
                        pending.push_back(child);
                }
            } catch (const std::exception &error) {
                std::cerr << "search: " << error.what() << '\n';
                status = 1;
            }
        }
        std::cout << "False\n";
        return status;
    }
} // namespace posix_shell
