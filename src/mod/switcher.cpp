#include "mod/switcher.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <vector>

#include "mod/camera.h"
#include "mod/game.h"
#include "mod/gameui.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/replay.h"
#include "mod/sharing.h"
#include "mod/ue.h"

namespace switcher {

namespace {

constexpr ULONGLONG StepTimeoutMs = 45000; // the winner's scene can be long
constexpr ULONGLONG SettleMs = 1000; // a screen takes calls once it has finished opening; the old key presses waited this long too
constexpr double SceneSpeed = 4.0;

enum class Stage { Idle, ToMatchEnd, EndMenu, ToList, Tab, Row, Details, Side, WaitForFight };

Stage g_stage = Stage::Idle;
ULONGLONG g_stageStart = 0;
int g_slot = -1;
int g_tab = gameui::TabMine;
int g_mark = 0; // what the stage waits to see change: questions shown, list fills
int g_tries = 0;
ULONGLONG g_readySince = 0;
ULONGLONG g_selectedAt = 0;
int g_startFightCount = 0;
std::string g_status;

void Enter(Stage stage, const char* status)
{
    g_stage = stage;
    g_stageStart = GetTickCount64();
    g_tries = 0;
    g_readySince = 0;
    g_selectedAt = 0;
    g_status = status;
    mod::Log("Switch: %s", status);
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

void Fail(const std::string& why)
{
    mod::Log("Switch stopped: %s", why.c_str());
    game::Time().Speed = 1.0;
    if (g_stage == Stage::ToMatchEnd)
        game::StopSeek();
    g_status = why;
    g_stage = Stage::Idle;
}

// The button of a question box whose name has one of the words in it, as the game names it (its text, or the key of its text).
int ButtonWith(std::initializer_list<const char*> words, int fallback)
{
    const std::vector<std::string> buttons = gameui::QuestionButtons();
    for (size_t i = 0; i < buttons.size(); ++i) {
        std::string lower = buttons[i];
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const char* word : words)
            if (lower.find(word) != std::string::npos)
                return static_cast<int>(i);
    }
    return fallback;
}

std::string ButtonList()
{
    std::string text;
    for (const std::string& button : gameui::QuestionButtons())
        text += (text.empty() ? "" : ", ") + button;
    return text;
}

// True once `ready` has held for SettleMs.
bool Settled(bool ready)
{
    const ULONGLONG now = GetTickCount64();
    if (!ready) {
        g_readySince = 0;
        return false;
    }
    if (!g_readySince)
        g_readySince = now;
    return now - g_readySince >= SettleMs;
}

// Takes a button of the details box or a question box. The box acts on the button its cursor is on, so the cursor goes there first,
// and the button is only taken once the box says its cursor is on it. True once taken; false while still on the way, with `failed`
// set when the cursor didn't get there.
bool Choose(bool details, int id, bool& failed)
{
    failed = false;
    const ULONGLONG now = GetTickCount64();
    const int cursor = details ? gameui::DetailsCursor() : gameui::QuestionCursor();
    if (cursor == id && (g_selectedAt == 0 || now - g_selectedAt > 150)) {
        g_selectedAt = 0;
        return details ? gameui::DecideDetails(id) : gameui::DecideQuestion(id);
    }
    if (g_selectedAt == 0) {
        g_selectedAt = now;
        if (cursor != id) {
            if (details)
                gameui::SelectDetails(id);
            else
                gameui::SelectQuestion(id);
        }
    } else if (now - g_selectedAt > 2000) {
        failed = true;
    }
    return false;
}

} // namespace

bool Start(int slot, std::string& error)
{
    if (g_stage != Stage::Idle) {
        error = "Already changing replay";
        return false;
    }
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

    const bool inList = gameui::ListUp();
    // A replay's box open in the list: Play Replay is right there if it's this replay. Otherwise the box has to go first, and only
    // the player can close it.
    const bool inBox = !inList && gameui::DetailsUp();
    if (inBox && !gameui::DetailsShow(slot)) {
        error = "A different replay's box is open in the game. Press Back there first.";
        return false;
    }
    // At the end of a replay its menu (Play Again, Return to Replay List, Return to Main Menu) is already up.
    const bool atEnd = !inList && !inBox && gameui::QuestionUp() && gameui::QuestionButtons().size() == 3;
    uint32_t endFrame = 0;
    for (const replay::SavedPoint& point : replay::SavedPoints())
        if (point.Type == replay::TypeMatchEnd)
            endFrame = point.Frame;
    if (!inList && !inBox && !atEnd && (!endFrame || !replay::CanJump())) {
        error = replay::CanJump() || game::FightCount() == 0 ? "Open a replay or the game's replay list first"
                                                              : "The replay is still getting ready. Try again in a few seconds.";
        return false;
    }

    g_slot = slot;
    g_tab = chosen->Downloaded ? gameui::TabDownloaded : gameui::TabMine;
    if (overlay::MenuOpen())
        overlay::ToggleMenu();
    camera::Config().Mode = camera::Mode::Game;
    game::TimeControl& time = game::Time();
    time.Paused = false;
    time.UseSpeedRamp = false;
    time.Speed = 1.0;
    g_startFightCount = game::FightCount();
    g_mark = gameui::QuestionsShown();
    if (inBox) {
        Enter(Stage::Details, "Starting the replay");
    } else if (inList) {
        Enter(Stage::Tab, "Finding the replay in the list");
    } else if (atEnd) {
        g_mark = -1; // the menu that's up is the one to answer
        Enter(Stage::ToMatchEnd, "Leaving this replay");
    } else {
        // The winner's scene needn't take its full time.
        time.Speed = SceneSpeed;
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
    const ULONGLONG age = now - g_stageStart;
    if (age > StepTimeoutMs && g_stage != Stage::WaitForFight) {
        Fail("The game's screens didn't come up. Nothing was changed.");
        return;
    }
    // Looking at the screens isn't free, so it's done a few times a second.
    static ULONGLONG nextLook = 0;
    if (now < nextLook)
        return;
    nextLook = now + 100;

    switch (g_stage) {
    case Stage::ToMatchEnd:
        // The end of the replay asks: Play Again, Return to Replay List, Return to Main Menu.
        if (Settled(gameui::QuestionsShown() != g_mark && gameui::QuestionUp())) {
            game::StopSeek();
            game::Time().Speed = 1.0;
            // Play Again, Return to Replay List, Return to Main Menu, in that order on 3.03.01 (keys TEXT_000_UI_MYREPLAY_021 to 023).
            const int id = ButtonWith({ "list", "myreplay_022" }, 1);
            if (g_selectedAt == 0)
                mod::Log("Switch: end menu has %s; taking %d", ButtonList().c_str(), id);
            bool failed = false;
            if (Choose(false, id, failed)) {
                g_mark = gameui::QuestionsShown();
                Enter(Stage::EndMenu, "Leaving this replay");
            } else if (failed) {
                Fail("Couldn't leave this replay");
            }
        } else if (!game::Seeking() && !gameui::QuestionUp()) {
            // A replay that a tip has paused doesn't move, so it never gets to its end.
            static int64_t lastPosition = -1;
            static ULONGLONG stillSince = 0;
            const int64_t position = game::Position();
            if (position != lastPosition || age < 1000) {
                lastPosition = position;
                stillSince = now;
            } else if (now - stillSince > 4000) {
                Fail("The replay didn't go to its end. If one of the game's tips has paused it, press K (Pause/Play) with the editor closed and try again.");
            }
        }
        break;
    case Stage::EndMenu:
        // Then it asks to be sure, starting on No.
        if (gameui::ListUp()) {
            Enter(Stage::Tab, "Finding the replay in the list");
        } else if (Settled(gameui::QuestionsShown() != g_mark && gameui::QuestionUp())) {
            // Yes, No (TEXT_000_UI_COMMON_006 and 007), with the cursor starting on No.
            const int id = ButtonWith({ "yes", "common_006" }, 0);
            if (g_selectedAt == 0)
                mod::Log("Switch: the question has %s; taking %d", ButtonList().c_str(), id);
            bool failed = false;
            if (Choose(false, id, failed))
                Enter(Stage::ToList, "Going back to the replay list");
            else if (failed)
                Fail("Couldn't answer the game's question");
        }
        break;
    case Stage::ToList:
        if (Settled(gameui::ListUp() && gameui::ListRows() > 0))
            Enter(Stage::Tab, "Finding the replay in the list");
        break;
    case Stage::Tab:
        if (!Settled(gameui::ListUp()))
            break;
        if (gameui::ListTab() == g_tab && gameui::ListRows() > 0) {
            Enter(Stage::Row, "Finding the replay in the list");
        } else if (g_tries == 0 || age > 3000 * static_cast<ULONGLONG>(g_tries)) {
            if (g_tries >= 2) {
                Fail("The game's list didn't change tab");
                return;
            }
            ++g_tries;
            gameui::OpenTab(g_tab);
        }
        break;
    case Stage::Row: {
        if (!Settled(gameui::ListUp()))
            break;
        const int row = gameui::RowOfSlot(g_slot);
        if (row < 0) {
            Fail("That replay isn't in the game's list");
            return;
        }
        mod::Log("Switch: slot %d is row %d of tab %d", g_slot, row, g_tab);
        if (!gameui::OpenRow(row)) {
            Fail("Couldn't open the replay in the game's list");
            return;
        }
        Enter(Stage::Details, "Starting the replay");
        break;
    }
    case Stage::Details:
        if (Settled(gameui::DetailsUp())) {
            if (!gameui::DetailsShow(g_slot)) {
                Fail("The game opened a different replay (slot " + std::to_string(gameui::DetailsSlot()) + "). Press Back in the game.");
                return;
            }
            bool failed = false;
            if (Choose(true, 0, failed)) { // Play Replay
                g_mark = gameui::QuestionsShown();
                Enter(Stage::Side, "Starting the replay");
            } else if (failed) {
                Fail("Couldn't press Play Replay");
            }
        } else if (age > 4000 && g_tries == 0 && gameui::ListUp()) {
            // The list can miss a call while it's still settling.
            ++g_tries;
            gameui::OpenRow(gameui::RowOfSlot(g_slot));
        }
        break;
    case Stage::Side:
        // Which side to show tips for. Some replays go straight to loading.
        if (game::FightCount() != g_startFightCount) {
            Enter(Stage::WaitForFight, "Loading the replay");
        } else if (Settled(gameui::QuestionsShown() != g_mark && gameui::QuestionUp())) {
            bool failed = false;
            if (Choose(false, ButtonWith({ "left" }, 0), failed))
                Enter(Stage::WaitForFight, "Loading the replay");
            else if (failed)
                Fail("Couldn't answer which side to show tips for");
        }
        break;
    case Stage::WaitForFight:
        // The fighters are there before the versus screen, and the replay's own screen only comes when it's playing.
        if (game::FightCount() != g_startFightCount && WidgetShown("WBP_UI_Replay_C")) {
            g_stage = Stage::Idle;
            g_status.clear();
            mod::Log("Switch: done");
            if (!overlay::MenuOpen())
                overlay::ToggleMenu();
        } else if (age > 60000) {
            Fail("The replay didn't load");
        }
        break;
    default:
        break;
    }
}

} // namespace switcher
