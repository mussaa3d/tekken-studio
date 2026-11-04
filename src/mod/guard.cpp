#include "mod/guard.h"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
// clang-format on

#include <MinHook.h>

#include <atomic>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/guardlogic.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/ue.h"

namespace guard {

namespace {

// A connection this long-lived is a real session rather than a check.
constexpr uint64_t ConnectionLimitMs = 60000;

std::atomic<bool> g_tripped { false };
std::atomic<int> g_connections { 0 };
std::atomic<int> g_steamState { -1 };
char g_lastAddress[64] = "";

bool IsLocalV4(DWORD address)
{
    const auto* b = reinterpret_cast<const unsigned char*>(&address);
    return b[0] == 127 || b[0] == 0 || (b[0] == 169 && b[1] == 254);
}

std::set<std::string> OpenConnections()
{
    const DWORD self = GetCurrentProcessId();
    std::set<std::string> open;
    char text[64];

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
            in_addr address {};
            address.S_un.S_addr = row.dwRemoteAddr;
            inet_ntop(AF_INET, &address, text, sizeof(text));
            open.insert(std::string(text) + ":" + std::to_string(ntohs(static_cast<u_short>(row.dwRemotePort))));
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
            inet_ntop(AF_INET6, &address, text, sizeof(text));
            open.insert(std::string(text) + ":" + std::to_string(ntohs(static_cast<u_short>(row.dwRemotePort))));
        }
    }
    return open;
}

// 1 when Steam is signed in, 0 when it's in Offline Mode, -1 before Steam has started up in the game. The game needs Steam
// signed in to reach its online services, which makes this the direct check.
int SteamSignedIn()
{
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam)
        return -1;
    using UserFn = void* (*)();
    using LoggedOnFn = bool (*)(void*);
    const auto getUser = reinterpret_cast<UserFn>(GetProcAddress(steam, "SteamAPI_SteamUser_v021"));
    const auto loggedOn = reinterpret_cast<LoggedOnFn>(GetProcAddress(steam, "SteamAPI_ISteamUser_BLoggedOn"));
    if (!getUser || !loggedOn)
        return -1;
    void* user = getUser();
    return user ? (loggedOn(user) ? 1 : 0) : -1;
}

// ----- The network block

using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using WsaConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
using WsaIoctlFn = int(WSAAPI*)(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

ConnectFn oConnect = nullptr;
WsaConnectFn oWsaConnect = nullptr;
WsaIoctlFn oWsaIoctl = nullptr;
LPFN_CONNECTEX oConnectEx = nullptr;
std::atomic<int> g_blocked { 0 };

bool IsOffThisPc(const sockaddr* address)
{
    if (!address)
        return false;
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        return !IsLocalV4(v4->sin_addr.S_un.S_addr);
    }
    if (address->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        return !IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr) && !IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr);
    }
    return false;
}

void NoteBlocked(const sockaddr* address)
{
    char text[64] = "?";
    if (address->sa_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(address)->sin_addr, text, sizeof(text));
    else
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr, text, sizeof(text));
    if (g_blocked++ < 50)
        mod::Log("Network block: stopped a connection to %s", text);
}

