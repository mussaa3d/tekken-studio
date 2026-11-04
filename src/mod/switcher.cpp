#include "mod/switcher.h"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <vector>

#include "mod/camera.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/replay.h"
#include "mod/sharing.h"
#include "mod/ue.h"

namespace switcher {

namespace {

constexpr ULONGLONG KeyHoldMs = 120, KeyGapMs = 350;
constexpr ULONGLONG ScreenTimeoutMs = 45000; // the winner's scene can be long
constexpr double SceneSpeed = 4.0;

enum class Stage { Idle, ToMatchEnd, EndMenu, ToList, TabClick, Rows, WaitForFight };

Stage g_stage = Stage::Idle;
std::deque<int> g_keys;
int g_heldKey = 0;
ULONGLONG g_nextKeyAt = 0, g_stageStart = 0;
int g_row = 0;
bool g_downloadedTab = false;
int g_startFightCount = 0;
std::string g_status;

// In the game's screens at 1456x819.
constexpr float TabX[2] = { 674.0f / 1456.0f, 516.0f / 1456.0f }; // My Replays & Tips, Downloaded Replays
constexpr float TabY = 97.0f / 819.0f;

void SendKey(int virtualKey, bool down)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(virtualKey);
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

bool WidgetShown(const char* className)
{
    void* cls = ue::FindClass(className);
    if (!cls)
        return false;
    static ue::FunctionRef visible { "Widget", "IsVisible" };
    for (void* widget : ue::AllObjectsOf(cls)) {
        if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        struct {
            bool ReturnValue;
        } params { false };
        ue::Call(widget, visible, &params);
        if (params.ReturnValue)
            return true;
    }
    return false;
}

void Enter(Stage stage, const char* status)
{
    g_stage = stage;
    g_stageStart = GetTickCount64();
    g_status = status;
    mod::Log("Switch: %s", status);
}

void Fail(const char* why)
{
    mod::Log("Switch stopped: %s", why);
    game::Time().Speed = 1.0;
    g_status = why;
    g_stage = Stage::Idle;
    g_keys.clear();
    if (g_heldKey) {
        SendKey(g_heldKey, false);
        g_heldKey = 0;
    }
}

void QueueRows()
{
    for (int i = 0; i < g_row; ++i)
        g_keys.push_back('S');
    g_keys.push_back('M'); // opens the details box, where Play Replay is first
    g_keys.push_back('M');
    g_keys.push_back(0);
    g_keys.push_back(0);
    g_keys.push_back(0);
    g_keys.push_back('M'); // which side to show tips for
}

void ClickTab()
{
    HWND window = GetForegroundWindow();
    RECT client {};
    GetClientRect(window, &client);
    POINT point { static_cast<LONG>(client.right * TabX[g_downloadedTab ? 1 : 0]), static_cast<LONG>(client.bottom * TabY) };
    ClientToScreen(window, &point);
    SetCursorPos(point.x, point.y);
    Sleep(30);
    INPUT input[2] = {};
    input[0].type = input[1].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, input, sizeof(INPUT));
}

} // namespace

bool Start(int slot, std::string& error)
{
    if (g_stage != Stage::Idle) {
        error = "Already changing replay";
        return false;
    }
    // The game's list has the newest slot on top, with downloaded replays on their own tab.
    const std::vector<sharing::Slot> slots = sharing::Slots();
    const auto chosen = std::find_if(slots.begin(), slots.end(), [&](const sharing::Slot& s) { return s.Index == slot; });
    if (chosen == slots.end()) {
        error = "That replay is gone. Press Refresh.";
        return false;
    }
    // The game refuses replays from older versions, and the newest version among them is the one running, near enough.
    uint32_t newest = 0;
    for (const sharing::Slot& s : slots)
        newest = std::max(newest, s.GameVersion);
    if (chosen->GameVersion != newest) {
        error = "That replay is from an older game version, so the game won't play it";
        return false;
    }
    g_row = static_cast<int>(std::count_if(slots.begin(), slots.end(), [&](const sharing::Slot& s) { return s.Downloaded == chosen->Downloaded && s.Index > slot; }));
    g_downloadedTab = chosen->Downloaded;

    const bool inList = WidgetShown("WBP_UI_Replay_List_C");
    uint32_t endFrame = 0;
    for (const replay::SavedPoint& point : replay::SavedPoints())
        if (point.Type == replay::TypeMatchEnd)
            endFrame = point.Frame;
    if (!inList && (!endFrame || !replay::CanJump())) {
        error = "Open a replay or the game's replay list first";
        return false;
    }

    if (overlay::MenuOpen())
        overlay::ToggleMenu();
    camera::Config().Mode = camera::Mode::Game;
    game::TimeControl& time = game::Time();
    time.Paused = false;
    time.UseSpeedRamp = false;
    time.Speed = 1.0;
    g_keys.clear();
    g_startFightCount = game::FightCount();
    if (inList) {
        Enter(Stage::TabClick, "Finding the replay in the list");
    } else {
        // If the end menu is already up there's nothing to go through, and otherwise the winner's scene needn't take its full time.
        time.Speed = SceneSpeed;
        if (!WidgetShown("WBP_UI_Dialog_C"))
            game::SeekToRound(endFrame);
        Enter(Stage::ToMatchEnd, "Going to the end of this replay");
    }
    return true;
}

