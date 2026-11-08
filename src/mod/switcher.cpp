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

enum class Stage { Idle, ToMatchEnd, EndMenu, ToList, TabClick, Rows, WaitForFight, Display };

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

// One thing at a time on the game's replay menu, with room between them for its animations.
struct Act {
    enum Kind { Key, Move, Press, Release, Wait } What;
    int KeyCode = 0;
    float X = 0, Y = 0; // in the 1456x819 frame of the game's picture
    ULONGLONG Ms = 0;
};
std::deque<Act> g_acts;
ULONGLONG g_actAt = 0;
bool g_reopenEditor = false;

POINT ClientPoint(float x, float y)
{
    HWND window = GetForegroundWindow();
    RECT client {};
    GetClientRect(window, &client);
    POINT point { static_cast<LONG>(client.right * x / 1456.0f), static_cast<LONG>(client.bottom * y / 819.0f) };
    ClientToScreen(window, &point);
    return point;
}

void MouseButton(bool down)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

// The game only follows the mouse when it sees it move, so each click comes after a small move onto the spot.
void QueueClick(float x, float y)
{
    g_acts.push_back({ Act::Move, 0, x - 8, y - 3, 500 });
    g_acts.push_back({ Act::Move, 0, x, y, 500 });
    g_acts.push_back({ Act::Press, 0, x, y, 40 }); // measured: 40 ms chooses, while 0 and 120 ms close the list without choosing
    g_acts.push_back({ Act::Release, 0, x, y, 800 });
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

const DisplayRow& DisplayRowInfo(int row)
{
    static const DisplayRow rows[DisplayRows] = {
        { "Player command history", 2 }, { "Opponent command history", 2 }, { "HUD", 2 }, { "Health info", 2 }, { "In-battle status (combo counter)", 2 },
        { "Player attack info", 2 }, { "Opponent attack info", 2 }, { "Player frame info", 4 }, { "Opponent frame info", 4 }, { "Hit properties (High, Mid, Low)", 2 },
        { "Recommended punishment", 3 }, { "Recommended sample combos", 3 }, { "High moves you can duck under", 3 }, { "Throw commands", 3 }, { "Version info", 2 },
    };
    return rows[row];
}

const char* DisplayOption(int row, int option)
{
    static const char* const shown[] = { "Display", "Hide" };
    static const char* const frame[] = { "Show details", "Simple display", "Show frame advantage", "Hide" };
    static const char* const tips[] = { "Display while paused", "Display without pausing", "Hide" };
    const int count = DisplayRowInfo(row).Options;
    if (option < 0 || option >= count)
        return "";
    return count == 4 ? frame[option] : (count == 3 ? tips[option] : shown[option]);
}

bool ApplyDisplay(const int (&choices)[DisplayRows], std::string& error)
{
    if (g_stage != Stage::Idle) {
        error = "The tool is busy with the game's screens";
        return false;
    }
    if (!replay::CanJump()) {
        error = "Open a replay first";
        return false;
    }
    bool any = false;
    for (int row = 0; row < DisplayRows; ++row)
        any = any || choices[row] >= 0;
    if (!any) {
        error = "Choose something to change first";
        return false;
    }
    g_reopenEditor = overlay::MenuOpen();
    if (g_reopenEditor)
        overlay::ToggleMenu();
    game::Time().Paused = false;

    // Measured at 1456x819: rows are 34 apart from 137, and a row's list opens at the row with its choices 34 apart.
    g_acts.clear();
    g_acts.push_back({ Act::Wait, 0, 0, 0, 500 });
    g_acts.push_back({ Act::Key, 'B', 0, 0, 1800 });
    for (int row = 0; row < DisplayRows; ++row) {
        if (choices[row] < 0)
            continue;
        const float y = 137.0f + 34.0f * row;
        QueueClick(650, y);
        QueueClick(1010, y + 34.0f * choices[row]);
    }
    g_acts.push_back({ Act::Key, 'K', 0, 0, 900 });
    g_actAt = 0;
    Enter(Stage::Display, "Setting the replay's display options");
    return true;
}

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
    g_acts.clear();
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
    if (g_stage == Stage::Display) {
        if (g_heldKey || now < g_actAt)
            return;
        if (g_acts.empty()) {
            g_stage = Stage::Idle;
            g_status.clear();
            mod::Log("Display options: done");
            if (g_reopenEditor && !overlay::MenuOpen())
                overlay::ToggleMenu();
            return;
        }
        const Act act = g_acts.front();
        g_acts.pop_front();
        g_actAt = now + act.Ms;
        if (act.What == Act::Key) {
            SendKey(act.KeyCode, true);
            g_heldKey = act.KeyCode;
            g_nextKeyAt = now + KeyHoldMs + 80;
        } else if (act.What == Act::Move) {
            const POINT point = ClientPoint(act.X, act.Y);
            SetCursorPos(point.x, point.y);
        } else if (act.What == Act::Press) {
            MouseButton(true);
        } else if (act.What == Act::Release) {
            MouseButton(false);
        }
        return;
    }
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
        // The Yes can land while the question is still opening. If it's still up, Yes is already picked, so confirm it again.
        if (look && keysDone && now - g_stageStart > 2500 && WidgetShown("WBP_UI_Dialog_C")) {
            g_keys = { 'M' };
            g_stageStart = now - 1000;
        }
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
