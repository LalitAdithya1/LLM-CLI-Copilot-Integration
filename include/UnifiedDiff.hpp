#ifndef UNIFIED_DIFF_HPP
#define UNIFIED_DIFF_HPP

#include <string>

std::string makeUnifiedDiff(
    const std::string& original,
    const std::string& proposed
);

#endif // UNIFIED_DIFF_HPP