int WSAAPI DetourConnect(SOCKET s, const sockaddr* address, int length)
{
    if (IsOffThisPc(address)) {
        NoteBlocked(address);
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return oConnect(s, address, length);
}

int WSAAPI DetourWsaConnect(SOCKET s, const sockaddr* address, int length, LPWSABUF caller, LPWSABUF callee, LPQOS sqos, LPQOS gqos)
{
    if (IsOffThisPc(address)) {
        NoteBlocked(address);
        WSASetLastError(WSAENETUNREACH);
        return SOCKET_ERROR;
    }
    return oWsaConnect(s, address, length, caller, callee, sqos, gqos);
}

BOOL PASCAL DetourConnectEx(SOCKET s, const sockaddr* address, int length, PVOID buffer, DWORD sendLength, LPDWORD sent, LPOVERLAPPED overlapped)
{
    if (IsOffThisPc(address)) {
        NoteBlocked(address);
        WSASetLastError(WSAENETUNREACH);
        return FALSE;
    }
    return oConnectEx(s, address, length, buffer, sendLength, sent, overlapped);
}

// WinHTTP and others get ConnectEx by asking for its address here, so that's where to step in.
int WSAAPI DetourWsaIoctl(SOCKET s, DWORD code, LPVOID in, DWORD inSize, LPVOID out, DWORD outSize, LPDWORD returned, LPWSAOVERLAPPED overlapped,
                          LPWSAOVERLAPPED_COMPLETION_ROUTINE routine)
{
    const int result = oWsaIoctl(s, code, in, inSize, out, outSize, returned, overlapped, routine);
    static const GUID connectEx = WSAID_CONNECTEX;
    if (result == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && in && inSize >= sizeof(GUID) && memcmp(in, &connectEx, sizeof(GUID)) == 0 && out) {
        auto* function = static_cast<LPFN_CONNECTEX*>(out);
        if (*function != DetourConnectEx) {
            oConnectEx = *function;
            *function = DetourConnectEx;
        }
    }
    return result;
}

void TurnOff(const std::string& reason)
{
    mod::Log("Online guard: %s. Turning the mod off.", reason.c_str());
    game::RunOnGameThread([] { ue::SetGlobalTimeDilation(1.0f); });
    Sleep(250); // let the game thread put time dilation back
    g_tripped = true;
    Sleep(250); // every detour now passes straight through
    game::Shutdown();
    MessageBoxW(nullptr,
                L"The game went online, so T8Cine turned itself off.\n\n"
                L"Close the game. Use Steam's Offline Mode before starting a cinematic session.",
                L"T8Cine", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

DWORD WINAPI Watch(void*)
{
    Ini settings;
    settings.Load(mod::DataDir() / "t8cine.ini");
    const bool enforce = settings.GetBool("online_guard", true);

    ConnectionAges ages;
    for (;;) {
        const int steam = SteamSignedIn();
        if (steam != g_steamState.exchange(steam))
            mod::Log("Online guard: Steam is %s", steam == 1 ? "signed in" : (steam == 0 ? "offline" : "not started yet"));

        const std::set<std::string> open = OpenConnections();
        g_connections = static_cast<int>(open.size());
        for (const std::string& connection : ages.Update(open, GetTickCount64())) {
            strncpy_s(g_lastAddress, connection.c_str(), _TRUNCATE);
            mod::Log("Online guard: the game connected to %s", connection.c_str());
        }

        if (enforce && steam == 1) {
            TurnOff("Steam is signed in");
            return 0;
        }
        const std::string lasting = ages.OpenLongerThan(ConnectionLimitMs);
        if (enforce && !lasting.empty()) {
            TurnOff("a connection to " + lasting + " stayed open");
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

void BlockInternet()
{
    const MH_STATUS started = MH_Initialize();
    if (started != MH_OK && started != MH_ERROR_ALREADY_INITIALIZED)
        return;
    HMODULE winsock = LoadLibraryW(L"ws2_32.dll");
    const auto hook = [&](const char* name, void* detour, void** original) {
        void* target = winsock ? reinterpret_cast<void*>(GetProcAddress(winsock, name)) : nullptr;
        if (!target)
            return false;
        return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    };
    const bool ok = hook("connect", reinterpret_cast<void*>(&DetourConnect), reinterpret_cast<void**>(&oConnect))
        && hook("WSAConnect", reinterpret_cast<void*>(&DetourWsaConnect), reinterpret_cast<void**>(&oWsaConnect))
        && hook("WSAIoctl", reinterpret_cast<void*>(&DetourWsaIoctl), reinterpret_cast<void**>(&oWsaIoctl));
    mod::Log(ok ? "Network block is on" : "Network block couldn't start");
}

int BlockedAttempts()
{
    return g_blocked.load(std::memory_order_relaxed);
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

int SteamState()
{
    return g_steamState.load(std::memory_order_relaxed);
}

} // namespace guard
