#include "mod/guard.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on

#include <atomic>
#include <cstdio>
#include <vector>

#include "common/config.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace guard {

namespace {

std::atomic<bool> g_tripped { false };
std::atomic<int> g_connections { 0 };
char g_lastAddress[64] = "";

bool IsLocalV4(DWORD address)
{
    const auto* b = reinterpret_cast<const unsigned char*>(&address);
    return b[0] == 127 || b[0] == 0 || (b[0] == 169 && b[1] == 254);
}

int CountConnections()
{
    const DWORD self = GetCurrentProcessId();
    int count = 0;

    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    std::vector<char> buffer(size + 1024);
    size = static_cast<DWORD>(buffer.size());
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR) {
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCPROW_OWNER_PID& row = table->table[i];
            if (row.dwOwningPid != self || row.dwState != MIB_TCP_STATE_ESTAB || IsLocalV4(row.dwRemoteAddr))
                continue;
            ++count;
            in_addr address {};
            address.S_un.S_addr = row.dwRemoteAddr;
            inet_ntop(AF_INET, &address, g_lastAddress, sizeof(g_lastAddress));
        }
    }

    size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    buffer.assign(size + 1024, 0);
    size = static_cast<DWORD>(buffer.size());
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR) {
        const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_TCP6ROW_OWNER_PID& row = table->table[i];
            if (row.dwOwningPid != self || row.dwState != MIB_TCP_STATE_ESTAB)
                continue;
            in6_addr address {};
            memcpy(&address, row.ucRemoteAddr, sizeof(address));
            if (IN6_IS_ADDR_LOOPBACK(&address) || IN6_IS_ADDR_LINKLOCAL(&address))
                continue;
            ++count;
            inet_ntop(AF_INET6, &address, g_lastAddress, sizeof(g_lastAddress));
        }
    }
    return count;
}

DWORD WINAPI Watch(void*)
{
    Ini settings;
    settings.Load(mod::DataDir() / "t8cine.ini");
    const bool enforce = settings.GetBool("online_guard", true);

    for (;;) {
        const int count = CountConnections();
        g_connections = count;
        if (count > 0 && enforce) {
            mod::Log("Online guard: the game connected to %s (%d connection(s)). Turning the mod off.", g_lastAddress, count);
            game::RunOnGameThread([] { ue::SetGlobalTimeDilation(1.0f); });
            Sleep(250); // let the game thread put time dilation back
            g_tripped = true;
            Sleep(250); // every detour now passes straight through
            game::Shutdown();
            MessageBoxW(nullptr,
                        L"The game connected to the internet, so T8Cine turned itself off.\n\n"
                        L"Close the game. Use Steam's Offline Mode before starting a cinematic session.",
                        L"T8Cine", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
            return 0;
        }
        Sleep(2000);
    }
}

} // namespace

void Start()
{
    CloseHandle(CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr));
}

bool Tripped()
{
    return g_tripped.load(std::memory_order_relaxed);
}

int LastConnectionCount()
{
    return g_connections.load(std::memory_order_relaxed);
}

const char* LastRemoteAddress()
{
    return g_lastAddress;
}

} // namespace guard
