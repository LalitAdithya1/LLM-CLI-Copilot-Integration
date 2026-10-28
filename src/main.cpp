#include "CLIController.hpp"

#include <exception>
#include <iostream>

int main(int argc, char* argv[]) {
    // CLIController::run reports command failures itself. This catches what
    // can escape before it runs, such as an allocation failure while the
    // controller is constructed, so the program never ends in
    // std::terminate.
    try {
        CLIController controller;
        return controller.run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: CodeAgent could not start (" << error.what() << ").\n";
    } catch (...) {
        std::cerr << "Fatal error: CodeAgent could not start.\n";
    }
    return 1;
}
