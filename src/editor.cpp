#include "editor.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace posix_shell {
    namespace {
        bool separator(char c) {
            return std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '&' ||
                   c == '|' || c == '<' || c == '>';
        }

        std::size_t wordStart(const std::string &line, std::size_t cursor) {
            while (cursor > 0 && !separator(line[cursor - 1]))
                --cursor;
            return cursor;
        }

        bool commandPosition(const std::string &line, std::size_t start) {
            bool command = true;
            for (std::size_t i = 0; i < start; ++i) {
                if (line[i] == ';' || line[i] == '&' || line[i] == '|')
                    command = true;
                else if (!std::isspace(static_cast<unsigned char>(line[i])))
                    command = false;
            }
            return command;
        }

        class TerminalMode {
            public:
            TerminalMode() {
                if (tcgetattr(STDIN_FILENO, &saved_) < 0)
                    throw std::runtime_error("tcgetattr failed");
                auto raw = saved_;
                raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN | ISIG));
                raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
                raw.c_cc[VMIN] = 1;
                raw.c_cc[VTIME] = 0;
                if (tcsetattr(STDIN_FILENO, TCSADRAIN, &raw) < 0)
                    throw std::runtime_error("tcsetattr failed");
            }
            ~TerminalMode() {
                tcsetattr(STDIN_FILENO, TCSADRAIN, &saved_);
            }

            private:
            struct termios saved_{};
        };

        std::string escapeSequence() {
            std::string result;
            // A lone Escape key must not block the editor. CSI and SS3 arrow keys
            // consist of a short sequence and may arrive in separate reads.
            for (int i = 0; i < 12; ++i) {
                struct pollfd input{STDIN_FILENO, POLLIN, 0};
                if (poll(&input, 1, 40) <= 0)
                    break;
                char c;
                if (read(STDIN_FILENO, &c, 1) != 1)
                    break;
                result += c;
                if (result.size() > 1 && ((c >= 'A' && c <= 'Z') || c == '~'))
                    break;
                if (result.size() == 1 && c != '[' && c != 'O')
                    break;
            }
            return result;
        }
    } // namespace

    std::vector<std::string> completions(const std::string &line, std::size_t cursor,
                                         const std::string &home) {
        cursor = std::min(cursor, line.size());
        const auto start = wordStart(line, cursor);
        const std::string prefix = line.substr(start, cursor - start);
        std::set<std::string> matches;
        const auto slash = prefix.rfind('/');
        const std::string directory =
            slash == std::string::npos ? "." : expandHome(prefix.substr(0, slash + 1), home);
        const std::string base = slash == std::string::npos ? prefix : prefix.substr(slash + 1);
        const std::string leading = slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
        try {
            for (const auto &name : directoryEntries(directory)) {
                if (name == "." || name == ".." ||
                    (name[0] == '.' && (base.empty() || base[0] != '.')))
                    continue;
                if (name.compare(0, base.size(), base) != 0)
                    continue;
                struct stat info{};
                const bool dir =
                    stat((directory + "/" + name).c_str(), &info) == 0 && S_ISDIR(info.st_mode);
                matches.insert(leading + name + (dir ? "/" : ""));
            }
        } catch (const std::exception &) { /* A missing prefix directory has no matches. */
        }
        if (commandPosition(line, start) && slash == std::string::npos) {
            for (const char *name : {"cd", "echo", "pwd", "ls", "pinfo", "search", "history",
                                     "exit", "jobs", "fg", "bg"}) {
                if (std::string(name).compare(0, prefix.size(), prefix) == 0)
                    matches.insert(name);
            }
            const char *environment = std::getenv("PATH");
            if (environment) {
                const std::string path = environment;
                std::size_t offset = 0;
                do {
                    const auto end = path.find(':', offset);
                    std::string part =
                        path.substr(offset, end == std::string::npos ? end : end - offset);
                    if (part.empty())
                        part = ".";
                    try {
                        for (const auto &name : directoryEntries(part)) {
                            if (name.compare(0, prefix.size(), prefix) != 0)
                                continue;
                            const std::string full = part + "/" + name;
                            struct stat info{};
                            if (stat(full.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                                access(full.c_str(), X_OK) == 0)
                                matches.insert(name);
                        }
                    } catch (const std::exception &) {
                    }
                    if (end == std::string::npos)
                        break;
                    offset = end + 1;
                } while (offset <= path.size());
            }
        }
        return {matches.begin(), matches.end()};
    }

    std::optional<std::string> readInteractiveLine(const std::string &prompt,
                                                   const std::vector<std::string> &history,
                                                   const std::string &home, int signalFd,
                                                   const std::function<bool()> &onEvent) {
        TerminalMode terminal;
        std::string line, draft;
        std::size_t cursor = 0, historyIndex = history.size(), view = 0;
        auto redraw = [&]() {
            struct winsize size{};
            const std::size_t columns =
                ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col >= 10 ? size.ws_col
                                                                                  : 80;
            std::string shownPrompt = prompt;
            if (shownPrompt.size() > columns - 5)
                shownPrompt = "..." + shownPrompt.substr(shownPrompt.size() - columns / 2);
            const std::size_t available = columns - shownPrompt.size() - 1;
            if (cursor < view)
                view = cursor;
            if (cursor >= view + available)
                view = cursor - available + 1;
            std::string display = "\r\033[2K" + shownPrompt + line.substr(view, available);
            display += "\r\033[" + std::to_string(shownPrompt.size() + cursor - view) + "C";
            writeAll(STDOUT_FILENO, display);
        };
        redraw();
        for (;;) {
            struct pollfd fds[]{{STDIN_FILENO, POLLIN, 0}, {signalFd, POLLIN, 0}};
            const int ready = poll(fds, 2, -1);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                return std::nullopt;
            }
            if (fds[1].revents & POLLIN) {
                writeAll(STDOUT_FILENO, "\r\033[2K");
                if (onEvent()) {
                    writeAll(STDOUT_FILENO, "^C\n");
                    line.clear();
                    draft.clear();
                    cursor = 0;
                    view = 0;
                    historyIndex = history.size();
                }
                redraw();
            }
            if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char c;
            const ssize_t count = read(STDIN_FILENO, &c, 1);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return std::nullopt;
            if (c == '\r' || c == '\n') {
                writeAll(STDOUT_FILENO, "\n");
                return line;
            }
            if (c == 4) {
                if (line.empty()) {
                    writeAll(STDOUT_FILENO, "\n");
                    return std::nullopt;
                }
                if (cursor < line.size())
                    line.erase(cursor, 1);
            } else if (c == 3) {
                writeAll(STDOUT_FILENO, "^C\n");
                return std::string{};
            } else if (c == 26) {
                continue;
            } else if (c == 127 || c == 8) {
                if (cursor)
                    line.erase(--cursor, 1);
            } else if (c == 1)
                cursor = 0;
            else if (c == 5)
                cursor = line.size();
            else if (c == 21) {
                line.erase(0, cursor);
                cursor = 0;
            } else if (c == 11)
                line.erase(cursor);
            else if (c == 12)
                writeAll(STDOUT_FILENO, "\033[2J\033[H");
            else if (c == 27) {
                const auto key = escapeSequence();
                if (key == "[A" || key == "OA") {
                    if (historyIndex > 0) {
                        if (historyIndex == history.size())
                            draft = line;
                        line = history[--historyIndex];
                        cursor = line.size();
                    }
                } else if (key == "[B" || key == "OB") {
                    if (historyIndex < history.size()) {
                        ++historyIndex;
                        line = historyIndex == history.size() ? draft : history[historyIndex];
                        cursor = line.size();
                    }
                } else if (key == "[D" || key == "OD") {
                    if (cursor)
                        --cursor;
                } else if (key == "[C" || key == "OC") {
                    if (cursor < line.size())
                        ++cursor;
                } else if (key == "[H" || key == "OH" || key == "[1~")
                    cursor = 0;
                else if (key == "[F" || key == "OF" || key == "[4~")
                    cursor = line.size();
                else if (key == "[3~" && cursor < line.size())
                    line.erase(cursor, 1);
            } else if (c == '\t') {
                const auto matches = completions(line, cursor, home);
                if (matches.empty()) {
                    writeAll(STDOUT_FILENO, "\a");
                    continue;
                }
                std::string common = matches[0];
                for (const auto &match : matches) {
                    std::size_t length = 0;
                    while (length < common.size() && length < match.size() &&
                           common[length] == match[length])
                        ++length;
                    common.resize(length);
                }
                const auto start = wordStart(line, cursor);
                const auto length = cursor - start;
                if (common.size() > length) {
                    line.replace(start, length, common);
                    cursor = start + common.size();
                } else if (matches.size() > 1) {
                    writeAll(STDOUT_FILENO, "\n");
                    for (const auto &match : matches)
                        writeAll(STDOUT_FILENO, match + "  ");
                    writeAll(STDOUT_FILENO, "\n");
                }
            } else if (static_cast<unsigned char>(c) >= 32) {
                line.insert(cursor++, 1, c);
            }
            redraw();
        }
    }
} // namespace posix_shell
