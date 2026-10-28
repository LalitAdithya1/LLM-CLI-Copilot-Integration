
#include "EnvLoader.hpp"

#include "CodeAgentError.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos) {
        return "";
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

} // namespace

bool loadEnvFile(const std::string& filePath) {
    std::ifstream file(filePath);

    if (!file.is_open()) {
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        line = trim(line);

        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto separator = line.find('=');

        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = trim(line.substr(0, separator));
        std::string value = trim(line.substr(separator + 1));
        // KEY="value" and KEY='value' store value without the quotes.
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }

        if (key.empty()) {
            continue;
        }

        // Do not overwrite an environment variable already supplied
        // by the shell or deployment environment.
        if (std::getenv(key.c_str()) != nullptr) {
            continue;
        }

        if (setenv(key.c_str(), value.c_str(), 0) != 0) {
            return false;
        }
    }

    return true;
}

std::string requireEnvironmentVariable(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || trim(value).empty()) {
        throw ConfigurationError::missingSetting(name);
    }
    return value;
}
