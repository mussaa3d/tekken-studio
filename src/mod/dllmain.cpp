// Entry point. The DLL does nothing unless the launcher has just started a cinematic session: without a
// fresh session file it is a plain dinput8 forwarder with no hooks and no threads.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "common/config.h"
#include "mod/audio.h"
#include "mod/camera.h"
#include "mod/dvars.h"
#include "mod/game.h"
#include "mod/gameui.h"
#include "mod/guard.h"
#include "mod/log.h"
#include "mod/inputblock.h"
#include "mod/overlay.h"
#include "mod/recorder.h"
#include "mod/savetrace.h"
#include "mod/sharing.h"
#include "mod/testcmd.h"

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
        g_log = _wfsopen((g_dataDir / "tekken8mvm.log").c_str(), L"w", _SH_DENYNO);
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

std::filesystem::path ModuleDirectory(HMODULE module)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(module, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::wstring ProcessName()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).filename().wstring();
}

// The launcher writes this right before starting the game and deletes it when the game closes. An old file
// (say, left by a crash) doesn't count.
bool SessionIsFresh(const std::filesystem::path& dataDir)
{
    std::error_code error;
    const auto written = std::filesystem::last_write_time(dataDir / "session.lock", error);
    if (error)
        return false;
    const auto age = std::filesystem::file_time_type::clock::now() - written;
    return age < std::chrono::minutes(10);
}

void LoadSettings()
{
    Ini ini;
    ini.Load(mod::DataDir() / "tekken8mvm.ini");
    recorder::Settings& r = recorder::Config();
    r.Fps = static_cast<int>(ini.GetNumber("record_fps", 60));
    r.CodecIndex = static_cast<int>(ini.GetNumber("record_codec", 0));
    const std::string ffmpeg = ini.Get("ffmpeg_path", "ffmpeg");
    strncpy_s(r.FfmpegPath, ffmpeg.c_str(), _TRUNCATE);
    const std::string output = ini.Get("output_dir");
    if (!output.empty()) {
        r.OutputDir = output;
    } else {
        wchar_t profile[MAX_PATH] = L".";
        GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        r.OutputDir = std::filesystem::path(profile) / "Videos" / "tekken8mvm";
    }
}

DWORD WINAPI GameThread(void*)
{
    mod::Log("tekken8mvm starting");
    guard::BlockInternet();
    LoadSettings();
    dvars::LoadExtra();
    // The executable may still be unpacking when we load, so wait for its code to be findable.
    for (int attempt = 0; attempt < 120; ++attempt) {
        if (game::Initialize()) {
            overlay::Install();
            inputblock::Install();
            gameui::Install();
            guard::Start();
            Ini ini;
            ini.Load(mod::DataDir() / "tekken8mvm.ini");
            if (ini.GetBool("trace_saves", false))
                savetrace::Start();
            if (ini.GetBool("test_commands", false))
                testcmd::Enable();
            sharing::OlderReplaysOn() = ini.GetBool("older_replays", true);
            sharing::AutoLoadOn() = ini.GetBool("auto_load_replays", true);
            sharing::AutoExportOn() = ini.GetBool("auto_export_replays", true);
            sharing::StartOlderReplays();
            mod::Log("tekken8mvm ready. Insert opens the menu.");
            return 0;
        }
        if (attempt == 0)
            mod::Log("Will retry for a while in case the game is still starting");
        Sleep(1000);
    }
    mod::Log("tekken8mvm gave up: this build of the game doesn't match. The game runs normally.");
    return 0;
}

volatile bool g_harnessReady = false;
bool g_harnessAudio = false;
ULONGLONG g_harnessStart = 0;

