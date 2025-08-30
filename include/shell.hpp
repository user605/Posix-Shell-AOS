#pragma once

#include "history.hpp"
#include "parser.hpp"
#include <string>
#include <sys/types.h>
#include <termios.h>

namespace posix_shell {
    class Shell {
        public:
        Shell();
        ~Shell();
        int run();

        private:
        struct Process {
            pid_t pid;
            int status = 0;
            bool done = false;
            bool stopped = false;
        };
        struct Job {
            int id;
            pid_t group;
            std::vector<Process> processes;
            std::string description;
            bool background;
            bool reportedStopped = false;
            struct termios modes{};
            bool hasModes = false;
        };
        int execute(const Pipeline &pipeline);
        int builtin(const Command &command, bool child = false);
        bool isBuiltin(const std::string &name) const;
        std::string prompt() const;
        void initializeSignals();
        void reapChildren(bool notify = true);
        void updateProcess(pid_t pid, int status);
        int waitForeground(int jobId, bool resume = false);
        int jobBuiltin(const Command &command, bool child);
        void shutdownJobs();
        static bool done(const Job &job);
        static bool stopped(const Job &job);
        void processEvents();

        std::string home_;
        std::string previous_;
        History history_;
        pid_t shellPid_ = 0;
        pid_t shellGroup_ = 0;
        bool interactive_ = false;
        struct termios originalModes_{};
        int signalPipe_[2]{-1, -1};
        std::vector<Job> jobs_;
        int nextJob_ = 1;
        bool exiting_ = false;
        int status_ = 0;
    };
} // namespace posix_shell
