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
- Send no key before the game is really in front: a key sent too early lands in whatever window has focus. Click the menus instead where you can.
- A camera pose only suits one stage and moment. Read `view` from `state` in the game camera at that moment, then `pose` it, and check the shot before trusting a number: an empty greenscreen shot "matches" at 0.000%.
- `tools\test_realtime.ps1` measures what you see, through ddagrab. Crop away anything on top of the game. Judge cloth by looking too: the broken cloth and the healthy cloth differ more in shape than in the numbers.

## 2.0: scripts and commands
- `tools\drive.ps1 shot|click|key|focus` takes a picture of a screen at 1280x720, clicks in real screen pixels, holds keys, or brings the game to the front. `tools\t8log.ps1` sends commands and prints the state lines and new log lines you ask for. `tools\test_record_size.ps1` records a stretch at a picture size and reports the files.
- At 1920x1080 in real pixels: skip the intro (960,527), the title (1582,752), OK on the offline notice (960,648), the Replay tab (892,141), My Replays & Tips (225,324), the Downloaded Replays tab (682,131), row n of the list (870, 312 + 138 per row), Play Replay in the box (960,665), LEFT for the tips (960,571). Delete Replay is at (960,800): look at a picture before clicking in that box.
- At the end of a replay: Return to Replay List (960,537), then Yes (960,571). The question starts on No.
- Takes and actors: `take <name> <side> <in> <out>`, `takes`, `takedelete <name>`, `actor <i> <on> <take|-> [start speed end body]`, `actorplace <i> x y z yaw [scale]`, `actormove <i> dx dy dz dyaw`, `actorcam <i>`.
- Props, effects, lights: `prop <i> <on> <asset|-> [x y z pitch yaw roll scale]`, `propcam <i>`, `vfx <i> <on> <asset|-> <timeline> <start> <length> <rate> [x y z scale]`, `vfxrestart <i>`, `lightaim <i> <aim> <pitch> <yaw> <cone> <shadows>`, `lightcam <i>`, `assets <Class> [part of a name]`, `assetpath <Class> <name>`.
- Picture: `capsize w h`, `outsize w h`, `winsize w h` (no recording), `winmove x y`, `shot <file>` (the next frame as raw pixels in the mod's folder; format 24 is `x2bgr10le` to ffmpeg).
- Looks and files: `preset save|load <name>`, `shotfile save|load <name>`, `reshade`, `reshadetech <effect> <technique> <0|1>`, `reshadevalue <effect> <value> a [b c d]`.
- `state` has lines for `picture`, `take`, each `actorN`, `propN` and `vfxN` that's on, and `reshade`.
- Undo only keeps steps while the editor is open: it's the editor that watches for changes.
- A replay stopped by one of the game's tips looks paused to the mod and to seeks. With the editor closed, K plays it again.
