#ifndef ENV_LOADER_HPP
#define ENV_LOADER_HPP

#include <string>

// Loads KEY=VALUE entries from a .env file into the process
// environment. Existing environment variables are preserved. A value wrapped
// in matching single or double quotes is stored without them. Not
// thread-safe: call it before starting threads that read the environment.
bool loadEnvFile(const std::string& filePath);

// Returns the value of a required environment variable. Throws
// ConfigurationError when it is unset or contains only whitespace; the
// message names the variable but never includes a value.
std::string requireEnvironmentVariable(const std::string& name);

#endif // ENV_LOADER_HPP
