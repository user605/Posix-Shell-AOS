#include "parser.hpp"
#include "util.hpp"

#include <cstring>
#include <stdexcept>

namespace posix_shell {
    namespace {
        bool isOperator(char c) {
            return c == ';' || c == '&' || c == '|' || c == '<' || c == '>';
        }

        std::vector<std::string> tokenize(const std::string &line) {
            std::vector<std::string> tokens;
            std::size_t cursor = 0;
            while (cursor < line.size()) {
                if (isOperator(line[cursor])) {
                    std::string op(1, line[cursor++]);
                    if (op == ">" && cursor < line.size() && line[cursor] == '>') {
                        op += line[cursor++];
                    }
                    tokens.push_back(op);
                } else {
                    const std::size_t start = cursor;
                    while (cursor < line.size() && !isOperator(line[cursor]))
                        ++cursor;
                    // strtok is explicitly required by the assignment. Each span is
                    // tokenized before processing the next one (no nested strtok use).
                    const std::string span = line.substr(start, cursor - start);
                    std::vector<char> buffer(span.begin(), span.end());
                    buffer.push_back('\0');
                    for (char *word = std::strtok(buffer.data(), " \t\r\n"); word;
                         word = std::strtok(nullptr, " \t\r\n")) {
                        tokens.emplace_back(word);
                    }
                }
            }
            return tokens;
        }

        bool operatorToken(const std::string &token) {
            return (token.size() == 1 && isOperator(token[0])) || token == ">>";
        }
    } // namespace

    std::vector<Pipeline> parse(const std::string &line) {
        const auto tokens = tokenize(line);
        std::vector<Pipeline> result;
        Pipeline pipeline;
        Command command;
        std::string description;
        auto finishCommand = [&]() {
            if (command.args.empty())
                throw std::runtime_error("expected a command");
            pipeline.commands.push_back(std::move(command));
            command = Command{};
        };
        auto finishPipeline = [&]() {
            finishCommand();
            pipeline.description = trim(description);
            result.push_back(std::move(pipeline));
            pipeline = Pipeline{};
            description.clear();
        };

        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const auto &token = tokens[i];
            if (token == ";" || token == "&") {
                if (command.args.empty() && command.redirects.empty() &&
                    pipeline.commands.empty()) {
                    if (token == ";")
                        continue;
                    throw std::runtime_error("unexpected '&'");
                }
                pipeline.background = token == "&";
                finishPipeline();
            } else if (token == "|") {
                finishCommand();
                description += " | ";
            } else if (token == "<" || token == ">" || token == ">>") {
                if (i + 1 == tokens.size() || operatorToken(tokens[i + 1])) {
                    throw std::runtime_error("expected a filename after '" + token + "'");
                }
                const auto kind = token == "<"   ? RedirectKind::Input
                                  : token == ">" ? RedirectKind::Output
                                                 : RedirectKind::Append;
                command.redirects.push_back({kind, tokens[++i]});
                description += " " + token + " " + tokens[i];
            } else {
                command.args.push_back(token);
                description += " " + token;
            }
        }
        if (!command.args.empty() || !command.redirects.empty() || !pipeline.commands.empty()) {
            finishPipeline();
        }
        return result;
    }
} // namespace posix_shell
