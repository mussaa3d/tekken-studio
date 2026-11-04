// Research tool for the replay sharing feature. Logs how the game opens, reads and decrypts its save files,
// with the code address of each caller, so the point where a replay becomes plain data can be found. It only
// logs: every call goes through unchanged. Turn it off with trace_saves = 0 in t8cine.ini.
#include "mod/savetrace.h"

#include <windows.h>

#include <bcrypt.h>
#include <intrin.h>

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>

#include "mod/log.h"
#include "mod/scan.h"

namespace savetrace {

namespace {

using CreateFileFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
using DecryptFn = NTSTATUS(WINAPI*)(BCRYPT_KEY_HANDLE, PUCHAR, ULONG, VOID*, PUCHAR, ULONG, PUCHAR, ULONG, ULONG*, ULONG);

CreateFileFn oCreateFile = nullptr;
ReadFileFn oReadFile = nullptr;
WriteFileFn oWriteFile = nullptr;
CloseHandleFn oCloseHandle = nullptr;
DecryptFn oDecrypt = nullptr;

std::mutex g_mutex;
std::set<HANDLE> g_saveHandles;
std::atomic<int> g_lines { 0 };
constexpr int MaxLines = 2000;
uintptr_t g_base = 0;

// Never log while holding g_mutex: logging itself calls WriteFile, which comes back through these hooks.
bool IsSaveHandle(HANDLE handle)
{
    std::lock_guard lock(g_mutex);
    return g_saveHandles.count(handle) != 0;
}

unsigned long long CallerRva(void* address)
{
    return reinterpret_cast<uintptr_t>(address) - g_base;
}

std::string Hex(const void* data, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        text.push_back(digits[bytes[i] >> 4]);
        text.push_back(digits[bytes[i] & 15]);
    }
    return text;
}

HANDLE WINAPI DetourCreateFile(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE file)
{
    const HANDLE handle = oCreateFile(path, access, share, security, disposition, flags, file);
    if (path && wcsstr(path, L"SaveGames") && handle != INVALID_HANDLE_VALUE) {
        {
            std::lock_guard lock(g_mutex);
            g_saveHandles.insert(handle);
        }
        if (g_lines++ < MaxLines)
            mod::Log("[saves] open %ls  access 0x%lX  from +0x%llX", path, access, CallerRva(_ReturnAddress()));
    }
    return handle;
}

BOOL WINAPI DetourReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read, LPOVERLAPPED overlapped)
{
    const BOOL result = oReadFile(file, buffer, size, read, overlapped);
    if (IsSaveHandle(file) && g_lines++ < MaxLines)
        mod::Log("[saves] read %lu bytes from +0x%llX, starts %s", size, CallerRva(_ReturnAddress()), Hex(buffer, std::min<DWORD>(size, 16)).c_str());
    return result;
}

BOOL WINAPI DetourWriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
    if (IsSaveHandle(file) && g_lines++ < MaxLines)
        mod::Log("[saves] write %lu bytes from +0x%llX", size, CallerRva(_ReturnAddress()));
    return oWriteFile(file, buffer, size, written, overlapped);
}

BOOL WINAPI DetourCloseHandle(HANDLE handle)
{
    {
        std::lock_guard lock(g_mutex);
        g_saveHandles.erase(handle);
    }
    return oCloseHandle(handle);
}

// Only large buffers: a replay file is about 1.5 MB, and the engine decrypts plenty of small things.
NTSTATUS WINAPI DetourDecrypt(BCRYPT_KEY_HANDLE key, PUCHAR input, ULONG inputSize, VOID* padding, PUCHAR iv, ULONG ivSize, PUCHAR output,
                              ULONG outputSize, ULONG* result, ULONG flags)
{
    const NTSTATUS status = oDecrypt(key, input, inputSize, padding, iv, ivSize, output, outputSize, result, flags);
    if (inputSize >= 4096 && output && g_lines++ < MaxLines)
        mod::Log("[saves] BCryptDecrypt %lu bytes from +0x%llX, iv size %lu, out starts %s", inputSize, CallerRva(_ReturnAddress()), ivSize,
                 Hex(output, 32).c_str());
    return status;
}

template <typename T>
bool Hook(const wchar_t* module, const char* name, void* detour, T& original)
{
    HMODULE handle = LoadLibraryW(module);
    void* target = handle ? reinterpret_cast<void*>(GetProcAddress(handle, name)) : nullptr;
    if (!target)
        return false;
    MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(&original));
    if (status == MH_OK)
        status = MH_EnableHook(target);
    return status == MH_OK;
}

} // namespace

void Start()
{
    g_base = scan::MainModule().Base;
    const bool files = Hook(L"kernelbase.dll", "CloseHandle", reinterpret_cast<void*>(&DetourCloseHandle), oCloseHandle)
        && Hook(L"kernelbase.dll", "CreateFileW", reinterpret_cast<void*>(&DetourCreateFile), oCreateFile)
        && Hook(L"kernelbase.dll", "ReadFile", reinterpret_cast<void*>(&DetourReadFile), oReadFile)
        && Hook(L"kernelbase.dll", "WriteFile", reinterpret_cast<void*>(&DetourWriteFile), oWriteFile);
    const bool crypto = Hook(L"bcrypt.dll", "BCryptDecrypt", reinterpret_cast<void*>(&DetourDecrypt), oDecrypt);
    mod::Log("Save tracer: file hooks %d, BCrypt hook %d", files, crypto);
}

} // namespace savetrace
