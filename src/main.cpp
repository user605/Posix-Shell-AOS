#include "shell.hpp"
#include <clocale>
#include <exception>
#include <iostream>

int main() {
    std::setlocale(LC_ALL, "");
    try {
        posix_shell::Shell shell;
        return shell.run();
    } catch (const std::exception &error) {
        std::cerr << "shell: " << error.what() << '\n';
        return 1;
    }
}
