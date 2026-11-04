#pragma once
// A flat key=value file. Used for t8cine.ini and the launcher's settings.
#include <filesystem>
#include <map>
#include <string>

class Ini {
public:
    bool Load(const std::filesystem::path& path);
    bool Save(const std::filesystem::path& path) const;

    std::string Get(const std::string& key, const std::string& fallback = {}) const;
    double GetNumber(const std::string& key, double fallback) const;
    bool GetBool(const std::string& key, bool fallback) const;
    void Set(const std::string& key, const std::string& value) { m_values[key] = value; }
    void SetNumber(const std::string& key, double value);
    void SetBool(const std::string& key, bool value) { m_values[key] = value ? "1" : "0"; }
    const std::map<std::string, std::string>& Values() const { return m_values; }

private:
    std::map<std::string, std::string> m_values;
};
