#pragma once
#include <string>
// Offline safety net. This is not a way round anything: if Steam is signed in, or the game holds a lasting connection to the
// internet, the mod turns itself off and stays off until the game restarts.
namespace guard {

void Start();
bool Tripped();

// Turns the mod off for the rest of the session (once): logs `reason` and shows `message` to the player.
void TurnOffBecause(const std::string& reason, const wchar_t* message);

// Stops this game process connecting to anything off this PC for the rest of the session, so a mod session has no traffic with
// any server at all. Call before the game starts talking.
void BlockInternet();
int BlockedAttempts();

// How many non-local connections the game held at the last check, and the last address seen.
int LastConnectionCount();
const char* LastRemoteAddress();

// 1 Steam is signed in, 0 Offline Mode, -1 not known yet.
int SteamState();

} // namespace guard
