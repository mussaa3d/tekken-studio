# Driving Tekken 8 for tests

Mouse clicks work on the game's menus. Keys need holding about 0.2 s (`hold_key`), or the game misses them. K is Back, M is Confirm (Esc doesn't work on every screen).
Positions below are in the 1456x819 screenshot frame, at 1080p windowed.

- From a cold start with the mod: game icon in the taskbar (495,802), click (728,400) to skip the intro, about 25 s later click (1200,570) at the title (sometimes twice), about 14 s later the offline notice: OK at (728,490). Then Replay tab, My Replays & Tips (170,246), Downloaded Replays tab (516,98), first row, Play Replay, Left (728,432).
- In a replay: K pause, I switch rounds, O/P round, L/; tip, B menu, A/D rewind and forward, U/N P1/P2 controls. After pressing arrows the prompts swap to the arrow keys.
- Activate the game from a script with `(New-Object -ComObject WScript.Shell).AppActivate(pid)`. `tools\press.ps1` sends keys computer use can't, like Insert.
- A pause from the mod freezes these menus too, so never send `pause 1` until a replay is really running. The tips side question can ignore its first input.
- Tips pause the replay until they're set to Hide in its menu (B): Recommended Punishment Techniques, High Moves You Can Duck Under, Throw Commands.

- Main menu tabs: Story (246,105), Offline (338,105), Online (432,105), Customization (552,105), Replay (674,105)
- Offline tab: Practice (138,312)
- Replay tab: Online Replay (160,177), My Replays & Tips (170,246)
- Replay list: first row (700,236). In the details box, Play Replay is (726,504). Delete Replay is just below it at (726,606), so be careful.
- Practice path: left side, then Return for the character, Return for the opponent, Return for a random stage.
- Offline notice at the title screen: click OK at (728,477).
