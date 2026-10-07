## The modular work breakdown


## Module 1 — Project Foundation and CLI Controller
First implementation task
Files: main.cpp, CLIController.hpp, CLIController.cpp, CMakeLists.txt
Responsibilities:
	• Initialize the application.
	• Parse commands and command-line arguments.
	• Recognize generate, refactor, and help.
	• Validate arguments and display usage instructions.
	• Coordinate modules without containing their internal implementation.
Deliverable: a working CLI that recognizes commands, even when the AI features are not yet implemented.


## Module 2 — File Manager
Files: FileManager.hpp, FileManager.cpp
Responsibilities:
	• Read individual source files.
	• Validate paths and supported file types.
	• Handle missing files, permissions, and read/write failures.
	• Save generated code to a specified output path.
	• Support safe file-writing operations.
Tools: std::filesystem, std::ifstream, std::ofstream.
Deliverable: a tested module for local file operations that works independently of the LLM.

## Module 3 — Prompt Builder
Files: PromptBuilder.hpp, PromptBuilder.cpp
Responsibilities:
	• Construct prompts for code generation and refactoring.
	• Combine user instructions with language and source-code context.
	• Keep generation and refactoring instructions separate.
	• Handle empty instructions and invalid inputs.
Deliverable: deterministic prompt construction that can be unit-tested without making API calls.

## Module 4 — LLM Client
Files: LLMClient.hpp, LLMClient.cpp
Responsibilities:
	• Read API configuration and credentials.
	• Construct authenticated HTTP requests.
	• Send requests using libcurl.
	• Handle HTTP errors, network failures, timeouts, and rate limits.
	• Return responses without printing secrets or modifying files.
Tools: libcurl, nlohmann/json.
Deliverable: a working HTTP client with mocked tests and a real API integration once the provider is selected.

## Module 5 — Response Parser
Files: ResponseParser.hpp, ResponseParser.cpp
Responsibilities:
	• Parse the provider's JSON response.
	• Extract generated code and explanations.
	• Validate required fields.
	• Handle malformed JSON and unexpected response structures.
	• Convert provider-specific responses into a common application result.
Deliverable: structured, validated AI results rather than raw API responses.

## Module 6 — Code Generator
Files: CodeGenerator.hpp, CodeGenerator.cpp
Responsibilities:
	• Receive a generation request.
	• Use the Prompt Builder to create a prompt.
	• Call the LLM Client.
	• Process the response through the Response Parser.
	• Display generated code and optionally save it through the File Manager.
Deliverable: the first complete AI feature.

## Module 7 — Code Refactorer
Files: CodeRefactorer.hpp, CodeRefactorer.cpp
Responsibilities:
	• Accept a source file and refactoring instructions.
	• Read the original code.
	• Request an improved version from the LLM.
	• Validate the returned result.
	• Pass the original and proposed versions to the Diff Manager.
Deliverable: a refactoring proposal that can be reviewed without changing the original file.

##  Module 8 — Diff and Approval Manager
Files: DiffManager.hpp, DiffManager.cpp
Responsibilities:
	• Compare original and proposed source code.
	• Display added, removed, and changed lines.
	• Ask the user to approve or reject changes.
	• Save only approved changes.
	• Handle declined changes and write failures safely.
Deliverable: a review-and-approval workflow that protects the original source by default.

## Module 9 — Integration and Quality Assurance
This is a shared responsibility rather than a single source file.
Responsibilities:
	• Connect all modules through the agreed interfaces.
	• Test complete generation and refactoring workflows.
	• Verify that rejected changes never overwrite the original.
	• Test API failures, invalid commands, and filesystem errors.
	• Verify builds on Windows, Linux, and macOS.
	• Update the README with verified setup and usage instructions.
Tools: GoogleTest, CTest, GitHub Actions, and Git.
