#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "iaisf/app/application.hpp"

int main(const int argc, char* argv[]) {
    try {
        std::vector<std::string> arguments;
        if (argc > 1) {
            arguments.reserve(static_cast<std::size_t>(argc - 1));
        }
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }

        iaisf::Application application{std::cout, std::cerr};
        return application.run(arguments);
    } catch (const std::exception&) {
        std::cerr << "internal error: application terminated unexpectedly\n";
        return 70;
    } catch (...) {
        std::cerr << "internal error: application terminated unexpectedly\n";
        return 70;
    }
}

