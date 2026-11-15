#include "mod/gameui.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../../third_party/minhook/src/hde/hde64.h"
#include "mod/events.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/overlay.h"
#include "mod/scan.h"
#include "mod/sharing.h"
#include "mod/ue.h"

namespace gameui {

namespace {

// The events the game sends its replay screens that matter here. Parameter offsets are from the game's own reflection data
// (the fnx test command), on 3.03.01.
enum class Kind : uint8_t {
    None,
    ListCommon,     // SetReplayCommonInfo(Index, data_no @0x4, favorite, date_text @0x10, ...)
    ListPlayerInfo, // SetReplayPlayerInfo(Index, side, three textures, player_name_text @0x20, ...)
    ListPlayerName, // SetReplayPlayerName(Index, side, player_name_text @0x8)
    ListClear,      // ClearButton()
    ListSelectTab,  // SelectTab(Index)
    ListIn,
    ListOut,
    ListShadeOn, // a box went over the list
    ListShadeOff,
    DetailsCommon,     // SetReplayCommonInfo(date_text @0, ...)
    DetailsPlayerInfo, // SetReplayPlayerInfo(side, three textures, player_name_text @0x20, ...)
    DetailsPlayerName, // SetReplayPlayerName(side, player_name_text @0x8)
    DetailsClear,      // ClearButtons()
    DetailsAdd,        // AddButton(Text @0, is_raw_text @0x10, is_enable @0x11)
    DetailsUpdate,     // UpdateButton(ID, Text @0x8, is_raw_text @0x18, is_enable @0x19)
    DetailsIn,
    DetailsOut,
    DetailsSelect,       // SelectMenu(ID): the game moved the cursor there
    DetailsDecide,       // DecideMenu(ID): the game took that button
    QuestionDescription, // SetDescription(Text @0, ...)
    QuestionUpdate,      // UpdateButton(ID, Text @0x8, is_raw_text @0x18, is_enable @0x19, ...)
    QuestionSelect,      // SelectMenu(ID): the cursor is on that button
    QuestionIn,
    QuestionOut,
};

struct Widget {
    void* Object = nullptr;
    uint32_t Index = 0;
    bool Up = false;
    void Set(void* object)
    {
        Object = object;
        Index = ue::Field<uint32_t>(object, ue::off::InternalIndex);
    }
    bool Alive() const { return Object && ue::StillAlive(Object, Index); }
};

struct Replay {
    std::string Date, Names[2];
    int Id = -1; // the list's ID for the row (its data_no), which is what a click on it passes
};

struct Button {
    std::string Text;
    bool Raw = false, Enabled = false;
};

// Which kind each function is. The function's name is kept with it, so a function freed and another made at the same address
// isn't taken for it.
struct Known {
    Kind What;
    uint64_t Name;
};
std::mutex g_cacheMutex;
std::unordered_map<void*, Known> g_cache;
std::atomic<void*> g_listClass { nullptr }, g_detailsClass { nullptr }, g_questionClass { nullptr };

// The screens as the game last described them. Events come on the game thread and the editor reads from the render thread.
// Never held while calling into the engine: that can fire an event, which takes it again.
std::mutex g_mutex;
Widget g_list, g_details, g_question;
bool g_listShaded = false;
int g_tab = -1, g_fills = 0, g_questions = 0;
std::vector<Replay> g_rows;
Replay g_detailsReplay;
std::vector<Button> g_detailsButtons, g_questionButtons;
int g_detailsSelected = 0, g_questionSelected = 0;
bool g_detailsOpened = false; // the box just came up, so its Export button goes in on the next tick
bool g_playDecided = false;   // Play Replay was taken in the box...
Replay g_played;              // ...for this replay
int g_seenFight = 0;

std::atomic<int> g_openSlot { -1 };

// The details box's Export Replay button: which box, which of its buttons, which slot.
std::atomic<void*> g_exportBox { nullptr };
std::atomic<int> g_exportId { -1 };
std::atomic<int> g_exportSlot { -1 };

using DecideFn = void (*)(void* widget, int id);
DecideFn oDecide = nullptr;
bool g_decideHooked = false;

std::string Utf8(const wchar_t* text, int length)
{
    if (!text || length <= 0)
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(bytes, 0)), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string Str(const uint8_t* params, size_t offset)
{
    const auto& text = *reinterpret_cast<const ue::FString*>(params + offset);
    if (!text.Data || text.Num <= 0 || text.Num > 4096)
        return {};
    int length = text.Num;
    while (length > 0 && text.Data[length - 1] == 0)
        --length;
    return Utf8(text.Data, length);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Kind Classify(void* function)
{
    struct Entry {
        const char* Name;
        Kind What;
    };
    static const Entry list[] = { { "SetReplayCommonInfo", Kind::ListCommon }, { "SetReplayPlayerInfo", Kind::ListPlayerInfo }, { "SetReplayPlayerName", Kind::ListPlayerName },
                                  { "ClearButton", Kind::ListClear }, { "SelectTab", Kind::ListSelectTab }, { "PlayAnimIn", Kind::ListIn }, { "PlayAnimOut", Kind::ListOut },
                                  { "PlayAnimShadeOn", Kind::ListShadeOn }, { "PlayAnimShadeOff", Kind::ListShadeOff } };
    static const Entry details[] = { { "SetReplayCommonInfo", Kind::DetailsCommon }, { "SetReplayPlayerInfo", Kind::DetailsPlayerInfo }, { "SetReplayPlayerName", Kind::DetailsPlayerName },
                                     { "ClearButtons", Kind::DetailsClear }, { "AddButton", Kind::DetailsAdd }, { "UpdateButton", Kind::DetailsUpdate }, { "PlayAnimIn", Kind::DetailsIn },
                                     { "PlayAnimOut", Kind::DetailsOut }, { "SelectMenu", Kind::DetailsSelect }, { "DecideMenu", Kind::DetailsDecide } };
    static const Entry question[] = { { "SetDescription", Kind::QuestionDescription }, { "UpdateButton", Kind::QuestionUpdate }, { "PlayAnimIn", Kind::QuestionIn },
                                      { "PlayAnimOut", Kind::QuestionOut }, { "SelectMenu", Kind::QuestionSelect } };
    // The class the function belongs to. A delegate's signature can belong to a package instead, which has no parents to walk.
    void* owner = ue::Field<void*>(function, 0x20);
    if (!owner || !scan::IsReadable(owner, 0x50))
        return Kind::None;
    void* ownerType = ue::Field<void*>(owner, ue::off::ClassPrivate);
    if (!ownerType || !scan::IsReadable(ownerType, ue::off::ClassCastFlags + 8) || !(ue::Field<uint64_t>(ownerType, ue::off::ClassCastFlags) & ue::off::CASTCLASS_UClass))
        return Kind::None;
    const auto pick = [&](const Entry* entries, size_t count) {
        const std::string name = ue::NameOf(function);
        for (size_t i = 0; i < count; ++i)
            if (name == entries[i].Name)
                return entries[i].What;
        return Kind::None;
    };
    if (void* cls = g_listClass.load(); cls && ue::IsChildOf(owner, cls))
        return pick(list, std::size(list));
    if (void* cls = g_detailsClass.load(); cls && ue::IsChildOf(owner, cls))
        return pick(details, std::size(details));
    if (void* cls = g_questionClass.load(); cls && ue::IsChildOf(owner, cls))
        return pick(question, std::size(question));
    return Kind::None;
}

void SetButtonText(std::vector<Button>& buttons, int id, Button button)
{
    if (id < 0 || id >= 64)
        return;
    if (static_cast<int>(buttons.size()) <= id)
        buttons.resize(static_cast<size_t>(id) + 1);
    buttons[static_cast<size_t>(id)] = std::move(button);
}

void Handle(Kind kind, void* object, const uint8_t* p)
{
    const auto Int = [&](size_t at) { return *reinterpret_cast<const int32_t*>(p + at); };
    const auto Bool = [&](size_t at) { return p[at] != 0; };
    const auto Row = [&](int index) -> Replay* {
        if (index < 0 || index >= 10000)
            return nullptr;
        if (static_cast<int>(g_rows.size()) <= index)
            g_rows.resize(static_cast<size_t>(index) + 1);
        return &g_rows[static_cast<size_t>(index)];
    };
    const auto Side = [](int side) { return side == 1 ? 1 : 0; };
    std::lock_guard lock(g_mutex);
    switch (kind) {
    case Kind::ListCommon:
        g_list.Set(object);
        if (Replay* row = Row(Int(0))) {
            row->Date = Str(p, 0x10);
            row->Id = Int(4);
        }
        break;
    case Kind::ListPlayerInfo:
        if (Replay* row = Row(Int(0)))
            row->Names[Side(Int(4))] = Str(p, 0x20);
        break;
    case Kind::ListPlayerName:
        if (Replay* row = Row(Int(0)))
            row->Names[Side(Int(4))] = Str(p, 0x8);
        break;
    case Kind::ListClear:
        g_list.Set(object);
        g_rows.clear();
        ++g_fills;
        break;
    case Kind::ListSelectTab:
        g_tab = Int(0);
        break;
    case Kind::ListIn:
        g_list.Set(object);
        g_list.Up = true;
        g_listShaded = false;
        g_playDecided = false;
        g_openSlot = -1; // nothing is playing while the list is up
        break;
    case Kind::ListOut:
        g_list.Up = false;
        break;
    case Kind::ListShadeOn:
        g_listShaded = true;
        break;
    case Kind::ListShadeOff:
        g_listShaded = false;
        break;
    case Kind::DetailsCommon:
        g_details.Set(object);
        g_detailsReplay = {};
        g_detailsReplay.Date = Str(p, 0);
        break;
    case Kind::DetailsPlayerInfo:
        g_detailsReplay.Names[Side(Int(0))] = Str(p, 0x20);
        break;
    case Kind::DetailsPlayerName:
        g_detailsReplay.Names[Side(Int(0))] = Str(p, 0x8);
        break;
    case Kind::DetailsClear:
        g_detailsButtons.clear();
        g_exportBox = nullptr;
        g_exportId = -1;
        break;
    case Kind::DetailsAdd:
        g_detailsButtons.push_back({ Str(p, 0), Bool(0x10), Bool(0x11) });
        break;
    case Kind::DetailsUpdate:
        SetButtonText(g_detailsButtons, Int(0), { Str(p, 0x8), Bool(0x18), Bool(0x19) });
        break;
    case Kind::DetailsIn:
        g_details.Set(object);
        g_details.Up = true;
        g_detailsOpened = true;
        g_playDecided = false;
        break;
    case Kind::DetailsOut:
        g_details.Up = false;
        g_exportBox = nullptr;
        g_exportId = -1;
        break;
    case Kind::DetailsSelect:
        g_detailsSelected = Int(0);
        break;
    case Kind::DetailsDecide:
        if (Int(0) == 0) {
            g_playDecided = true;
            g_played = g_detailsReplay;
        }
        break;
    case Kind::QuestionDescription:
        g_question.Set(object);
        g_questionButtons.clear();
        break;
    case Kind::QuestionUpdate:
        SetButtonText(g_questionButtons, Int(0), { Str(p, 0x8), Bool(0x18), Bool(0x19) });
        break;
    case Kind::QuestionSelect:
        g_questionSelected = Int(0);
        break;
    case Kind::QuestionIn:
        g_question.Set(object);
        g_question.Up = true;
        ++g_questions;
        break;
    case Kind::QuestionOut:
        g_question.Up = false;
        break;
    default:
        break;
    }
}

void OnEvent(void* object, void* function, void* params)
{
    if (!function || !object)
        return;
    Kind kind;
    {
        std::lock_guard lock(g_cacheMutex);
        const uint64_t name = ue::Field<uint64_t>(function, ue::off::NamePrivate);
        const auto found = g_cache.find(function);
        if (found != g_cache.end() && found->second.Name == name) {
            kind = found->second.What;
        } else {
            kind = Classify(function);
            g_cache[function] = { kind, name };
        }
    }
    if (kind != Kind::None)
        Handle(kind, object, static_cast<const uint8_t*>(params));
}

bool Shows(const Replay& replay, const sharing::Slot& slot, bool& namesToo)
{
    namesToo = replay.Names[0] == slot.Player1 && replay.Names[1] == slot.Player2;
    return sharing::DateMatches(replay.Date, slot.Played);
}

int MatchSlot(const Replay& replay)
{
    if (replay.Date.empty())
        return -1;
    int dateOnly = -1, dateMatches = 0;
    for (const sharing::Slot& slot : sharing::Slots()) {
        if (!sharing::DateMatches(replay.Date, slot.Played))
            continue;
        if (replay.Names[0] == slot.Player1 && replay.Names[1] == slot.Player2)
            return slot.Index;
        dateOnly = slot.Index;
        ++dateMatches;
    }
    // The names as the screens show them might not be quite the stored ones; a date to the second is enough on its own.
    return dateMatches == 1 ? dateOnly : -1;
}

bool Visible(void* widget)
{
    static ue::FunctionRef visible { "Widget", "IsVisible" };
    struct {
        bool ReturnValue;
    } params { false };
    return ue::Call(widget, visible, &params) && params.ReturnValue;
}

// A widget half way through going away shouldn't take the game with it.
bool Guarded(void* object, ue::FunctionRef& ref, void* params)
{
    __try {
        return ue::Call(object, ref, params);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallWithId(const Widget& widget, ue::FunctionRef& ref, int id)
{
    if (!widget.Alive())
        return false;
    struct {
        int32_t ID;
    } params { id };
    return Guarded(widget.Object, ref, &params);
}

// UpdateButton on the details box, a Blueprint event, so it's looked up on the box's own class.
bool SetDetailsButton(void* box, int id, const wchar_t* text, bool enabled)
{
    std::wstring copy(text);
    struct {
        int32_t ID;
        int32_t Padding;
        ue::FString Text;
        bool Raw;
        bool Enable;
        uint8_t Rest[6];
    } params {};
    params.ID = id;
    params.Text = ue::FString(copy);
    params.Raw = true;
    params.Enable = enabled;
    return ue::CallOwn(box, "UpdateButton", &params);
}

std::wstring Wide(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// The box's buttons made again through its own ClearButtons and AddButton, the same as the game made them but with Export Replay,
// enabled, in place of button `id`. Renaming a greyed-out button isn't enough: it stays unclickable, and the box takes a click on it
// as Confirm on whichever button is chosen, which starts with Play Replay.
bool RebuildDetailsButtons(void* box, const std::vector<Button>& buttons, int id, int selected)
{
    uint8_t none[16] = {};
    if (!ue::CallOwn(box, "ClearButtons", none))
        return false;
    for (size_t i = 0; i < buttons.size(); ++i) {
        const bool ours = static_cast<int>(i) == id;
        std::wstring text = ours ? std::wstring(L"Export Replay") : Wide(buttons[i].Text);
        struct {
            ue::FString Text;
            bool Raw;
            bool Enable;
            uint8_t Rest[6];
        } params {};
        params.Text = ue::FString(text);
        params.Raw = ours ? true : buttons[i].Raw;
        params.Enable = ours ? true : buttons[i].Enabled;
        if (!ue::CallOwn(box, "AddButton", &params))
            return false;
    }
    // AddButton only takes the game's text keys, so the button is named after it's made.
    SetDetailsButton(box, id, L"Export Replay", true);
    struct {
        int32_t ID;
    } select { selected };
    ue::CallOwn(box, "SelectMenu", &select);
    return true;
}

void ExportFromBox(void* box, int id)
{
    g_exportId = -1; // once per box
    const int slot = g_exportSlot.load();
    std::string name, error;
    if (sharing::ExportNamed(slot, name, error)) {
        SetDetailsButton(box, id, L"Exported", false);
        menu::Flash("Exported to shared_replays: " + name);
        mod::Log("Replay screens: exported slot %d from the game's details box as %s", slot, name.c_str());
    } else {
        SetDetailsButton(box, id, L"Export failed", false);
        menu::Flash("Export failed: " + error);
        mod::Log("Replay screens: export of slot %d failed: %s", slot, error.c_str());
    }
}

// The game's code behind every InvokeDecideCallback of its menus: it calls the box's callback with the button's ID. The Export
// button's ID is taken here, before the game's own code would treat it as the button that used to be there.
void DetourDecide(void* widget, int id)
{
    if (widget && widget == g_exportBox.load(std::memory_order_relaxed) && id == g_exportId.load(std::memory_order_relaxed)) {
        ExportFromBox(widget, id);
        return;
    }
    oDecide(widget, id);
}

// The native function behind the reflected InvokeDecideCallback: its thunk reads the ID and makes one last call, to it. On 3.03.01
// it's at +0x2E7A700 and starts push rbx; sub rsp, 20h; mov rbx, [rcx+...] (the box's callback). Anything else is refused: early on
// the reflected function can still point at the engine's generic code, whose last call is something else entirely.
void* FindDecideCode(void* cls)
{
    void* function = ue::FindFunction(cls, "InvokeDecideCallback");
    if (!function)
        return nullptr;
    const auto* code = ue::Field<const uint8_t*>(function, 0xD8);
    const scan::Module module = scan::MainModule();
    const auto inGame = [&](const uint8_t* at) { return reinterpret_cast<uintptr_t>(at) >= module.Base && reinterpret_cast<uintptr_t>(at) < module.Base + module.Size; };
    if (!inGame(code))
        return nullptr;
    const uint8_t* target = nullptr;
    for (int at = 0, steps = 0; at < 0x100 && steps < 80; ++steps) {
        hde64s instruction {};
        const unsigned length = hde64_disasm(code + at, &instruction);
        if (!length || (instruction.flags & F_ERROR))
            return nullptr;
        if (instruction.opcode == 0xE8)
            target = code + at + length + static_cast<int32_t>(instruction.imm.imm32);
        if (instruction.opcode == 0xC3)
            break;
        at += static_cast<int>(length);
    }
    static const uint8_t start[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99 };
    if (!target || !inGame(target) || !scan::IsReadable(target, sizeof(start)) || memcmp(target, start, sizeof(start)) != 0)
        return nullptr;
    return const_cast<uint8_t*>(target);
}

void InstallDecideHook()
{
    if (g_decideHooked || !g_detailsClass.load())
        return;
    void* target = FindDecideCode(g_detailsClass.load());
    if (!target) {
        // Tried again every couple of seconds; said once.
        static bool said = false;
        if (!said)
            mod::Log("Replay screens: the details box's decide code isn't there yet");
        said = true;
        return;
    }
    g_decideHooked = true;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&DetourDecide), reinterpret_cast<void**>(&oDecide)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        oDecide = nullptr;
        mod::Log("Replay screens: decide hook failed");
        return;
    }
    mod::Log("Replay screens: decide code at +0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(target) - scan::MainModule().Base));
}

bool ResolveClasses()
{
    if (!g_listClass.load())
        g_listClass = ue::FindClass("PolarisUMGReplayList");
    if (!g_detailsClass.load())
        g_detailsClass = ue::FindClass("PolarisUMGReplayDialog");
    if (!g_questionClass.load())
        g_questionClass = ue::FindClass("PolarisUMGDialog");
    return g_listClass.load() && g_detailsClass.load() && g_questionClass.load();
}

void AddExportButton()
{
    void* box = nullptr;
    Replay replay;
    std::vector<Button> buttons;
    int selected = 0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_details.Up || !g_details.Alive())
            return;
        box = g_details.Object;
        replay = g_detailsReplay;
        buttons = g_detailsButtons;
        selected = g_detailsSelected;
    }
    if (!oDecide)
        return;
    const int slot = MatchSlot(replay);
    if (slot < 0)
        return; // not a saved replay, so there's nothing to export
    // A button the game greyed out: the profiles can't be shown offline. Never the first, which plays the replay.
    int id = -1;
    for (int i = static_cast<int>(buttons.size()) - 1; i > 0; --i)
        if (!buttons[static_cast<size_t>(i)].Enabled) {
            id = i;
            break;
        }
    if (id < 0) {
        mod::Log("Replay screens: no greyed-out button in the details box for Export");
        return;
    }
    // Rebuilding fires the box's own events, which forget any Export button, so it's noted after.
    if (!RebuildDetailsButtons(box, buttons, id, selected)) {
        mod::Log("Replay screens: couldn't put Export Replay in the details box");
        return;
    }
    g_exportSlot = slot;
    g_exportId = id;
    g_exportBox = box;
    mod::Log("Replay screens: Export Replay is button %d (was \"%s\") for slot %d", id, buttons[static_cast<size_t>(id)].Text.c_str(), slot);
}

} // namespace

void Install()
{
    ResolveClasses();
    if (!events::Listen(&OnEvent))
        mod::Log("Replay screens: can't follow the game's events");
    InstallDecideHook();
}

void OnGameTick()
{
    static ULONGLONG nextResolve = 0;
    static bool resolved = false;
    if (!g_decideHooked && GetTickCount64() >= nextResolve) {
        nextResolve = GetTickCount64() + 2000;
        if (ResolveClasses()) {
            if (!resolved) {
                resolved = true;
                std::lock_guard lock(g_cacheMutex);
                g_cache.clear(); // anything classified before the classes were known
            }
            InstallDecideHook();
        }
    }

    const int fight = game::FightCount();
    bool decided = false, opened = false;
    Replay played;
    {
        std::lock_guard lock(g_mutex);
        if (fight != g_seenFight) {
            // A fight is on: the menus are gone, whether or not they said so.
            g_seenFight = fight;
            g_list.Up = g_details.Up = g_question.Up = false;
            if (g_playDecided) {
                decided = true;
                played = g_played;
                g_playDecided = false;
            }
        }
        opened = g_detailsOpened;
        g_detailsOpened = false;
    }
    if (decided) {
        const int slot = MatchSlot(played);
        g_openSlot = slot;
        mod::Log("Replay screens: playing slot %d (%s, %s vs %s)", slot, played.Date.c_str(), played.Names[0].c_str(), played.Names[1].c_str());
    }
    if (opened && ExportButtonOn())
        AddExportButton();
}

bool ListUp()
{
    Widget list;
    bool covered = false;
    {
        std::lock_guard lock(g_mutex);
        list = g_list;
        covered = g_listShaded || g_details.Up || g_question.Up;
    }
    return list.Up && !covered && list.Alive() && Visible(list.Object);
}

bool DetailsUp()
{
    Widget details;
    {
        std::lock_guard lock(g_mutex);
        details = g_details;
    }
    return details.Up && details.Alive() && Visible(details.Object);
}

bool QuestionUp()
{
    Widget question;
    {
        std::lock_guard lock(g_mutex);
        question = g_question;
    }
    return question.Up && question.Alive() && Visible(question.Object);
}

int ListTab()
{
    std::lock_guard lock(g_mutex);
    return g_tab;
}

int ListRows()
{
    std::lock_guard lock(g_mutex);
    return static_cast<int>(g_rows.size());
}

int ListFills()
{
    std::lock_guard lock(g_mutex);
    return g_fills;
}

int QuestionsShown()
{
    std::lock_guard lock(g_mutex);
    return g_questions;
}

int RowOfSlot(int index)
{
    sharing::Slot slot;
    if (!sharing::SlotInfo(index, slot))
        return -1;
    std::vector<Replay> rows;
    {
        std::lock_guard lock(g_mutex);
        rows = g_rows;
    }
    int dateOnly = -1, dateMatches = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!sharing::DateMatches(rows[i].Date, slot.Played))
            continue;
        if (rows[i].Names[0] == slot.Player1 && rows[i].Names[1] == slot.Player2)
            return static_cast<int>(i);
        dateOnly = static_cast<int>(i);
        ++dateMatches;
    }
    return dateMatches == 1 ? dateOnly : -1;
}

