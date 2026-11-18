#include "mod/hudframe.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <string>

#include "mod/log.h"
#include "mod/ue.h"

namespace hudframe {

namespace {

template <typename T> struct TArray {
    T* Data;
    int32_t Num;
    int32_t Max;
};

int Offset(void* object, const char* property)
{
    void* cls = object ? ue::Field<void*>(object, ue::off::ClassPrivate) : nullptr;
    return cls ? ue::PropertyOffset(cls, property) : -1;
}

void DumpWidget(void* widget, int depth)
{
    if (!widget || depth > 9)
        return;
    std::string line(depth * 2, ' ');
    line += ue::ClassNameOf(widget) + " " + ue::NameOf(widget);
    const std::string cls = ue::ClassNameOf(widget);
    char extra[256] = "";
    if (cls == "SizeBox") {
        const int w = Offset(widget, "WidthOverride"), h = Offset(widget, "HeightOverride");
        if (w >= 0 && h >= 0)
            snprintf(extra, sizeof(extra), "  width %g height %g", ue::Field<float>(widget, w), ue::Field<float>(widget, h));
    } else if (cls == "ScaleBox") {
        const int s = Offset(widget, "Stretch"), u = Offset(widget, "UserSpecifiedScale");
        if (s >= 0)
            snprintf(extra, sizeof(extra), "  stretch %d scale %g", ue::Field<uint8_t>(widget, s), u >= 0 ? ue::Field<float>(widget, u) : -1.0f);
    }
    line += extra;
    // Where it sits in its parent.
    const int slotAt = Offset(widget, "Slot");
    void* slot = slotAt >= 0 ? ue::Field<void*>(widget, slotAt) : nullptr;
    if (slot) {
        line += "  in " + ue::ClassNameOf(slot);
        if (ue::ClassNameOf(slot) == "CanvasPanelSlot") {
            const int layout = Offset(slot, "LayoutData");
            if (layout >= 0) {
                const float* margin = &ue::Field<float>(slot, layout);
                const double* anchors = reinterpret_cast<const double*>(margin + 4);
                char text[200];
                snprintf(text, sizeof(text), " offsets %g %g %g %g anchors %g %g %g %g align %g %g", margin[0], margin[1], margin[2], margin[3], anchors[0], anchors[1],
                         anchors[2], anchors[3], anchors[4], anchors[5]);
                line += text;
            }
        }
    }
    mod::Log("[hudtree] %s", line.c_str());

    // A user widget's own tree, a panel's children, a content widget's one child.
    const int treeAt = Offset(widget, "WidgetTree");
    if (treeAt >= 0) {
        void* tree = ue::Field<void*>(widget, treeAt);
        const int rootAt = Offset(tree, "RootWidget");
        if (tree && rootAt >= 0)
            DumpWidget(ue::Field<void*>(tree, rootAt), depth + 1);
        return;
    }
    const int slotsAt = Offset(widget, "Slots");
    if (slotsAt < 0)
        return;
    const auto& slots = ue::Field<TArray<void*>>(widget, slotsAt);
    for (int i = 0; i < slots.Num && i < 64; ++i) {
        void* child = slots.Data[i];
        const int contentAt = Offset(child, "Content");
        if (child && contentAt >= 0)
            DumpWidget(ue::Field<void*>(child, contentAt), depth + 1);
    }
}

} // namespace

// Where a widget sits: its parents up to the top, then the screen that holds that tree, and on up.
void Chain(void* widget)
{
    std::string line;
    for (int guard = 0; widget && guard < 40; ++guard) {
        line += ue::ClassNameOf(widget) + "(" + ue::NameOf(widget) + ") < ";
        const int slotAt = Offset(widget, "Slot");
        void* slot = slotAt >= 0 ? ue::Field<void*>(widget, slotAt) : nullptr;
        const int parentAt = Offset(slot, "Parent");
        void* parent = slot && parentAt >= 0 ? ue::Field<void*>(slot, parentAt) : nullptr;
        if (!parent) {
            // The top of a tree: its outer is the WidgetTree, whose outer is the user widget.
            void* outer = ue::Field<void*>(widget, 0x20);
            if (outer && ue::ClassNameOf(outer) == "WidgetTree")
                outer = ue::Field<void*>(outer, 0x20);
            if (outer == widget || !outer || ue::ClassNameOf(outer) == "Package" || ue::ClassNameOf(outer).find("GameInstance") != std::string::npos) {
                if (outer)
                    line += "[" + ue::ClassNameOf(outer) + "]";
                break;
            }
            parent = outer;
        }
        widget = parent;
    }
    mod::Log("[hudtree] chain %s", line.c_str());
}

void Dump(const std::string& words)
{
    if (words.rfind("chain ", 0) == 0) {
        void* userWidget = ue::FindClass("UserWidget");
        static ue::FunctionRef visibleRef { "Widget", "IsVisible" };
        for (void* widget : ue::AllObjectsOf(userWidget)) {
            if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
                continue;
            if (ue::ClassNameOf(widget).find(words.substr(6)) == std::string::npos)
                continue;
            struct {
                bool ReturnValue;
            } shown { false };
            ue::Call(widget, visibleRef, &shown);
            if (shown.ReturnValue)
                Chain(widget);
        }
        mod::Log("[hudtree] end");
        return;
    }
    void* userWidget = ue::FindClass("UserWidget");
    if (!userWidget)
        return;
    static ue::FunctionRef inViewport { "UserWidget", "IsInViewport" };
    static ue::FunctionRef visible { "Widget", "IsVisible" };
    static ue::FunctionRef geometry { "Widget", "GetCachedGeometry" };
    for (void* widget : ue::AllObjectsOf(userWidget)) {
        if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        struct {
            bool ReturnValue;
        } shown { false }, top { false };
        ue::Call(widget, visible, &shown);
        ue::Call(widget, inViewport, &top);
        if (!shown.ReturnValue || (words.empty() && !top.ReturnValue))
            continue;
        const std::string cls = ue::ClassNameOf(widget);
        if (!words.empty() && cls.find(words) == std::string::npos)
            continue;
        alignas(16) uint8_t params[256] = {};
        ue::Call(widget, geometry, params);
        const float* f = reinterpret_cast<const float*>(params);
        mod::Log("[hudtree] == %s in the viewport, geometry %g %g %g %g %g %g %g %g", cls.c_str(), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        DumpWidget(widget, 1);
    }
    mod::Log("[hudtree] end");
}

void Anchor(float minX, float minY, float maxX, float maxY)
{
    void* userWidget = ue::FindClass("UserWidget");
    if (!userWidget)
        return;
    static ue::FunctionRef inViewport { "UserWidget", "IsInViewport" };
    static ue::FunctionRef setAnchors { "UserWidget", "SetAnchorsInViewport" };
    int count = 0;
    for (void* widget : ue::AllObjectsOf(userWidget)) {
        if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        struct {
            bool ReturnValue;
        } top { false };
        ue::Call(widget, inViewport, &top);
        if (!top.ReturnValue)
            continue;
        struct {
            double MinX, MinY, MaxX, MaxY;
        } anchors { minX, minY, maxX, maxY };
        ue::Call(widget, setAnchors, &anchors);
        ++count;
    }
    mod::Log("[hudtree] anchored %d widgets to %g %g %g %g", count, minX, minY, maxX, maxY);
}

namespace {

constexpr double DesignWidth = 3840, DesignHeight = 2160;

struct Vec2 {
    double X, Y;
};

// One of the 3840 x 2160 boxes in a HUD screen's design, as the game made it.
struct Box {
    void* Slot = nullptr;
    void* Owner = nullptr; // the screen it's in, to tell when it's gone
    uint32_t OwnerIndex = 0;
    Vec2 Position {}, Size {};
    bool Shift = false; // a small part placed from the design's centre, which moves with the nearest edge instead of growing
};

struct Placed {
    void* Widget = nullptr;
    uint32_t Index = 0;
};

// A screen the game made without adding it to the window the usual way (the button bar at the bottom, the information bar). Its
// layout doesn't take new anchors, so it's drawn smaller instead, with a render transform about the window's centre.
struct Scaled {
    void* Widget = nullptr;
    uint32_t Index = 0;
    bool Bar = false;     // a bar along the bottom, which goes to the bottom of the picture when the HUD takes the shape
    float UiScale = 0.5f; // its design units to pixels
};

std::vector<Box> g_boxes;
std::vector<Placed> g_placed;
std::vector<Scaled> g_scaled;
Mode g_mode = Mode::Game;
double g_anchors[4] = { 0, 0, 1, 1 };
Vec2 g_design { DesignWidth, DesignHeight };
int g_tick = 0;
void* g_gameInstance = nullptr; // the outer of the screens the game makes, learned from one on screen

ue::FunctionRef g_inViewport { "UserWidget", "IsInViewport" };
ue::FunctionRef g_setAnchors { "UserWidget", "SetAnchorsInViewport" };
ue::FunctionRef g_setSize { "CanvasPanelSlot", "SetSize" };
ue::FunctionRef g_setPosition { "CanvasPanelSlot", "SetPosition" };
ue::FunctionRef g_setRenderScale { "Widget", "SetRenderScale" };
ue::FunctionRef g_setRenderTranslation { "Widget", "SetRenderTranslation" };
ue::FunctionRef g_cachedGeometry { "Widget", "GetCachedGeometry" };
int g_windowWidth = 0, g_windowHeight = 0, g_pictureWidth = 0, g_pictureHeight = 0; // in pixels

void SetTransform(void* widget, double scale, double translateY)
{
    struct {
        double X, Y;
    } size { scale, scale }, move { 0, translateY };
    ue::Call(widget, g_setRenderScale, &size);
    ue::Call(widget, g_setRenderTranslation, &move);
}

// The whole screen drawn at the picture's size, centred on it; a bar along the bottom goes to the bottom of the picture when the HUD
// takes the shape.
void Fit(const Scaled& scaled)
{
    if (g_windowWidth <= 0 || g_windowHeight <= 0)
        return;
    const double k = std::min(static_cast<double>(g_pictureWidth) / g_windowWidth, static_cast<double>(g_pictureHeight) / g_windowHeight);
    double down = 0;
    if (scaled.Bar && g_mode == Mode::Shape)
        down = (g_pictureHeight - g_windowHeight * k) / 2 / std::max(scaled.UiScale, 0.05f); // pixels to the screen's own units
    SetTransform(scaled.Widget, k, down);
}

void SetAnchors(void* widget, const double anchors[4])
{
    struct {
        double MinX, MinY, MaxX, MaxY;
    } params { anchors[0], anchors[1], anchors[2], anchors[3] };
    ue::Call(widget, g_setAnchors, &params);
}

void Place(void* slot, Vec2 position, Vec2 size)
{
    Vec2 p = position, s = size;
    ue::Call(slot, g_setSize, &s);
    ue::Call(slot, g_setPosition, &p);
}

// The box at the design's shape, still centred where the game's was.
void Reshape(const Box& box)
{
    if (box.Shift) {
        // A part in the outer third of the design keeps its distance from that edge.
        const double dx = (g_design.X - DesignWidth) / 2, dy = (g_design.Y - DesignHeight) / 2;
        Vec2 position = box.Position;
        position.X += box.Position.X < -DesignWidth / 6 ? -dx : box.Position.X > DesignWidth / 6 ? dx : 0;
        position.Y += box.Position.Y < -DesignHeight / 6 ? -dy : box.Position.Y > DesignHeight / 6 ? dy : 0;
        Place(box.Slot, position, box.Size);
        return;
    }
    Vec2 position = box.Position;
    if (position.X == -DesignWidth / 2 && position.Y == -DesignHeight / 2)
        position = { -g_design.X / 2, -g_design.Y / 2 }; // placed by its corner (the replay and practice controls)
    Place(box.Slot, position, g_design);
}

bool Alive(void* object, uint32_t index)
{
    return ue::StillAlive(object, index);
}

void* Child(void* slot)
{
    const int at = Offset(slot, "Content");
    return slot && at >= 0 ? ue::Field<void*>(slot, at) : nullptr;
}

const TArray<void*>* SlotsOf(void* panel)
{
    const int at = Offset(panel, "Slots");
    return panel && at >= 0 ? &ue::Field<TArray<void*>>(panel, at) : nullptr;
}

// The 3840 x 2160 boxes of one screen: in its root canvas, a ScaleBox that scales to fit, holding a canvas whose children are the
// boxes.
void FindBoxes(void* screen, uint32_t index)
{
    const int treeAt = Offset(screen, "WidgetTree");
    void* tree = treeAt >= 0 ? ue::Field<void*>(screen, treeAt) : nullptr;
    const int rootAt = Offset(tree, "RootWidget");
    void* root = tree && rootAt >= 0 ? ue::Field<void*>(tree, rootAt) : nullptr;
    const TArray<void*>* top = SlotsOf(root);
    if (!top)
        return;
    for (int i = 0; i < top->Num && i < 64; ++i) {
        void* scaleBox = Child(top->Data[i]);
        if (!scaleBox || ue::ClassNameOf(scaleBox) != "ScaleBox")
            continue;
        const int stretchAt = Offset(scaleBox, "Stretch");
        if (stretchAt < 0 || ue::Field<uint8_t>(scaleBox, stretchAt) != 2) // ScaleToFit
            continue;
        const TArray<void*>* inside = SlotsOf(scaleBox);
        void* canvas = inside && inside->Num > 0 ? Child(inside->Data[0]) : nullptr;
        const TArray<void*>* boxes = canvas && ue::ClassNameOf(canvas) == "CanvasPanel" ? SlotsOf(canvas) : nullptr;
        if (!boxes)
            continue;
        for (int b = 0; b < boxes->Num && b < 64; ++b) {
            void* slot = boxes->Data[b];
            if (!slot || ue::ClassNameOf(slot) != "CanvasPanelSlot")
                continue;
            if (std::any_of(g_boxes.begin(), g_boxes.end(), [&](const Box& known) { return known.Slot == slot; }))
                continue;
            const int layoutAt = Offset(slot, "LayoutData");
            if (layoutAt < 0)
                continue;
            const float* margin = &ue::Field<float>(slot, layoutAt);
            const double* anchors = reinterpret_cast<const double*>(margin + 4);
            const bool whole = margin[2] == DesignWidth && margin[3] == DesignHeight;
            const bool centred = anchors[0] == 0.5 && anchors[1] == 0.5 && anchors[2] == 0.5 && anchors[3] == 0.5;
            if (!whole && !centred)
                continue;
            Box box;
            box.Shift = !whole;
            box.Slot = slot;
            box.Owner = screen;
            box.OwnerIndex = index;
            box.Position = { margin[0], margin[1] };
            box.Size = { margin[2], margin[3] };
            g_boxes.push_back(box);
            Reshape(box);
        }
    }
}

void RestoreBoxes()
{
    for (const Box& box : g_boxes)
        if (Alive(box.Owner, box.OwnerIndex))
            Place(box.Slot, box.Position, box.Size);
    g_boxes.clear();
}

// The ScaleBox slot of a screen's root canvas that fills it, or null.
void* FittingSlot(void* screen)
{
    const int treeAt = Offset(screen, "WidgetTree");
    void* tree = treeAt >= 0 ? ue::Field<void*>(screen, treeAt) : nullptr;
    const int rootAt = Offset(tree, "RootWidget");
    void* root = tree && rootAt >= 0 ? ue::Field<void*>(tree, rootAt) : nullptr;
    const TArray<void*>* top = SlotsOf(root);
    for (int i = 0; top && i < top->Num && i < 64; ++i) {
        void* slot = top->Data[i];
        void* child = Child(slot);
        if (!child || ue::ClassNameOf(child) != "ScaleBox" || ue::ClassNameOf(slot) != "CanvasPanelSlot")
            continue;
        const int layoutAt = Offset(slot, "LayoutData");
        if (layoutAt < 0)
            continue;
        const double* anchors = reinterpret_cast<const double*>(&ue::Field<float>(slot, layoutAt) + 4);
        if (anchors[0] == 0 && anchors[1] == 0 && anchors[2] == 1 && anchors[3] == 1)
            return slot;
    }
    return nullptr;
}

void RestoreScaled()
{
    for (const Scaled& scaled : g_scaled)
        if (Alive(scaled.Widget, scaled.Index))
            SetTransform(scaled.Widget, 1, 0);
    g_scaled.clear();
}

void RestoreAnchors()
{
    RestoreScaled();
    const double whole[4] = { 0, 0, 1, 1 };
    for (const Placed& placed : g_placed)
        if (Alive(placed.Widget, placed.Index))
            SetAnchors(placed.Widget, whole);
    g_placed.clear();
}

} // namespace

void Apply(Mode mode, float aspect, int windowWidth, int windowHeight)
{
    if (mode == Mode::Game || aspect <= 0 || windowWidth <= 0 || windowHeight <= 0) {
        if (g_mode != Mode::Game) {
            RestoreBoxes();
            RestoreAnchors();
            g_mode = Mode::Game;
        }
        return;
    }

    // Where the picture is in the window, inside the bars a fixed shape adds.
    double anchors[4] = { 0, 0, 1, 1 };
    const double window = static_cast<double>(windowWidth) / windowHeight;
    if (aspect > window + 1e-4) {
        const double inset = (1.0 - window / aspect) / 2;
        anchors[1] = inset;
        anchors[3] = 1 - inset;
    } else if (aspect < window - 1e-4) {
        const double inset = (1.0 - aspect / window) / 2;
        anchors[0] = inset;
        anchors[2] = 1 - inset;
    }
    // The design at the picture's shape: as wide as the game's if it's taller, as tall if it's wider.
    const Vec2 design = aspect >= DesignWidth / DesignHeight ? Vec2 { DesignHeight * aspect, DesignHeight } : Vec2 { DesignWidth, DesignWidth / aspect };

    const bool anchorsChanged = std::memcmp(anchors, g_anchors, sizeof(anchors)) != 0;
    const bool designChanged = design.X != g_design.X || design.Y != g_design.Y;
    if (g_mode == Mode::Shape && mode != Mode::Shape)
        RestoreBoxes();
    std::memcpy(g_anchors, anchors, sizeof(anchors));
    g_design = mode == Mode::Shape ? design : Vec2 { DesignWidth, DesignHeight };
    const bool modeChanged = g_mode != mode;
    g_mode = mode;

    std::erase_if(g_placed, [](const Placed& p) { return !Alive(p.Widget, p.Index); });
    std::erase_if(g_scaled, [](const Scaled& x) { return !Alive(x.Widget, x.Index); });
    g_windowWidth = windowWidth;
    g_windowHeight = windowHeight;
    g_pictureWidth = static_cast<int>(windowWidth * (anchors[2] - anchors[0]) + 0.5);
    g_pictureHeight = static_cast<int>(windowHeight * (anchors[3] - anchors[1]) + 0.5);
    if (anchorsChanged || modeChanged)
        for (const Scaled& scaled : g_scaled)
            Fit(scaled);
    std::erase_if(g_boxes, [](const Box& b) { return !Alive(b.Owner, b.OwnerIndex); });
    if (anchorsChanged || modeChanged)
        for (const Placed& placed : g_placed)
            SetAnchors(placed.Widget, g_anchors);
    if ((designChanged || modeChanged) && mode == Mode::Shape)
        for (const Box& box : g_boxes)
            Reshape(box);

    // New screens come and go with every menu and round: look for them a few times a second.
    if (++g_tick < 15 && !anchorsChanged && !modeChanged && !designChanged)
        return;
    g_tick = 0;
    void* userWidget = ue::FindClass("UserWidget");
    if (!userWidget)
        return;
    for (void* widget : ue::AllObjectsOf(userWidget)) {
        if (ue::Field<uint32_t>(widget, ue::off::ObjectFlags) & ue::off::RF_ClassDefaultObject)
            continue;
        struct {
            bool ReturnValue;
        } top { false };
        const uint32_t index = ue::Field<uint32_t>(widget, ue::off::InternalIndex);
        // A screen the game made on its own (its outer is the game instance) and didn't add to the window the usual way: its ScaleBox
        // goes into the picture.
        void* outer = ue::Field<void*>(widget, 0x20);
        if (outer && outer == g_gameInstance) {
            ue::Call(widget, g_inViewport, &top);
            if (!top.ReturnValue) {
                if (FittingSlot(widget) && !std::any_of(g_scaled.begin(), g_scaled.end(), [&](const Scaled& x) { return x.Widget == widget; })) {
                    Scaled scaled;
                    scaled.Widget = widget;
                    scaled.Index = index;
                    const std::string cls = ue::ClassNameOf(widget);
                    scaled.Bar = cls.find("Guide_Button") != std::string::npos || cls.find("InformationBar") != std::string::npos;
                    // The screen's UI scale, from its last layout: pixels over its own units.
                    alignas(16) uint8_t geometry[256] = {};
                    ue::Call(widget, g_cachedGeometry, geometry);
                    const float uiScale = reinterpret_cast<const float*>(geometry)[2];
                    if (uiScale > 0.05f && uiScale < 8.0f)
                        scaled.UiScale = uiScale;
                    g_scaled.push_back(scaled);
                    Fit(scaled);
                }
                if (mode == Mode::Shape)
                    FindBoxes(widget, index);
                continue;
            }
        } else {
            ue::Call(widget, g_inViewport, &top);
            if (!top.ReturnValue)
                continue;
            if (!g_gameInstance && outer && ue::ClassNameOf(outer).find("GameInstance") != std::string::npos) {
                g_gameInstance = outer;
                mod::Log("HUD: the game's screens belong to %s", ue::ClassNameOf(outer).c_str());
            }
        }
        if (!std::any_of(g_placed.begin(), g_placed.end(), [&](const Placed& p) { return p.Widget == widget; })) {
            g_placed.push_back({ widget, index });
            SetAnchors(widget, g_anchors);
        }
        if (mode == Mode::Shape)
            FindBoxes(widget, index);
    }
}

} // namespace hudframe
