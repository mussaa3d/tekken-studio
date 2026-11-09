#pragma once
// Blending two looks for the look keys on the timeline. A look is the editor's settings written down as text, the way a preset is.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace cine {

using LookValues = std::map<std::string, std::string>;

inline bool LookNumber(const std::string& text, double& value)
{
    if (text.empty())
        return false;
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0';
}

// The look `alpha` of the way from a to b. Numbers that `continuous` accepts move smoothly. Everything else, switches and choices and
// settings only one of the two has, keeps a's value until b is reached.
template <typename Continuous>
LookValues BlendLook(const LookValues& a, const LookValues& b, double alpha, Continuous continuous)
{
    if (alpha <= 0)
        return a;
    if (alpha >= 1)
        return b;
    LookValues out = a;
    for (auto& [key, value] : out) {
        const auto other = b.find(key);
        double from = 0, to = 0;
        if (other == b.end() || other->second == value || !continuous(key) || !LookNumber(value, from) || !LookNumber(other->second, to))
            continue;
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%.6g", from + (to - from) * alpha);
        value = buffer;
    }
    return out;
}

} // namespace cine
