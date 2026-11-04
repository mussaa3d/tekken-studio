#include "common/config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {
std::string Trim(const std::string& text)
{
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
} // namespace

bool Ini::Load(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file)
        return false;
    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const size_t equals = line.find('=');
        if (equals != std::string::npos)
            m_values[Trim(line.substr(0, equals))] = Trim(line.substr(equals + 1));
    }
    return true;
}

bool Ini::Save(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return false;
    for (const auto& [key, value] : m_values)
        file << key << " = " << value << "\n";
    return true;
}

std::string Ini::Get(const std::string& key, const std::string& fallback) const
{
    const auto it = m_values.find(key);
    return it == m_values.end() ? fallback : it->second;
}

double Ini::GetNumber(const std::string& key, double fallback) const
{
    const auto it = m_values.find(key);
    if (it == m_values.end() || it->second.empty())
        return fallback;
    char* end = nullptr;
    const double value = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : value;
}

bool Ini::GetBool(const std::string& key, bool fallback) const
{
    const auto it = m_values.find(key);
    return it == m_values.end() ? fallback : (it->second == "1" || it->second == "true");
}

void Ini::SetNumber(const std::string& key, double value)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.6g", value);
    m_values[key] = buffer;
}
