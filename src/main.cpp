#include "shell.hpp"
#include <exception>
#include <iostream>

int main() {
    try {
        posix_shell::Shell shell;
        return shell.run();
    } catch (const std::exception &error) {
        std::cerr << "shell: " << error.what() << '\n';
        return 1;
    }
}
