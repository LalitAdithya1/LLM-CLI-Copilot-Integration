
#ifndef ENV_LOADER_HPP
#define ENV_LOADER_HPP

#include <string>

// Loads KEY=VALUE entries from a .env file into the process
// environment. Existing environment variables are preserved.
bool loadEnvFile(const std::string& filePath);

#endif // ENV_LOADER_HPP
