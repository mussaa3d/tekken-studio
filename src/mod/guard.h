#pragma once
// Offline safety net. This is not a way round anything: when the game process connects to the internet,
// the mod turns itself off and stays off until the game restarts.
namespace guard {

void Start();
bool Tripped();

// How many non-local connections the game held at the last check, and the last address seen.
int LastConnectionCount();
const char* LastRemoteAddress();

} // namespace guard
