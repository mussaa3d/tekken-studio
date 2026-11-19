// Tekken Studio's replay saver: a dinput8.dll of its own that only saves your replays as .t8replay files while you play, online
// too, the way a pak mod sits in the game folder. Every few seconds it reads the game's replay list and writes any replay of yours
// that isn't in my_replays yet. It has no hooks, writes nothing into the game's memory, blocks nothing and connects to nothing.
// With saver_on_key = 1 in the ini it does nothing at all until Ctrl+F10 is pressed with the game in front: only then is the
// replay list read and what's new written, with a high beep for something saved and a low one for nothing new.
// The launcher's Replays tab puts it in and takes it out. For a cinematic session the launcher sets it aside like any other mod's
// dinput8.dll, and puts it back afterwards; the session's own mod saves replays the same way meanwhile.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>

#include "common/config.h"
#include "mod/log.h"
#include "mod/sharing.h"

void LoadRealDinput8();

namespace mod {

namespace {
std::filesystem::path g_dataDir;
std::mutex g_logMutex;
FILE* g_log = nullptr;
} // namespace

const std::filesystem::path& DataDir()
{
    return g_dataDir;
}

void SetDataDir(std::filesystem::path path)
{
    g_dataDir = std::move(path);
}

void Log(const char* format, ...)
{
    std::lock_guard lock(g_logMutex);
    if (!g_log) {
        std::error_code ignored;
        std::filesystem::create_directories(g_dataDir, ignored);
        g_log = _wfsopen((g_dataDir / "replay-saver.log").c_str(), L"w", _SH_DENYNO);
        if (!g_log)
            return;
    }
    SYSTEMTIME time;
    GetLocalTime(&time);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

} // namespace mod

namespace {

std::filesystem::path g_outputDir;

std::filesystem::path OutputDir()
{
    return g_outputDir;
}

std::filesystem::path ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool IsTheGame()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return _wcsicmp(std::filesystem::path(path).filename().c_str(), L"Polaris-Win64-Shipping.exe") == 0;
}

// One look at the replay list. A fault while reading (memory the game let go of between two reads) stops the saver for the rest of
// this run instead of taking the game down: nothing here is worth a crash.
bool Tick()
{
    __try {
        sharing::AutoExportTick();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whether the game's own window is the one in front: the key is the game's only then.
bool GameInFront()
{
    DWORD owner = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &owner);
    return owner == GetCurrentProcessId();
}

// Ctrl+F10: what's new in the replay list is written now. The list has to read the same a few times running before anything is
// written (the game may be in the middle of adding to it), so it's looked at for a few seconds.
bool SaveNow()
{
    const int before = sharing::AutoExported();
    for (int look = 0; look < 8; ++look) {
        if (!Tick())
            return false;
        Sleep(400);
    }
    const int saved = sharing::AutoExported() - before;
    mod::Log("Replay saver: Ctrl+F10, %d saved", saved);
    Beep(saved > 0 ? 1320 : 440, saved > 0 ? 180 : 320);
    return true;
}

DWORD WINAPI SaverThread(void*)
{
    Ini ini;
    ini.Load(mod::DataDir() / "tekken8mvm.ini");
    const std::string output = ini.Get("output_dir");
    if (!output.empty()) {
        g_outputDir = output;
    } else {
        wchar_t profile[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        g_outputDir = std::filesystem::path(profile) / "Videos" / "tekken8mvm";
    }
    sharing::SetOutputDirSource(OutputDir);
    const bool onKey = ini.GetBool("saver_on_key", false);
    mod::Log("Tekken Studio replay saver: saving your replays to %s, %s", (g_outputDir / "my_replays").string().c_str(),
             onKey ? "when Ctrl+F10 is pressed" : "as the game adds them");
    for (bool down = false;;) {
        bool fine = true;
        if (onKey) {
            Sleep(50);
            const bool now = GameInFront() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F10) & 0x8000);
            if (now && !down)
                fine = SaveNow();
            down = now;
        } else {
            Sleep(5000);
            fine = Tick();
        }
        if (!fine) {
            mod::Log("Replay saver: stopped after a fault while reading the replay list. The game carries on; the next start tries again.");
            return 0;
        }
    }
}

} // namespace

// How the launcher tells the replay saver from the studio's mod and from other mods' dinput8.dll.
extern "C" __declspec(dllexport) const char tekkenStudioReplaySaver[] = "TEKKEN-STUDIO-REPLAY-SAVER";

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(module);
    LoadRealDinput8();
    if (!IsTheGame())
        return TRUE;
    mod::SetDataDir(ModuleDirectory(module) / "tekken8mvm");
    CloseHandle(CreateThread(nullptr, 0, SaverThread, nullptr, 0, nullptr));
    return TRUE;
}
