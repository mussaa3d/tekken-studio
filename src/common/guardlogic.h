#pragma once
// How long each connection has been open. Steam's client runs inside the game's process and makes short connectivity checks
// even in Offline Mode, so one brief connection means nothing. One that stays open does.
#include <cstdint>
#include <map>
#include <set>
#include <string>

class ConnectionAges {
public:
    // Call with the connections open right now. Returns the ones never seen before, for logging.
    std::set<std::string> Update(const std::set<std::string>& open, uint64_t nowMs)
    {
        std::set<std::string> added;
        for (auto it = m_firstSeen.begin(); it != m_firstSeen.end();)
            it = open.count(it->first) ? std::next(it) : m_firstSeen.erase(it);
        for (const std::string& connection : open)
            if (m_firstSeen.emplace(connection, nowMs).second)
                added.insert(connection);
        m_now = nowMs;
        return added;
    }

    // The connection open the longest, empty if none has reached the limit.
    std::string OpenLongerThan(uint64_t limitMs) const
    {
        for (const auto& [connection, firstSeen] : m_firstSeen)
            if (m_now - firstSeen >= limitMs)
                return connection;
        return {};
    }

private:
    std::map<std::string, uint64_t> m_firstSeen;
    uint64_t m_now = 0;
};
