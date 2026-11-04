#pragma once
// Developer test channel, off unless tekken8mvm.ini has test_commands = 1. Lines in tekken8mvm\commands.txt are run on the game
// thread and the file is deleted; the "state" command writes tekken8mvm\state.txt. Lets tests drive features precisely.
namespace testcmd {

void Enable();
void OnGameTick();

} // namespace testcmd
