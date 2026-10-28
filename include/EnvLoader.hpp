#ifndef ENV_LOADER_HPP
#define ENV_LOADER_HPP

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

// Loads KEY=VALUE entries from a .env file into the process
// environment. Existing environment variables are preserved. A value wrapped
// in matching single or double quotes is stored without them. Not
// thread-safe: call it before starting threads that read the environment.
bool loadEnvFile(const std::string& filePath);

// The .env files CodeAgent reads, highest priority first:
//   1. the file named by CODEAGENT_ENV_FILE, when it is set;
//   2. .env in the current working directory (per-project settings);
//   3. $XDG_CONFIG_HOME/codeagent/.env, or ~/.config/codeagent/.env when
//      XDG_CONFIG_HOME is unset (per-user settings, used from any folder).
// Variables already in the process environment take precedence over all of
// them, because loading never overwrites a variable.
std::vector<std::filesystem::path> configurationFileCandidates();

// Loads each existing candidate in order and returns the files that were
// read. A missing CODEAGENT_ENV_FILE is reported on warnings, because it was
// configured explicitly; other missing files are normal.
std::vector<std::filesystem::path> loadConfigurationFiles(
    const std::vector<std::filesystem::path>& candidates,
    std::ostream& warnings
);

// Loads configurationFileCandidates() once per process. Call before starting
// threads that read the environment, because setting variables is not
// thread-safe.
void loadConfigurationFilesOnce();

// Returns the value of a required environment variable. Throws
// ConfigurationError when it is unset or contains only whitespace; the
// message names the variable but never includes a value, and the diagnostic
// lists the .env locations that were checked.
std::string requireEnvironmentVariable(const std::string& name);

#endif // ENV_LOADER_HPP
