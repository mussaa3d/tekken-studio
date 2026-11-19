// Recording to video, frame for frame. The game is let run one frame at a time (skip.cpp asks RecorderAllowsFrame once a game
// frame), the picture that frame makes is grabbed off the screen a few presents later, and the frames go to ffmpeg down a pipe at 60
// a second. So the video has every game frame once however long the PC takes over each, and at a slow speed each frame is written
// more than once, which is slow motion made of the game's real frames. The studio draws nothing while it records and BlueSpark's
// overlay is off, so the video is the game's picture alone. There is no sound in it.
#include "studio.h"

#include "camera.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace studio {

namespace {

// How many presents after a game frame ran before the picture is grabbed: the frame has to be drawn, presented and put on the screen
// by Windows first.
constexpr int SettlePresents = 6;

enum class Phase { Off, NeedFrame, Settling };
std::atomic<Phase> g_phase { Phase::Off };
std::atomic<int> g_presents { 0 };
std::atomic<bool> g_stopAsked { false };
std::atomic<uint64_t> g_written { 0 }, g_gameFrames { 0 };

HANDLE g_pipe = nullptr, g_process = nullptr;
HWND g_window = nullptr;
int g_width = 0, g_height = 0; // the part of the window that's recorded
int g_left = 0, g_top = 0;     // its corner in the window
double g_share = 0; // slow motion: video frames owed for the game frames so far
std::vector<uint8_t> g_pixels;
std::wstring g_file;
std::string g_error;
bool g_first = false; // the first wait of a recording, which only lets the studio's own drawing leave the screen

HWND GameWindow()
{
    struct Find {
        HWND Window = nullptr;
        long long Area = 0;
    } find;
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            RECT client {};
            if (owner != GetCurrentProcessId() || !IsWindowVisible(window) || !GetClientRect(window, &client))
                return TRUE;
            auto* find = reinterpret_cast<Find*>(param);
            const long long area = static_cast<long long>(client.right) * client.bottom;
            if (area > find->Area) {
                find->Area = area;
                find->Window = window;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    return find.Window;
}

std::wstring FindFfmpeg()
{
    // The studio's own setting first, then the one Tekken Studio's installer downloads, then the PATH.
    wchar_t buffer[MAX_PATH] = L"";
    if (FILE* file = _wfopen((DataDir() / "ffmpeg-path.txt").c_str(), L"r, ccs=UTF-8")) {
        if (fgetws(buffer, MAX_PATH, file)) {
            std::wstring path = buffer;
            while (!path.empty() && (path.back() == L'\n' || path.back() == L'\r' || path.back() == L' '))
                path.pop_back();
            fclose(file);
            if (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
                return path;
        } else {
            fclose(file);
        }
    }
    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    for (const wchar_t* folder : { L"tekken-studio", L"tekken8mvm" }) {
        const std::wstring path = std::wstring(local) + L"\\Programs\\" + folder + L"\\ffmpeg\\bin\\ffmpeg.exe";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return path;
    }
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buffer, nullptr))
        return buffer;
    return {};
}

std::wstring OutputFile()
{
    wchar_t profile[MAX_PATH] = L".";
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    const std::filesystem::path folder = std::filesystem::path(profile) / "Videos" / "tekken8mvm";
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf_s(name, L"ttt2_%04d%02d%02d_%02d%02d%02d.mp4", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return (folder / name).wstring();
}

bool StartFfmpeg()
{
    const std::wstring ffmpeg = FindFfmpeg();
    if (ffmpeg.empty()) {
        g_error = "ffmpeg wasn't found. Install Tekken Studio with its ffmpeg, or put ffmpeg on your PATH.";
        return false;
    }
    SECURITY_ATTRIBUTES inherit { sizeof(inherit), nullptr, TRUE };
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 64 << 20))
        return false;
    SetHandleInformation(write, HANDLE_FLAG_INHERIT, 0);
    g_file = OutputFile();
    // A portrait cut of a 16:9 window is narrow (607 wide at 1080p): brought up to 1080 wide, which is what phones are sent.
    const wchar_t* upscale = g_width < g_height && g_width < 1080 ? L"scale=1080:-2:flags=lanczos," : L"";
    wchar_t command[2048];
    swprintf_s(command,
               L"\"%s\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt bgr0 -s %dx%d -r 60 -i - -vf \"%sscale=out_color_matrix=bt709:out_range=tv\" "
               L"-colorspace bt709 -color_primaries bt709 -color_trc bt709 -c:v libx264 -preset medium -crf 15 -pix_fmt yuv420p \"%s\"",
               ffmpeg.c_str(), g_width, g_height, upscale, g_file.c_str());
    // ffmpeg gets the pipe's reading end and nothing else of this process's: any other open file handed down would stay locked
    // for as long as it runs.
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = read;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> list(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
    HANDLE handles[] = { read };
    BOOL started = FALSE;
    PROCESS_INFORMATION process {};
    if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)
        && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) {
        started = CreateProcessW(nullptr, command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                 &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }
    CloseHandle(read);
    if (!started) {
        CloseHandle(write);
        g_error = "ffmpeg couldn't be started.";
        return false;
    }
    CloseHandle(process.hThread);
    g_process = process.hProcess;
    g_pipe = write;
    return true;
}

