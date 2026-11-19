#pragma once
// BlueSpark's own functions that the studio calls, declared here exactly as its prebuilt library (rex\prebuilt\lib\ttt2_ours.lib,
// release 23) exports them, so the linker finds them by their C++ names. They move from release to release, which is why a studio
// build is tied to one BlueSpark release (build-studio.ps1 checks it).
#include <cstdint>
#include <filesystem>
#include <string>

// "The outside hold": freezes an offline fight (Practice, a replay) for as long as someone asks, sound included. Only with BlueSpark's
// pages on (--ttt2_bluespark), which is how the studio starts the game.
void Ttt2TrainingHoldRequestOffline(const char* who, bool hold);
bool Ttt2TrainingHoldHolding(uint8_t* base);
void Ttt2TrainingHoldRequest(const char* who, bool hold); // the same, for Practice only
bool Ttt2ReplayHolding();

// The keyboard layer that plays the game from the keys: captured, the keys stop reaching the fight (as for its Controller Setup page).
void Ttt2KeyboardCapture(bool on);

bool Ttt2BlueSparkOn();
bool Ttt2FightInPlay();
bool Ttt2FightPlayingNow();
uint64_t Ttt2EngineFrameNow();
uint64_t Ttt2TestFightSteps();
uint64_t Ttt2TestFrame();

// Replays: BlueSpark records every offline Versus match and every online match, and plays them back in Practice.
bool Ttt2ReplayPlaying();
uint64_t Ttt2ReplayFrameNow();
// Research: the two counts of a replay being run forward (where it is, where it's going), and whether it's running fast now.
void Ttt2ReplayForwardFrames(uint64_t* a, uint64_t* b);
bool Ttt2ReplayForwardFast(uint8_t* base);
std::filesystem::path Ttt2ReplaysFolder();
bool Ttt2ReplayChoose(uint8_t* base, const std::string& file);
