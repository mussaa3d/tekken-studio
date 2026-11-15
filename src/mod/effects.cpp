#include "mod/effects.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mod/actors.h"
#include "mod/dvars.h"
#include "mod/game.h"
#include "mod/log.h"
#include "mod/recorder.h"
#include "mod/replaydisplay.h"
#include "mod/spawner.h"
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
                                  "suppressiongauge", "wbp_ui_practice_", "fpsdisplay" };
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

std::vector<std::string> SplitWords(const std::string& text)
{
    std::vector<std::string> words;
    std::string word;
    for (const char c : text + ",") {
        if (c == ',') {
            if (!word.empty())
                words.push_back(Lower(word));
            word.clear();
        } else if (c != ' ') {
            word += c;
        }
    }
    return words;
}

std::vector<std::string> g_keepHud;

bool IsHudClass(void* cls, void* userWidget, bool keepStatus)
{
    const auto it = g_hudClassCache.find(cls);
    if (it != g_hudClassCache.end())
        return it->second;
    bool hud = false;
    if (ue::IsChildOf(cls, userWidget)) {
        // The in-battle status sits in these two, inside the practice widget that holds the other panels. Those panels are widgets of their own, so they still go.
        const std::string own = Lower(ue::NameOf(cls));
        // What a HUD preset keeps. Whole names only: the health bar's widget holds the names and portraits as widgets of their own, which still go.
        if (std::find(g_keepHud.begin(), g_keepHud.end(), own) != g_keepHud.end()) {
            g_hudClassCache[cls] = false;
            return false;
        }
        if (keepStatus && (own == "wbp_ui_practice_s2_c" || own == "wbp_ui_practice_info_l_s2_c" || own == "wbp_ui_practice_info_r_s2_c")) {
            g_hudClassCache[cls] = false;
            return false;
        }
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

// The hit labels: widgets whose class name has one of the words the settings list.
bool IsLabelClass(void* cls, const std::vector<std::string>& words)
{
    const std::string name = Lower(ue::NameOf(cls));
    for (const std::string& word : words)
        if (!word.empty() && name.find(word) != std::string::npos)
            return true;
    return false;
}

std::vector<std::string> LabelWords()
{
    std::vector<std::string> words;
    std::string word;
    for (const char c : std::string(g_settings.HitLabelWidgets) + ",") {
        if (c == ',') {
            if (!word.empty())
                words.push_back(Lower(word));
            word.clear();
        } else if (c != ' ') {
            word += c;
        }
    }
    return words;
}

void UpdateHud()
{
    // 3 hides the HUD but leaves the in-battle status, 2 hides all of it, 1 only the hit labels.
    const int mode = g_settings.HideHud ? (g_settings.KeepBattleStatus ? 3 : 2) : (g_settings.HideHitLabels ? 1 : 0);
    static int shownMode = 0;
    static std::string shownWords;
    const std::string words = mode == 1 ? std::string(g_settings.HitLabelWidgets) : std::string(g_settings.KeepHud);
    if (mode != shownMode || words != shownWords) {
        g_keepHud = mode >= 2 ? SplitWords(g_settings.KeepHud) : std::vector<std::string>();
        if (g_hudApplied) {
            for (const HudWidget& widget : g_hidden)
                if (ue::StillAlive(widget.Object, widget.Index))
                    ue::SetWidgetVisibility(widget.Object, widget.Original);
            g_hidden.clear();
            g_hudClassCache.clear();
            g_hudApplied = false;
        }
        shownMode = mode;
        shownWords = words;
    }
    if (mode == 0)
        return;
    g_hudApplied = true;
    const std::vector<std::string> labelWords = mode == 1 ? LabelWords() : std::vector<std::string>();

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
        if (!object || !ue::ClassChainIsSound(object) || (ue::Field<uint32_t>(object, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject))
            continue;
        void* cls = ue::Field<void*>(object, ue::off::ClassPrivate);
        if (!cls || !(mode >= 2 ? IsHudClass(cls, userWidget, mode == 3) : (ue::IsChildOf(cls, userWidget) && IsLabelClass(cls, labelWords))))
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

// The drama manager runs rage arts and heat smashes. Destroying it or the effect manager crashes the game the next time it calls them
// (a rewind does, for one), so they stay, and what they show gets switched off instead.
void* g_dramaManager = nullptr;
uint32_t g_dramaManagerIndex = 0;
bool g_dramaPlaying = false;
int g_dramaLookup = 0;

void* DramaManager()
{
    if (g_dramaManager && ue::StillAlive(g_dramaManager, g_dramaManagerIndex))
        return g_dramaManager;
    g_dramaManager = nullptr;
    if (--g_dramaLookup > 0 || !game::Fighter(0))
        return nullptr;
    g_dramaLookup = 120;
    if (void* cls = ue::FindClass("PolarisDramaManager")) {
        const auto objects = ue::AllObjectsOf(cls);
        if (!objects.empty()) {
            g_dramaManager = objects.front();
            g_dramaManagerIndex = ue::Field<uint32_t>(g_dramaManager, ue::off::InternalIndex);
        }
    }
    return g_dramaManager;
}

void UpdateDrama()
{
    void* manager = DramaManager();
    bool playing = false;
    if (manager) {
        struct {
            void* ReturnValue;
        } current { nullptr };
        static ue::FunctionRef getCurrent { "PolarisDramaManager", "GetCurrentDramaActor" };
        ue::Call(manager, getCurrent, &current);
        // The drama actor stays around between cinematics, so what counts is whether its sequence is playing.
        static const int playerAt = [] {
            void* cls = ue::FindClass("PolarisDramaActor");
            return cls ? ue::PropertyOffset(cls, "LevelSequencePlayer") : -1;
        }();
        void* player = current.ReturnValue && playerAt >= 0 ? ue::Field<void*>(current.ReturnValue, playerAt) : nullptr;
        if (player) {
            struct {
                bool ReturnValue;
            } isPlaying { false };
            static ue::FunctionRef playingRef { "MovieSceneSequencePlayer", "IsPlaying" };
            ue::Call(player, playingRef, &isPlaying);
            playing = isPlaying.ReturnValue;
        }
    }
    if (playing != g_dramaPlaying) {
        mod::Log("Drama %s at %.2f", playing ? "starts" : "ends", game::Clock());
        g_dramaPlaying = playing;
    }
    if (playing && g_settings.NoDramaManager) {
        static ue::FunctionRef reset { "PolarisDramaManager", "ResetDramaParameterPostProc" };
        ue::Call(manager, reset, nullptr);
    }
}

// Hit sparks and rage art glows, hidden as they appear.
struct HiddenEffect {
    void* Object;
    uint32_t Index;
};
std::vector<HiddenEffect> g_hiddenEffects;
int g_effectRescan = 0;

void SetEffectHidden(void* component, bool hidden)
{
    struct {
        bool Hidden, Propagate;
    } params { hidden, false };
    static ue::FunctionRef ref { "SceneComponent", "SetHiddenInGame" };
    ue::Call(component, ref, &params);
}

void UpdateHitEffects()
{
    if (!g_settings.NoHitEffects) {
        if (!g_hiddenEffects.empty()) {
            for (const HiddenEffect& effect : g_hiddenEffects)
                if (ue::StillAlive(effect.Object, effect.Index))
                    SetEffectHidden(effect.Object, false);
            g_hiddenEffects.clear();
        }
        return;
    }
    if (--g_effectRescan > 0)
        return;
    g_effectRescan = 3;
    void* cls = ue::FindClass("NiagaraComponent");
    if (!cls)
        return;
    static ue::FunctionRef getOwner { "ActorComponent", "GetOwner" };
    static void* lastLogged = nullptr;
    for (void* component : ue::AllObjectsOf(cls)) {
        struct {
            void* ReturnValue;
        } owner { nullptr };
        ue::Call(component, getOwner, &owner);
        if (!owner.ReturnValue)
            continue;
        const std::string ownerClass = Lower(ue::ClassNameOf(owner.ReturnValue));
        const bool fightEffect = ownerClass.find("effect") != std::string::npos || ownerClass.find("niagara") != std::string::npos || ownerClass.find("character") != std::string::npos;
        if (!fightEffect || ownerClass.find("stage") != std::string::npos)
            continue;
        const uint32_t index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
        const bool known = std::any_of(g_hiddenEffects.begin(), g_hiddenEffects.end(), [&](const HiddenEffect& e) { return e.Object == component && e.Index == index; });
        if (known)
            continue;
        SetEffectHidden(component, true);
        g_hiddenEffects.push_back({ component, index });
        if (owner.ReturnValue != lastLogged) {
            lastLogged = owner.ReturnValue;
            mod::Log("Effects: hiding what %s shows", ownerClass.c_str());
        }
    }
    std::erase_if(g_hiddenEffects, [](const HiddenEffect& e) { return !ue::StillAlive(e.Object, e.Index); });
}

// The map's mist is particles, which no fog setting reaches. So with a fog colour chosen, the stage's own particles go.
std::vector<HiddenEffect> g_hiddenMist;
int g_mistRescan = 0;

void UpdateMapMist()
{
    if (!g_settings.FogHidesMist || !dvars::Tint().FogOverridden) {
        for (const HiddenEffect& effect : g_hiddenMist)
            if (ue::StillAlive(effect.Object, effect.Index))
                SetEffectHidden(effect.Object, false);
        g_hiddenMist.clear();
        g_mistRescan = 0;
        return;
    }
    if (--g_mistRescan > 0)
        return;
    g_mistRescan = 30;
    void* cls = ue::FindClass("NiagaraComponent");
    if (!cls)
        return;
    static ue::FunctionRef getOwner { "ActorComponent", "GetOwner" };
    for (void* component : ue::AllObjectsOf(cls)) {
        struct {
            void* ReturnValue;
        } owner { nullptr };
        ue::Call(component, getOwner, &owner);
        if (!owner.ReturnValue)
            continue;
        const std::string ownerClass = Lower(ue::ClassNameOf(owner.ReturnValue));
        // Fight effects, UI and breaking walls are part of what's happening, and those particles stay.
        if (ownerClass != "niagaraactor" && ownerClass != "polarisniagaraactor")
            continue;
        const uint32_t index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
        if (std::any_of(g_hiddenMist.begin(), g_hiddenMist.end(), [&](const HiddenEffect& e) { return e.Object == component && e.Index == index; }))
            continue;
        SetEffectHidden(component, true);
        g_hiddenMist.push_back({ component, index });
    }
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
std::unordered_set<void*> g_hiddenSet; // the same components, to find one quickly
enum class Hide { Nothing, Stage, Fighters, AllButEffects };
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
    // An actor you placed is a fighter as far as the looks go: it stays with them on the greenscreen, and goes with them from the stage.
    if (actors::IsActorPart(actor))
        return true;
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
    // Some fighter parts (Roger's sign, a hat that's been knocked off) are shown again by the game every frame. Drawn only in scene
    // captures, they stay out of the picture whatever the game does with their hidden flag.
    static void* primitive = ue::FindClass("PrimitiveComponent");
    if (primitive && ue::IsA(component, primitive)) {
        struct {
            bool Value;
        } capture { hidden };
        static ue::FunctionRef captureOnly { "PrimitiveComponent", "SetVisibleInSceneCaptureOnly" };
        ue::Call(component, captureOnly, &capture);
    }
}

void ShowHiddenComponents()
{
    for (const HiddenComponent& hidden : g_hiddenComponents)
        if (ue::StillAlive(hidden.Object, hidden.Index))
            SetComponentHidden(hidden.Object, false);
    g_hiddenComponents.clear();
    g_hiddenSet.clear();
}

// Hit sparks live on the effect manager and read as part of the fight, so they stay with the fighters.
bool IsFightEffect(void* owner)
{
    static void* cls = nullptr;
    cls = ue::FindClass("PolarisEffectManager");
    return owner && cls && ue::IsA(owner, cls);
}

// Sparks, trails, auras and the glow of heat and rage arts: particle systems the fight owns, not the stage's own.
// Measured on a hit: the sparks are pooled systems with no owner, on the world settings (Unreal's pool), or on PolarisNiagaraActor. The stage's clouds and mist are on plain
// NiagaraActor and its breakable walls on their own blueprints, so those go. Only particle systems count: Bryan's weapon is a mesh the
// effect manager holds on to.
bool IsFightEffectComponent(void* component, void* owner)
{
    static void* niagara = ue::FindClass("NiagaraComponent");
    static void* cascade = ue::FindClass("ParticleSystemComponent");
    if (!(niagara && ue::IsA(component, niagara)) && !(cascade && ue::IsA(component, cascade)))
        return false;
    if (!owner || IsFightEffect(owner) || spawner::IsPlacedEffect(component))
        return true;
    const std::string ownerClass = Lower(ue::ClassNameOf(owner));
    if (ownerClass.find("stage") != std::string::npos || ownerClass.find("niagarauiactor") != std::string::npos)
        return false;
    // Unreal's pool of reused particle systems belongs to the world settings: hits and impacts come from there. The stage's own
    // fires and mist are placed NiagaraActors.
    if (ownerClass == "worldsettings" || ownerClass.find("worldsettings") != std::string::npos)
        return true;
    return BelongsToFighter(owner) || ownerClass.find("polarisniagara") != std::string::npos || ownerClass.find("effect") != std::string::npos
        || ownerClass.find("character") != std::string::npos;
}

// The asset a mesh component draws, or "" for anything else.
std::string MeshNameOf(void* component)
{
    void* cls = ue::Field<void*>(component, ue::off::ClassPrivate);
    if (!cls)
        return "";
    // Where each class keeps its mesh, looked up once per class.
    static std::unordered_map<void*, int> offsets;
    auto it = offsets.find(cls);
    if (it == offsets.end()) {
        int found = -1;
        for (const char* property : { "SkeletalMesh", "SkinnedAsset", "StaticMesh" })
            if (found < 0)
                found = ue::PropertyOffset(cls, property);
        it = offsets.emplace(cls, found).first;
    }
    if (it->second < 0)
        return "";
    void* mesh = ue::Field<void*>(component, it->second);
    return mesh ? ue::NameOf(mesh) : "";
}

// Everything that's part of the fight and not of the stage: the fighters and what they own, the hit effects, the afterimages a fast
// move leaves (PolarisTraceMeshActor), and character props the game spawns on their own, such as Roger's bucket and punching bag
// (SkeletalMeshActor with an SK_CH_ mesh) and the scenery a rage art brings in (SKM_<character>Rage...).
struct SideCache {
    uint32_t Index;
    bool FightSide;
    bool Effect; // a fight particle effect, which the effects look keeps
};
// Each component's side, worked out once: with several streams the look changes several times a frame, and working it out again
// for thousands of components each time made recording crawl. Cleared when a new fight starts.
std::unordered_map<void*, SideCache> g_sideCache;

bool WorkOutFightSide(void* component, void* owner)
{
    if (BelongsToFighter(owner) || IsFightEffect(owner) || IsFightEffectComponent(component, owner))
        return true;
    if (owner && ue::ClassNameOf(owner) == "PolarisTraceMeshActor")
        return true;
    const std::string mesh = MeshNameOf(component);
    return mesh.rfind("SK_CH_", 0) == 0 || mesh.rfind("SM_CH_", 0) == 0 || (mesh.rfind("SKM_", 0) == 0 && mesh.find("Rage") != std::string::npos);
}

const SideCache& SideOf(void* component)
{
    const uint32_t index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
    const auto it = g_sideCache.find(component);
    if (it != g_sideCache.end() && it->second.Index == index)
        return it->second;
    void* owner = ComponentOwner(component);
    return g_sideCache[component] = { index, WorkOutFightSide(component, owner), IsFightEffectComponent(component, owner) };
}

bool IsFightSide(void* component, void*)
{
    return SideOf(component).FightSide;
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
        const SideCache& side = SideOf(component);
        if (what == Hide::AllButEffects ? side.Effect : side.FightSide == (what == Hide::Stage))
            continue;
        const uint32_t index = ue::Field<uint32_t>(component, ue::off::InternalIndex);
        // Leave alone anything the game itself had hidden, so showing it again doesn't pop it into view.
        if (hiddenByte >= 0 && (ue::Field<uint8_t>(component, hiddenByte) & hiddenMask))
            continue;
        SetComponentHidden(component, true);
        // The game shows some parts again every frame, so the same one can come round twice: it's put back once.
        if (g_hiddenSet.insert(component).second) {
            g_hiddenComponents.push_back({ component, index });
            ++count;
        }
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

// Every tick, since the game can rebuild the flags when a level loads.
void ApplyFlags()
{
    void* viewport = ue::GameViewport();
    if (!viewport)
        return;
    const recorder::Pass pass = recorder::ActivePass();
    const bool effectsOnly = pass == recorder::Pass::EffectsOnBlack;
    const bool hideStage = pass == recorder::Pass::Greenscreen || pass == recorder::Pass::FightersOnBlack || effectsOnly;
    const bool hideFighters = pass == recorder::Pass::StageOnly;
    ApplyHiding(effectsOnly ? Hide::AllButEffects : (hideStage ? Hide::Stage : (hideFighters ? Hide::Fighters : Hide::Nothing)));

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
    apply(g_fogFlag, hideStage);
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
    g_sideCache.clear();
    g_dramaManager = nullptr;
    g_dramaLookup = 0;
    g_dramaPlaying = false;
    g_hiddenEffects.clear();
}

bool DramaPlaying()
{
    return g_dramaPlaying;
}

void OnGameTick()
{
    g_refresh.exchange(false);
    UpdateDrama();
    UpdateHitEffects();
    UpdateMapMist();
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

void* OwnerOf(void* component)
{
    return ComponentOwner(component);
}

void LogVisibleOwners()
{
    void* primitive = ue::FindClass("PrimitiveComponent");
    void* scene = ue::FindClass("SceneComponent");
    if (!primitive || !scene)
        return;
    int hiddenByte = -1;
    uint8_t hiddenMask = 0;
    ue::BoolProperty(scene, "bHiddenInGame", hiddenByte, hiddenMask);
    std::map<std::string, int> groups;
    for (void* component : ue::AllObjectsOf(primitive)) {
        if (ue::Field<uint32_t>(component, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        if (hiddenByte >= 0 && (ue::Field<uint8_t>(component, hiddenByte) & hiddenMask))
            continue;
        void* owner = ComponentOwner(component);
        void* parent = owner ? ParentOf(owner) : nullptr;
        const std::string key = ue::ClassNameOf(component) + " | " + (owner ? ue::ClassNameOf(owner) + " " + ue::NameOf(owner) : std::string("no owner")) + " | parent "
            + (parent ? ue::ClassNameOf(parent) : std::string("-")) + " | mesh " + MeshNameOf(component) + (IsFightSide(component, owner) ? " | FIGHT" : "");
        ++groups[key];
    }
    for (const auto& [key, count] : groups)
        mod::Log("[owners] %d x %s", count, key.c_str());
    mod::Log("[owners] end");
}

bool IsFighterPart(void* owner)
{
    return BelongsToFighter(owner) || IsFightEffect(owner);
}

void LogVisibleWidgets()
{
    void* userWidget = ue::FindClass("UserWidget");
    if (!userWidget)
        return;
    static ue::FunctionRef visible { "Widget", "IsVisible" };
    std::map<std::string, int> found;
    for (void* widget : ue::AllObjectsOf(userWidget)) {
        if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        struct {
            bool ReturnValue;
        } params { false };
        ue::Call(widget, visible, &params);
        if (params.ReturnValue)
            ++found[ue::ClassNameOf(widget)];
    }
    for (const auto& [name, count] : found)
        mod::Log("[hud] %d x %s", count, name.c_str());
    mod::Log("[hud] end");
}

namespace {
int g_hudPreset = 0;
}

const char* HudPresetName(int preset)
{
    static const char* const names[HudPresetCount] = { "The game's own", "Fragmovie: health, timer and damage", "Health bars and timer",
                                                       "Broadcast: the match HUD, no replay panels", "Combo text only", "Clean: no HUD" };
    return preset >= 0 && preset < HudPresetCount ? names[preset] : "Custom";
}

void ApplyHudPreset(int preset)
{
    if (preset < 0 || preset >= HudPresetCount)
        return;
    // The match HUD (WBP_UI_HUD_S2) holds the health bars (HUD_Gauge_S2), which hold the names, portraits and badges (HUD_Player_S2).
    // Each list names the parents of what it keeps too; their other children are widgets of their own and still go.
    const char* const bars = "wbp_ui_hud_s2_c,wbp_ui_hud_gauge_s2_c,wbp_ui_hud_timer_c,wbp_ui_hud_timer_number_c";
    const char* const match = "wbp_ui_hud_s2_c,wbp_ui_hud_gauge_s2_c,wbp_ui_hud_timer_c,wbp_ui_hud_timer_number_c,wbp_ui_hud_player_s2_c,"
                              "wbp_ui_hud_gaugecustom_c,wbp_ui_hud_roundcounter_c,wbp_ui_hud_wins_number_c,wbp_ui_hud_char_icon_c,"
                              "wbp_ui_hud_zone_s2_c,wbp_ui_hud_information_c,wbp_ui_hud_prostration_c";
    struct Preset {
        bool HideHud, KeepStatus;
        const char* Keep;
        bool Status[6]; // hit combo, damage, counter, clean, punish, tornado
    };
    const Preset presets[HudPresetCount] = {
        { false, false, "", { true, true, true, true, true, true } },
        { true, true, bars, { true, true, false, false, false, false } },
        { true, false, bars, { true, true, true, true, true, true } },
        { true, false, match, { true, true, true, true, true, true } },
        { true, true, "", { true, true, true, true, true, true } },
        { true, false, "", { true, true, true, true, true, true } },
    };
    const Preset& p = presets[preset];
    g_settings.HideHud = p.HideHud;
    g_settings.KeepBattleStatus = p.KeepStatus;
    g_settings.HideHitLabels = false;
    strncpy_s(g_settings.KeepHud, p.Keep, _TRUNCATE);
    for (int item = 0; item < replaydisplay::StatusItems && item < 6; ++item)
        replaydisplay::StatusShown(item) = p.Status[item];
    replaydisplay::Changed(); // finds the status text again, so the hidden items go now and not at the next fight
    g_hudPreset = preset;
}

int& HudPreset()
{
    return g_hudPreset;
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