bool DetailsShow(int index)
{
    sharing::Slot slot;
    if (!sharing::SlotInfo(index, slot))
        return false;
    Replay replay;
    {
        std::lock_guard lock(g_mutex);
        if (!g_details.Up)
            return false;
        replay = g_detailsReplay;
    }
    bool namesToo = false;
    return Shows(replay, slot, namesToo) && (namesToo || MatchSlot(replay) == index);
}

int DetailsSlot()
{
    Replay replay;
    {
        std::lock_guard lock(g_mutex);
        if (!g_details.Up)
            return -1;
        replay = g_detailsReplay;
    }
    return MatchSlot(replay);
}

std::vector<std::string> QuestionButtons()
{
    std::vector<std::string> texts;
    std::lock_guard lock(g_mutex);
    for (const Button& button : g_questionButtons)
        texts.push_back(button.Text);
    return texts;
}

bool OpenTab(int tab)
{
    static ue::FunctionRef ref { "PolarisUMGReplayList", "InvokeTabDecideCallback" };
    Widget list;
    {
        std::lock_guard lock(g_mutex);
        list = g_list;
    }
    return list.Up && CallWithId(list, ref, tab);
}

bool OpenRow(int row)
{
    static ue::FunctionRef ref { "PolarisUMGReplayList", "InvokeButtonDecideCallback" };
    Widget list;
    int id = -1;
    {
        std::lock_guard lock(g_mutex);
        list = g_list;
        if (row >= 0 && row < static_cast<int>(g_rows.size()))
            id = g_rows[static_cast<size_t>(row)].Id;
    }
    return id >= 0 && list.Up && CallWithId(list, ref, id);
}

