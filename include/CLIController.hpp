
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
    using GuiCommand = std::function<int(std::ostream&, std::ostream&)>;

    CLIController();
    CLIController(
        LLMCall llmCall,
        std::istream& input,
        std::ostream& output,
        std::ostream& error,
        GuiCommand guiCommand = {}
    );

    // Runs one command. Never throws: failures are reported on the error
    // stream and as exit code 1.
    int run(int argc, char* argv[]);

private:
    // Runs the command; may throw CodeAgentError or other exceptions.
    int dispatch(int argc, char* argv[]);

    LLMCall llmCall_;
    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
    GuiCommand guiCommand_;
};

#endif
