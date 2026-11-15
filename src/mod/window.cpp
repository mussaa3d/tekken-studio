#include "mod/window.h"

#include <windows.h>

#include <atomic>
#include <string>

#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/ue.h"

namespace window {

namespace {

std::atomic<int> g_wantWidth { 0 }, g_wantHeight { 0 };
std::atomic<bool> g_oversize { false };

// The window as it was before the first capture size, to go back to.
struct Original {
    bool Valid = false;
    int Width = 0, Height = 0;
    RECT Rect {};
    bool Maximized = false;
    wchar_t Mode = L'w'; // w a window, b borderless, f fullscreen
} g_original;

int g_appliedWidth = 0, g_appliedHeight = 0;
enum class Pending { Nothing, BecomingWindow, Capture, Restore };
Pending g_pending = Pending::Nothing;
int g_settle = 0;

void ClientSize(HWND window, int& width, int& height)
{
    RECT client {};
    GetClientRect(window, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
}

bool HasCaption(HWND window)
{
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION;
}

void SetResolution(int width, int height, const wchar_t* mode)
{
    const std::wstring command = L"r.SetRes " + std::to_wstring(width) + L"x" + std::to_wstring(height) + mode;
    mod::Log("Window: %ls", command.c_str());
    ue::ConsoleCommand(command);
}

// Measured: the engine's own resolution command won't go past the desktop's size, and Windows won't either unless the window says it may
// (WM_GETMINMAXINFO, answered in the overlay). Sizing the window directly does it, and the engine follows the window: the back buffer
// was 3840 x 2160 on a 1920 x 1080 monitor.
void ResizeTo(HWND window, int width, int height)
{
    RECT frame { 0, 0, width, height };
    AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)));
    // The picture's corner goes on the corner of the monitor the game is on, whichever monitor that is.
    MONITORINFO monitor { sizeof(monitor) };
    GetMonitorInfoW(MonitorFromRect(&g_original.Rect, MONITOR_DEFAULTTONEAREST), &monitor);
    if (IsZoomed(window))
        ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, nullptr, monitor.rcMonitor.left + frame.left, monitor.rcMonitor.top + frame.top, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

void SetCaptureSize(int width, int height)
{
    g_wantWidth = width;
    g_wantHeight = height;
}

bool AllowsOversize()
{
    return g_oversize;
}

void PictureSize(int& width, int& height)
{
    width = height = 0;
    if (const HWND window = overlay::GameWindow())
        ClientSize(window, width, height);
}

void OnGameTick()
{
    const HWND window = overlay::GameWindow();
    if (!window)
        return;
    int width = 0, height = 0;
    ClientSize(window, width, height);

    const int wantWidth = g_wantWidth, wantHeight = g_wantHeight;
    if (wantWidth != g_appliedWidth || wantHeight != g_appliedHeight) {
        g_appliedWidth = wantWidth;
        g_appliedHeight = wantHeight;
        g_settle = 600;
        if (wantWidth > 0 && wantHeight > 0) {
            if (!g_original.Valid) {
                g_original.Valid = true;
                g_original.Width = width;
                g_original.Height = height;
                GetWindowRect(window, &g_original.Rect);
                g_original.Maximized = IsZoomed(window) != 0;
                g_original.Mode = HasCaption(window) ? L'w' : (overlay::ExclusiveFullscreen() ? L'f' : L'b');
                mod::Log("Window: was %d x %d, %s", width, height, g_original.Mode == L'w' ? "a window" : (g_original.Mode == L'f' ? "fullscreen" : "borderless"));
            }
            g_oversize = true;
            if (HasCaption(window)) {
                ResizeTo(window, wantWidth, wantHeight);
                g_pending = Pending::Capture;
            } else {
                // Borderless and fullscreen draw at the monitor's size whatever they're asked for. Only a window can be any size.
                SetResolution(width, height, L"w");
                g_pending = Pending::BecomingWindow;
            }
        } else if (g_original.Valid) {
            g_oversize = false;
            if (g_original.Mode != L'w')
                SetResolution(g_original.Width, g_original.Height, g_original.Mode == L'f' ? L"f" : L"wf");
            else if (g_original.Maximized)
                ShowWindow(window, SW_MAXIMIZE);
            else
                SetWindowPos(window, nullptr, g_original.Rect.left, g_original.Rect.top, g_original.Rect.right - g_original.Rect.left, g_original.Rect.bottom - g_original.Rect.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            g_pending = Pending::Restore;
        }
        return;
    }

    switch (g_pending) {
    case Pending::Nothing:
        break;
    case Pending::BecomingWindow:
        if (HasCaption(window) || --g_settle <= 0) {
            ResizeTo(window, wantWidth, wantHeight);
            g_pending = Pending::Capture;
            g_settle = 600;
        }
        break;
    case Pending::Capture:
        if (width == wantWidth && height == wantHeight) {
            mod::Log("Window: the picture is %d x %d", width, height);
            g_pending = Pending::Nothing;
        } else if (--g_settle <= 0) {
            mod::Log("Window: asked for %d x %d, the game gave %d x %d", wantWidth, wantHeight, width, height);
            g_pending = Pending::Nothing;
        }
        break;
    case Pending::Restore:
        if ((width == g_original.Width && height == g_original.Height) || --g_settle <= 0) {
            mod::Log("Window: back to %d x %d", width, height);
            g_original.Valid = false;
            g_pending = Pending::Nothing;
        }
        break;
    }
}

} // namespace window