// Stand-in for the game during testing: overlay and recorder only, driven by environment variables.
DWORD WINAPI HarnessThread(void*)
{
    mod::Log("tekken8mvm harness mode");
    LoadSettings();
    dvars::LoadExtra();

    overlay::HarnessOptions options;
    options.Enabled = true;
    wchar_t number[32] = L"0";
    GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_REC", number, 32);
    options.RecordFrames = _wtoi(number);
    options.Screenshots = GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_SHOT", options.ScreenshotBase, 260) > 0;
    options.UiTest = GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_UITEST", nullptr, 0) > 0;
    options.Preview = GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_PREVIEW", nullptr, 0) > 0;

    // Stream and motion blur setups under test come in through the environment: "looks" is one digit per stream.
    recorder::Settings& r = recorder::Config();
    wchar_t text[32] = L"";
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_LOOKS", text, 32) > 0) {
        for (int i = 0; i < recorder::MaxStreams; ++i) {
            r.Streams[i].Enabled = text[i] != 0;
            if (text[i] == 0)
                break;
            r.Streams[i].Look = static_cast<recorder::Look>(text[i] - L'0');
        }
    }
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_SETTLE", text, 32) > 0)
        r.SettleFrames = _wtoi(text);
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_SAMPLES", text, 32) > 0)
        r.MotionSamples = _wtoi(text);
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_SHUTTER", text, 32) > 0)
        r.ShutterDegrees = static_cast<float>(_wtof(text));
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_SPEED", text, 32) > 0)
        game::Time().Speed = _wtof(text);
    // Picture sizes under test, as 2560x1440: the size to render at and the size of the video.
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_CAPSIZE", text, 32) > 0)
        swscanf_s(text, L"%dx%d", &r.CaptureWidth, &r.CaptureHeight);
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_OUTSIZE", text, 32) > 0)
        swscanf_s(text, L"%dx%d", &r.OutputWidth, &r.OutputHeight);
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_RAMP", nullptr, 0) > 0)
        game::Time().UseSpeedRamp = true;
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_DOLLY", nullptr, 0) > 0)
        camera::Config().Mode = camera::Mode::Dolly;
    g_harnessStart = GetTickCount64();

    // Something to look at on the timeline.
    camera::SetKeys({ { 1.0, { 0, 0, 100 }, { 0, 0, 0 }, 40 }, { 3.0, { 200, 50, 120 }, { -10, 30, 0 }, 45 }, { 4.5, { 300, 200, 100 }, { 5, 80, 10 }, 50 },
                      { 9.0, { 0, 400, 180 }, { 20, 170, -10 }, 30 } });
    game::SetSpeedKeys({ { 2.0, 1.0 }, { 4.0, 0.05 }, { 6.0, 0.05 }, { 8.0, 1.0 } });

    overlay::SetHarness(options);
    overlay::Install();
    g_harnessReady = true;

    // The audio pass, against a sound this process plays.
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS_AUDIO", text, 32) > 0) {
        audio::Start(mod::DataDir() / "harness_audio.wav");
        g_harnessAudio = true;
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) bool tekken8mvmHarnessReady()
{
    return g_harnessReady;
}

// How the launcher tells our dinput8.dll from another mod's.
extern "C" __declspec(dllexport) const char tekken8mvmMarker[] = "TEKKEN8MVM-BUILD-MARKER";

extern "C" __declspec(dllexport) bool tekken8mvmHarnessBusy()
{
    return recorder::Active() || overlay::HarnessBusy();
}

// What the preview's fake scene draws.
extern "C" __declspec(dllexport) void tekken8mvmHarnessState(double* out)
{
    const camera::View view = camera::LastView();
    out[0] = game::Clock();
    out[1] = view.Location.X;
    out[2] = view.Location.Y;
    out[3] = view.Rotation.Yaw;
}

extern "C" __declspec(dllexport) void tekken8mvmHarnessWait()
{
    const camera::View view = camera::LastView();
    mod::Log("Harness end: clock %.3f s after %.3f s of real time, %llu ticks, speed %.3f, camera %.1f %.1f %.1f", game::Clock(),
             (GetTickCount64() - g_harnessStart) / 1000.0, static_cast<unsigned long long>(game::StepCount()), game::Time().Speed, view.Location.X, view.Location.Y, view.Location.Z);
    recorder::WaitUntilFinished();
    if (g_harnessAudio) {
        audio::Stop();
        while (audio::Running())
            Sleep(10);
        mod::Log("Harness audio: %.2f s, heard sound %d, error [%s]", audio::SecondsCaptured(), audio::HeardSound(), audio::LastError().c_str());
    }
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(module);
    LoadRealDinput8();

    const std::filesystem::path dataDir = ModuleDirectory(module) / "tekken8mvm";

    wchar_t harness[8] = L"";
    if (GetEnvironmentVariableW(L"TEKKEN8MVM_HARNESS", harness, 8) > 0) {
        mod::SetDataDir(dataDir);
        CloseHandle(CreateThread(nullptr, 0, HarnessThread, nullptr, 0, nullptr));
        return TRUE;
    }

    if (_wcsicmp(ProcessName().c_str(), L"Polaris-Win64-Shipping.exe") != 0 || !SessionIsFresh(dataDir))
        return TRUE;

    mod::SetDataDir(dataDir);
    CloseHandle(CreateThread(nullptr, 0, GameThread, nullptr, 0, nullptr));
    return TRUE;
}