// The game's picture as it is on the screen now.
bool Grab()
{
    POINT corner { 0, 0 };
    if (!g_window || !ClientToScreen(g_window, &corner))
        return false;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = g_width;
    info.bmiHeader.biHeight = -g_height; // top row first
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bitmap && bits) {
        HGDIOBJ old = SelectObject(memory, bitmap);
        ok = BitBlt(memory, 0, 0, g_width, g_height, screen, corner.x + g_left, corner.y + g_top, SRCCOPY) != 0;
        GdiFlush();
        if (ok)
            memcpy(g_pixels.data(), bits, g_pixels.size());
        SelectObject(memory, old);
    }
    if (bitmap)
        DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

void Finish()
{
    if (g_pipe)
        CloseHandle(g_pipe);
    g_pipe = nullptr;
    if (g_process) {
        WaitForSingleObject(g_process, 20000);
        CloseHandle(g_process);
    }
    g_process = nullptr;
    g_phase = Phase::Off;
    g_stopAsked = false;
    Log("Recorder: stopped after %llu game frames, %llu video frames", static_cast<unsigned long long>(g_gameFrames.load()),
        static_cast<unsigned long long>(g_written.load()));
}

} // namespace

bool RecorderActive()
{
    return g_phase.load(std::memory_order_relaxed) != Phase::Off;
}

uint64_t RecorderFrames()
{
    return g_written;
}

std::string RecorderMessage()
{
    if (!g_error.empty())
        return g_error;
    if (g_file.empty())
        return {};
    const std::filesystem::path file = g_file;
    return (RecorderActive() ? "Recording " : "Saved ") + file.filename().string();
}

std::filesystem::path RecorderFolder()
{
    wchar_t profile[MAX_PATH] = L".";
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return std::filesystem::path(profile) / "Videos" / "tekken8mvm";
}

void RecorderStart()
{
    if (RecorderActive())
        return;
    g_error.clear();
    g_window = GameWindow();
    RECT client {};
    if (!g_window || !GetClientRect(g_window, &client)) {
        g_error = "The game's window wasn't found.";
        return;
    }
    // The picture's shape, if one is on: only that part of the window. The video format wants even sizes.
    float left = 0, top = 0, right = 0, bottom = 0;
    camera::ShapeRect(static_cast<float>(client.right), static_cast<float>(client.bottom), left, top, right, bottom);
    g_left = static_cast<int>(left + 0.5f);
    g_top = static_cast<int>(top + 0.5f);
    g_width = static_cast<int>(right - left + 0.5f) & ~1;
    g_height = static_cast<int>(bottom - top + 0.5f) & ~1;
    if (g_width < 16 || g_height < 16)
        return;
    g_pixels.assign(static_cast<size_t>(g_width) * g_height * 4, 0);
    if (!StartFfmpeg()) {
        Log("Recorder: %s", g_error.c_str());
        return;
    }
    g_written = 0;
    g_gameFrames = 0;
    g_share = 0;
    g_presents = 0;
    g_stopAsked = false;
    Time().Paused = false;
    Log("Recorder: %d x %d to %ls", g_width, g_height, g_file.c_str());
    // The picture on screen now is from before the studio's own drawing stopped: a few presents go by before the first frame runs.
    g_phase = Phase::Settling;
    g_first = true;
}

void RecorderStop()
{
    if (RecorderActive())
        g_stopAsked = true;
}

// Game thread, once a game frame: whether the game may run this one.
bool RecorderAllowsFrame()
{
    if (g_phase.load(std::memory_order_relaxed) != Phase::NeedFrame)
        return false;
    ++g_gameFrames;
    g_presents = 0;
    g_phase = Phase::Settling;
    return true;
}

// The thread that draws the overlay, once a present.
void RecorderOnPresent()
{
    const Phase phase = g_phase.load(std::memory_order_relaxed);
    if (phase == Phase::Off)
        return;
    if (phase == Phase::NeedFrame) {
        // Waiting for the game to run a frame. If it never does (a menu came up), a stop still has to end the file.
        if (g_stopAsked)
            Finish();
        return;
    }
    if (++g_presents < SettlePresents)
        return;
    if (g_first) {
        // Nothing to write yet: this was only the wait for the studio's own drawing to leave the screen.
        g_first = false;
    } else if (Grab()) {
        // At a slow speed each game frame fills more than one video frame.
        g_share += 1.0 / std::fmax(Time().Speed, 0.05);
        int copies = static_cast<int>(g_share);
        g_share -= copies;
        for (; copies > 0; --copies) {
            DWORD done = 0;
            if (!WriteFile(g_pipe, g_pixels.data(), static_cast<DWORD>(g_pixels.size()), &done, nullptr) || done != g_pixels.size()) {
                g_error = "ffmpeg stopped taking frames.";
                g_stopAsked = true;
                break;
            }
            ++g_written;
        }
    }
    if (g_stopAsked) {
        Finish();
        return;
    }
    g_phase = Phase::NeedFrame;
}

} // namespace studio
