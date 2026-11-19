// Tekken Studio for Tekken Tag Tournament 2 (the BlueSpark recompilation). This file is linked into a separate copy of the game,
// ttt2-studio.exe, next to BlueSpark's own ttt2.exe, which stays as it was. It starts the studio once the game's window and its
// ImGui drawer exist.
#include "studio.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

namespace studio {

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;

std::filesystem::path ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}
} // namespace

const std::filesystem::path& DataDir()
{
    static const std::filesystem::path dir = ExeDir() / "studio";
    return dir;
}

void Log(const char* format, ...)
{
    std::lock_guard lock(g_logMutex);
    if (!g_log) {
        std::error_code ignored;
        std::filesystem::create_directories(DataDir(), ignored);
        g_log = _wfsopen((DataDir() / "studio.log").c_str(), L"w", _SH_DENYNO);
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

uint8_t* GuestBase()
{
    rex::Runtime* runtime = rex::Runtime::instance();
    return runtime ? runtime->virtual_membase() : nullptr;
}

namespace {

// Waits for the game's window, then puts the studio's dialog on its ImGui drawer, from the UI thread as the drawer wants.
void Boot()
{
    Log("Tekken Studio for TTT2 starting");
    // The launcher says how to start: "studio" opens the editor, anything else leaves the simple bar.
    {
        char mode[32] = "";
        if (FILE* file = _wfopen((DataDir() / "start-mode.txt").c_str(), L"r")) {
            fgets(mode, sizeof(mode), file);
            fclose(file);
        }
        Editor() = strncmp(mode, "studio", 6) == 0 ? EditorMode::Studio : EditorMode::Simple;
    }
    for (int tries = 0; tries < 6000; ++tries) {
        rex::Runtime* runtime = rex::Runtime::instance();
        if (runtime && runtime->imgui_drawer() && runtime->display_window()) {
            rex::ui::ImGuiDrawer* drawer = runtime->imgui_drawer();
            runtime->display_window()->app_context().CallInUIThread([drawer] {
                CreateEditor(drawer);
                Log("Editor ready. Insert opens it.");
            });
            StartCommands();
            StartReplayKeeper();
            return;
        }
        Sleep(50);
    }
    Log("Gave up: the game's window never came");
}

struct Starter {
    Starter() { std::thread(Boot).detach(); }
} g_starter;

} // namespace

} // namespace studio