void Cancel()
{
    if (g_stage != Stage::Idle)
        Fail("Cancelled");
}

bool Running()
{
    return g_stage != Stage::Idle;
}

std::string Status()
{
    return g_status;
}

void OnGameTick()
{
    if (g_stage == Stage::Idle)
        return;
    const ULONGLONG now = GetTickCount64();

    if (g_heldKey && now >= g_nextKeyAt) {
        SendKey(g_heldKey, false);
        g_heldKey = 0;
        g_nextKeyAt = now + KeyGapMs;
    }
    // Keys go to whatever is in front, so they wait for the game to be there.
    if (!overlay::IsGameForeground()) {
        g_status = "Waiting: click back into the game to continue";
        g_stageStart = now;
        return;
    }
    if (!g_heldKey && !g_keys.empty() && now >= g_nextKeyAt) {
        const int key = g_keys.front();
        g_keys.pop_front();
        if (key) {
            SendKey(key, true);
            g_heldKey = key;
            g_nextKeyAt = now + KeyHoldMs;
        } else {
            g_nextKeyAt = now + 1000; // a screen is on its way
        }
    }
    const bool keysDone = g_keys.empty() && !g_heldKey && now >= g_nextKeyAt;
    // Looking through the game's objects isn't free, so screens are only checked a few times a second.
    static ULONGLONG nextLook = 0;
    const bool look = now >= nextLook;
    if (look)
        nextLook = now + 250;
    if (now - g_stageStart > ScreenTimeoutMs && g_stage != Stage::WaitForFight) {
        Fail("The game's screens didn't come up. Nothing was changed.");
        return;
    }

    switch (g_stage) {
    case Stage::ToMatchEnd:
        if (look && WidgetShown("WBP_UI_Dialog_C")) {
            game::StopSeek();
            game::Time().Speed = 1.0;
            g_keys = { 0, 'S', 'M', 0, 'W', 'M' }; // Return to Replay List, then Yes on the question that follows, which starts on No
            Enter(Stage::EndMenu, "Leaving this replay");
        }
        break;
    case Stage::EndMenu:
        if (keysDone)
            Enter(Stage::ToList, "Going back to the replay list");
        break;
    case Stage::ToList:
        if (look && WidgetShown("WBP_UI_Replay_List_C")) {
            g_keys = { 0, 0 };
            Enter(Stage::TabClick, "Finding the replay in the list");
        }
        break;
    case Stage::TabClick:
        if (keysDone) {
            ClickTab();
            g_keys = { 0 };
            QueueRows();
            Enter(Stage::Rows, "Starting the replay");
        }
        break;
    case Stage::Rows:
        if (keysDone)
            Enter(Stage::WaitForFight, "Loading the replay");
        break;
    case Stage::WaitForFight:
        // The fighters are there before the versus screen, and the replay's own screen only comes when it's playing.
        if (look && game::FightCount() != g_startFightCount && WidgetShown("WBP_UI_Replay_C")) {
            g_stage = Stage::Idle;
            g_status.clear();
            mod::Log("Switch: done");
            if (!overlay::MenuOpen())
                overlay::ToggleMenu();
        } else if (now - g_stageStart > 60000) {
            Fail("The replay didn't load");
        }
        break;
    default:
        break;
    }
}

} // namespace switcher