bool DecideDetails(int id)
{
    static ue::FunctionRef ref { "PolarisUMGReplayDialog", "InvokeDecideCallback" };
    Widget details;
    {
        std::lock_guard lock(g_mutex);
        details = g_details;
    }
    return details.Up && CallWithId(details, ref, id);
}

bool SelectDetails(int id)
{
    static ue::FunctionRef ref { "PolarisUMGReplayDialog", "InvokeSelectCallback" };
    Widget details;
    {
        std::lock_guard lock(g_mutex);
        details = g_details;
    }
    return details.Up && CallWithId(details, ref, id);
}

bool SelectQuestion(int id)
{
    static ue::FunctionRef ref { "PolarisUMGDialog", "InvokeSelectCallback" };
    Widget question;
    {
        std::lock_guard lock(g_mutex);
        question = g_question;
    }
    return question.Up && CallWithId(question, ref, id);
}

int DetailsCursor()
{
    std::lock_guard lock(g_mutex);
    return g_detailsSelected;
}

int QuestionCursor()
{
    std::lock_guard lock(g_mutex);
    return g_questionSelected;
}

bool DecideQuestion(int id)
{
    static ue::FunctionRef ref { "PolarisUMGDialog", "InvokeDecideCallback" };
    Widget question;
    {
        std::lock_guard lock(g_mutex);
        question = g_question;
    }
    return question.Up && CallWithId(question, ref, id);
}

int OpenSlot()
{
    return g_openSlot.load();
}

bool& ExportButtonOn()
{
    static bool on = true;
    return on;
}

} // namespace gameui
