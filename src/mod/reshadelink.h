#pragma once
// ReShade, when the player has it in the game: its techniques and the values of its effects can be read and set from the editor, and
// kept in look keys like everything else. The mod registers with ReShade as an add-on, through ReShade's own add-on API.
//
// With ReShade there, recording and the editor also move to after ReShade's effects, so a video has the effects in it and the editor
// isn't drawn through them.
#include <string>
#include <vector>

class Ini;

namespace reshadelink {

struct Technique {
    std::string Effect; // the effect file, such as SMAA.fx
    std::string Name;
    bool On = false;
};

enum class Kind { Float, Int, Bool };

struct Value {
    std::string Effect;
    std::string Name;
    std::string Label; // what ReShade's own menu calls it
    Kind Kind = Kind::Float;
    int Count = 1; // 1 to 4 numbers
    float Numbers[4] = {};
    float Min = 0, Max = 1;
    bool Colour = false;
};

// ReShade is in the game and took the add-on.
bool Found();
// What's going on, for the menu: not there, there but too old, or ready.
std::string StatusText();

// The techniques ReShade has loaded, and the values of the effects that are switched on. Safe from the menu.
std::vector<Technique> Techniques();
std::vector<Value> Values();
void SetTechnique(const std::string& effect, const std::string& name, bool on);
void SetValue(const std::string& effect, const std::string& name, const float numbers[4]);

// The look: every technique's switch and every value, written the way a preset is.
void WriteLook(Ini& ini);
void ReadLook(const Ini& ini);
// Whether a look key's entry is a number that moves smoothly between two keys.
bool MovesSmoothly(const std::string& key);

// True once ReShade is drawing the game's frames: the overlay then does its work when ReShade says its effects are done.
bool OwnsPresent();

// Game thread, once per frame: looks for ReShade until it's found or clearly not there.
void OnGameTick();

} // namespace reshadelink

namespace overlay {
// Render thread, from ReShade, after its effects and before its own menu.
void PresentFromReShade();
} // namespace overlay
