
#include "EnvLoader.hpp"

#include "CodeAgentError.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

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

std::vector<std::filesystem::path> configurationFileCandidates() {
    std::vector<std::filesystem::path> candidates;

    const char* explicitFile = std::getenv("CODEAGENT_ENV_FILE");
    if (explicitFile != nullptr && !trim(explicitFile).empty()) {
        candidates.emplace_back(explicitFile);
    }

    std::error_code error;
    const auto workingDirectory = std::filesystem::current_path(error);
    if (!error) {
        candidates.push_back(workingDirectory / ".env");
    }

    // Only an absolute XDG_CONFIG_HOME is valid, per the XDG specification.
    const char* configHome = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    if (configHome != nullptr && std::filesystem::path(configHome).is_absolute()) {
        candidates.push_back(std::filesystem::path(configHome) / "codeagent" / ".env");
    } else if (home != nullptr && std::filesystem::path(home).is_absolute()) {
        candidates.push_back(std::filesystem::path(home) / ".config" / "codeagent" / ".env");
    }

    return candidates;
}

std::vector<std::filesystem::path> loadConfigurationFiles(
    const std::vector<std::filesystem::path>& candidates,
    std::ostream& warnings
) {
    const char* explicitFile = std::getenv("CODEAGENT_ENV_FILE");
    std::vector<std::filesystem::path> loaded;

    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) &&
            loadEnvFile(candidate.string())) {
            loaded.push_back(candidate);
        } else if (explicitFile != nullptr && candidate == explicitFile) {
            warnings
                << "Warning: CODEAGENT_ENV_FILE is set to " << candidate
                << ", but that file could not be read. Check the path.\n";
        }
    }

    return loaded;
}

void loadConfigurationFilesOnce() {
    // A function-local static is initialized exactly once, even when several
    // threads get here at the same time.
    static const auto loaded =
        loadConfigurationFiles(configurationFileCandidates(), std::cerr);
    (void)loaded;
}

std::string requireEnvironmentVariable(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr || trim(value).empty()) {
        std::string searched;
        for (const auto& candidate : configurationFileCandidates()) {
            searched += (searched.empty() ? "" : ", ") + candidate.string();
        }
        throw ConfigurationError::missingSetting(name, searched);
    }
    return value;
}
