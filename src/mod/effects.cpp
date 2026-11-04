#include "mod/effects.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "mod/dvars.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/recorder.h"
#include "mod/ue.h"

namespace effects {

namespace {

Settings g_settings;
std::atomic<bool> g_refresh { false };
std::atomic<bool> g_dump { false };
std::mutex g_commandMutex;
std::vector<std::wstring> g_commands;

// Tekken's fight HUD is made of UMG widgets. A widget counts as HUD when its class chain has one of these
// words and none of the excluded ones, so menus and dialogs stay. List from the Tekken Overlay rebuild.
const char* const kHudWords[] = { "hud", "gamemessage", "buttonhelp", "controlguide", "guide_button", "ragearts", "matchinfo",
                                  "replayinfo", "replaycontrol", "combochallenge", "counterpractice", "storyiw",
                                  "suppressiongauge", "wbp_ui_practice_" };
const char* const kNotHudWords[] = { "menu", "option", "dialog", "fader", "arrowbutton", "wbp_ui_op_" };

struct HudWidget {
    void* Object;
    uint32_t Index;
    ue::SlateVisibility Original;
};
std::vector<HudWidget> g_hidden;
std::map<void*, bool> g_hudClassCache;
uint32_t g_scanCursor = 0;
bool g_hudApplied = false;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsHudClass(void* cls, void* userWidget)
{
    const auto it = g_hudClassCache.find(cls);
    if (it != g_hudClassCache.end())
        return it->second;
    bool hud = false;
    if (ue::IsChildOf(cls, userWidget)) {
        for (void* c = cls; c && c != userWidget; c = ue::Field<void*>(c, ue::off::SuperStruct)) {
            const std::string name = Lower(ue::NameOf(c));
            for (const char* word : kNotHudWords)
                if (name.find(word) != std::string::npos) {
                    g_hudClassCache[cls] = false;
                    return false;
                }
            for (const char* word : kHudWords)
                hud = hud || name.find(word) != std::string::npos;
            hud = hud || name == "polarisumgpractice";
        }
    }
    g_hudClassCache[cls] = hud;
    return hud;
}

void UpdateHud()
{
    if (!g_settings.HideHud) {
        if (g_hudApplied) {
            for (const HudWidget& widget : g_hidden)
                if (ue::StillAlive(widget.Object, widget.Index))
                    ue::SetWidgetVisibility(widget.Object, widget.Original);
            g_hidden.clear();
            g_hudClassCache.clear();
            g_hudApplied = false;
        }
        return;
    }
    g_hudApplied = true;

    void* userWidget = ue::FindClass("UserWidget");
    const uint32_t count = ue::ObjectCount();
    if (!userWidget || count == 0)
        return;

    // A slice of the object array per tick: widgets appear whenever a new screen opens.
    for (uint32_t n = 0; n < 4096; ++n) {
        if (g_scanCursor >= count)
            g_scanCursor = 0;
        const uint32_t index = g_scanCursor++;
        void* object = ue::ObjectAt(index);
        if (!object || (ue::Field<uint32_t>(object, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject))
            continue;
        void* cls = ue::Field<void*>(object, ue::off::ClassPrivate);
        if (!cls || !IsHudClass(cls, userWidget))
            continue;
        const bool known = std::any_of(g_hidden.begin(), g_hidden.end(),
                                       [&](const HudWidget& w) { return w.Object == object && w.Index == index; });
        if (!known)
            g_hidden.push_back({ object, index, ue::WidgetVisibility(object) });
    }
    std::erase_if(g_hidden, [](const HudWidget& w) { return !ue::StillAlive(w.Object, w.Index); });
    for (const HudWidget& widget : g_hidden)
        if (ue::WidgetVisibility(widget.Object) != ue::SlateVisibility::Hidden)
            ue::SetWidgetVisibility(widget.Object, ue::SlateVisibility::Hidden);
}

void RemoveManagers(const char* className)
{
    void* cls = ue::FindClass(className);
    if (!cls) {
        mod::Log("Class %s not found", className);
        return;
    }
    int removed = 0;
    for (void* actor : ue::AllObjectsOf(cls)) {
        ue::DestroyActor(actor);
        ++removed;
    }
    mod::Log("Removed %d %s", removed, className);
}

// Shipping builds strip the ShowFlag console commands, so these write the viewport's bitfield directly.
// The original bit is remembered when an option turns on and put back when it turns off.
struct ShowFlag {
    bool Settings::*Option; // null when only a recording pass turns this one off
    int WordOffset;
    int Bit;
    bool HiddenWithStage = false;
    bool HiddenWithFighters = false;
    bool Applied = false;
    bool Original = true;
};
ShowFlag g_showFlags[] = {
    { &Settings::NoBloom, 0, 1 },
    { &Settings::NoVignette, 0, 8 },
    { &Settings::NoDepthOfField, 0, 20 },
    { &Settings::NoMotionBlur, 0, 21 },
    { &Settings::NoParticles, 4, 10 },
    { nullptr, 0, 10, true },        // Decals aren't primitives, so hiding components doesn't reach them
    { nullptr, 4, 22, true },        // Atmosphere, likewise
};

// ----- Hiding the stage or the fighters for a recording look. Crowds, holograms and particles are all kinds of mesh, so show
// flags can't pick them apart from the fighters. This goes by who owns each component.
struct HiddenComponent {
    void* Object;
    uint32_t Index;
};
std::vector<HiddenComponent> g_hiddenComponents;
enum class Hide { Nothing, Stage, Fighters };
Hide g_hiding = Hide::Nothing;
int g_hideRescan = 0;

void* ComponentOwner(void* component)
{
    struct {
        void* ReturnValue;
    } params { nullptr };
    static ue::FunctionRef ref { "ActorComponent", "GetOwner" };
    ue::Call(component, ref, &params);
    return params.ReturnValue;
}

void* ParentOf(void* actor)
{
    struct {
        void* ReturnValue;
    } params { nullptr };
    static ue::FunctionRef attached { "Actor", "GetAttachParentActor" };
    ue::Call(actor, attached, &params);
    if (params.ReturnValue)
        return params.ReturnValue;
    static ue::FunctionRef owner { "Actor", "GetOwner" };
    ue::Call(actor, owner, &params);
    return params.ReturnValue;
}

bool BelongsToFighter(void* actor)
{
    for (int depth = 0; actor && depth < 6; ++depth) {
        if (actor == game::Fighter(0) || actor == game::Fighter(1))
            return true;
        actor = ParentOf(actor);
    }
    return false;
}

void SetComponentHidden(void* component, bool hidden)
{
    struct {
        bool Hidden, Propagate;
    } params { hidden, false };
    static ue::FunctionRef ref { "SceneComponent", "SetHiddenInGame" };
    ue::Call(component, ref, &params);
}

void ShowHiddenComponents()
{
    for (const HiddenComponent& hidden : g_hiddenComponents)
        if (ue::StillAlive(hidden.Object, hidden.Index))
            SetComponentHidden(hidden.Object, false);
    g_hiddenComponents.clear();
}

// Hit sparks live on the effect manager and read as part of the fight, so they stay with the fighters.
bool IsFightEffect(void* owner)
{
    static void* cls = nullptr;
    cls = ue::FindClass("PolarisEffectManager");
    return owner && cls && ue::IsA(owner, cls);
}

void HideComponents(Hide what)
{
    void* primitive = ue::FindClass("PrimitiveComponent");
    void* scene = ue::FindClass("SceneComponent");
    if (!primitive || !scene || !game::Fighter(0))
        return;
    static int hiddenByte = -1;
    static uint8_t hiddenMask = 0;
    if (hiddenByte < 0)
        ue::BoolProperty(scene, "bHiddenInGame", hiddenByte, hiddenMask);

    int count = 0;
    for (void* component : ue::AllObjectsOf(primitive)) {
        void* owner = ComponentOwner(component);
        const bool fighterPart = BelongsToFighter(owner) || IsFightEffect(owner);
        if (fighterPart == (what == Hide::Stage))
            continue;
        const uint32_t index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
        // Leave alone anything the game itself had hidden, so showing it again doesn't pop it into view.
        if (hiddenByte >= 0 && (ue::Field<uint8_t>(component, hiddenByte) & hiddenMask))
            continue;
        SetComponentHidden(component, true);
        g_hiddenComponents.push_back({ component, index });
        ++count;
    }
    if (count > 0)
        mod::Log("Look: hid %d components", count);
}

void ApplyHiding(Hide wanted)
{
    if (wanted != g_hiding) {
        ShowHiddenComponents();
        g_hiding = wanted;
        g_hideRescan = 0;
    }
    if (g_hiding == Hide::Nothing)
        return;
    // New effects keep appearing during a fight.
    if (--g_hideRescan <= 0) {
        g_hideRescan = 20;
        HideComponents(g_hiding);
    }
}

// The stage's sky and fog get in the way of a clean black background.
ShowFlag g_fogFlag { nullptr, 4, 18 };

recorder::Pass g_appliedPass = recorder::Pass::Picture;

// Sets up the fog for a recording pass. The fog changes go through engine calls, so only on a change.
void ApplyPassFog(recorder::Pass pass)
{
    if (pass == g_appliedPass)
        return;
    g_appliedPass = pass;
    switch (pass) {
    case recorder::Pass::Greenscreen:
        if (!dvars::SetFogOverride(0, 1, 0, 0.5f, g_settings.GreenClearDistance)) {
            mod::Log("Greenscreen: this stage has no fog object, so pure black pixels get painted green instead");
            recorder::Config().PaintBlackGreen = true;
        }
        break;
    case recorder::Pass::DepthWhiteFog:
        dvars::SetFogOverride(1, 1, 1, g_settings.DepthDensity, 0);
        break;
    case recorder::Pass::DepthBlackFog:
        dvars::SetFogOverride(0, 0, 0, g_settings.DepthDensity, 0);
        break;
    default:
        dvars::ClearFogOverride();
        break;
    }
}

// Every tick, since the game can rebuild the flags when a level loads.
void ApplyFlags()
{
    void* viewport = ue::GameViewport();
    if (!viewport)
        return;
    const recorder::Pass pass = recorder::ActivePass();
    ApplyPassFog(pass);
    const bool hideStage = pass == recorder::Pass::Greenscreen || pass == recorder::Pass::FightersOnBlack;
    const bool hideFighters = pass == recorder::Pass::StageOnly;
    ApplyHiding(hideStage ? Hide::Stage : (hideFighters ? Hide::Fighters : Hide::Nothing));

    const auto apply = [&](ShowFlag& flag, bool off) {
        uint32_t& word = ue::Field<uint32_t>(viewport, ue::off::ViewportShowFlags + flag.WordOffset);
        const uint32_t mask = 1u << flag.Bit;
        if (off) {
            if (!flag.Applied) {
                flag.Original = (word & mask) != 0;
                flag.Applied = true;
            }
            word &= ~mask;
        } else if (flag.Applied) {
            word = flag.Original ? (word | mask) : (word & ~mask);
            flag.Applied = false;
        }
    };
    apply(g_fogFlag, pass == recorder::Pass::FightersOnBlack);
    for (ShowFlag& flag : g_showFlags) {
        const bool wanted = (flag.Option && g_settings.*flag.Option) || (flag.HiddenWithStage && hideStage) || (flag.HiddenWithFighters && hideFighters);
        apply(flag, wanted);
    }
}

void Dump()
{
    std::ofstream file(mod::DataDir() / "dump_polaris.txt", std::ios::trunc);
    int classes = 0;
    ue::ForEachObject([&](void* object) {
        void* cls = ue::Field<void*>(object, ue::off::ClassPrivate);
        if (!cls || !(ue::Field<uint64_t>(cls, ue::off::ClassCastFlags) & ue::off::CASTCLASS_UClass))
            return true;
        const std::string name = ue::NameOf(object);
        const std::string lower = Lower(name);
        if (lower.find("polaris") == std::string::npos && lower.find("wbp_ui_hud") == std::string::npos)
            return true;
        ++classes;
        file << "CLASS " << name;
        for (void* super = ue::Field<void*>(object, ue::off::SuperStruct); super; super = ue::Field<void*>(super, ue::off::SuperStruct))
            file << " : " << ue::NameOf(super);
        file << "\n";
        for (void* field = ue::Field<void*>(object, ue::off::Children); field; field = ue::Field<void*>(field, ue::off::FieldNext))
            if (ue::Field<uint64_t>(ue::Field<void*>(field, ue::off::ClassPrivate), ue::off::ClassCastFlags) & ue::off::CASTCLASS_UFunction)
                file << "    fn " << ue::NameOf(field) << "\n";
        for (const std::string& property : ue::PropertyNames(object))
            file << "    prop " << property << "\n";
        return true;
    });
    mod::Log("Wrote %d classes to dump_polaris.txt", classes);
}

} // namespace

Settings& Config()
{
    return g_settings;
}

void OnFightFound()
{
    if (g_settings.NoHitEffects)
        RemoveManagers("PolarisEffectManager");
    if (g_settings.NoDramaManager)
        RemoveManagers("PolarisDramaManager");
}

void OnGameTick()
{
    if (g_refresh.exchange(false) && game::Fighter(0))
        OnFightFound();
    ApplyFlags();
    {
        std::vector<std::wstring> commands;
        {
            std::lock_guard lock(g_commandMutex);
            commands.swap(g_commands);
        }
        for (const std::wstring& command : commands) {
            mod::Log("Console: %ls", command.c_str());
            ue::ConsoleCommand(command);
        }
    }
    if (g_dump.exchange(false))
        Dump();
    UpdateHud();
}

void RequestRefresh()
{
    g_refresh = true;
}

void RunCommand(const std::string& command)
{
    std::lock_guard lock(g_commandMutex);
    g_commands.emplace_back(command.begin(), command.end());
}

void RequestDump()
{
    g_dump = true;
}

} // namespace effects
