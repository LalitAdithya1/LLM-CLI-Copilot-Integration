
#ifndef CLI_CONTROLLER_HPP
#define CLI_CONTROLLER_HPP

#include <functional>
#include <iosfwd>
#include <string>

class CLIController {
public:
    using LLMCall = std::function<bool(
        const std::string& prompt,
        std::string& response,
        bool requireExplicitCompletion
    )>;

    CLIController();
    CLIController(
        LLMCall llmCall,
        std::istream& input,
        std::ostream& output,
        std::ostream& error
    );

    int run(int argc, char* argv[]);

private:
    LLMCall llmCall_;
    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
};

#endif
