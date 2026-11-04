#pragma once
// Developer test channel, off unless t8cine.ini has test_commands = 1. Lines in T8Cine\commands.txt are run on the game
// thread and the file is deleted; the "state" command writes T8Cine\state.txt. Lets tests drive features precisely.
namespace testcmd {

void Enable();
void OnGameTick();

} // namespace testcmd
