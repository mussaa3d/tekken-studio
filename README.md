# tekken8mvm

Called T8Cine until now. The launcher moves your settings, presets and videos over to the new name by itself.

Cinematic tools for Tekken 8 on PC, in the spirit of CoDMVM, HLAE and IWXMVM. Offline only.

Tested in the real game on version 3.02.02 (Steam build 25221600), on replays and through real mouse and keyboard input. What wasn't tested is listed under [Not verified](#not-verified).

## What's in it
The editor (Insert) is laid out like IWXMVM: the game picture on the left, tool tabs on the right, playback controls and a keyframe timeline along the bottom.

- **Timeline:** the replay's real frame count, with camera keys, a timescale curve, the recording range, round markers, and the live tick and tick rate.
- **Rewinding:** click anywhere on the ruler and the replay goes there, back or forward, to the exact frame, in under a second. A moment looks the same however you got to it.
- **Preview shot:** goes to the first camera key, flies the path and stops on the last key.
- **Record shot:** the same, recording from the first key's exact frame to the last key's.
- **Round buttons:** R1, R2, R3 and so on beside the timescale jump straight to a round of the replay.
- **Streams:** several files at once that line up frame for frame: picture, greenscreen (green or blue), stage only, fighters on black, depth. Every look also shows on screen, so you can see it before recording.
- **Depth:** the game's real depth buffer as a smooth grey picture with clean silhouettes, near things white and far things dark. Near, far and curve sliders set how it falls off.
- **Speed ramps:** ease between timescales by dragging keys on the curve.
- **Free camera:** fly, look, roll and zoom, while the fight runs or is paused.
- **Attached camera:** fix the camera to a bone, orbit it, or watch it from wherever you fly.
- **Time control:** pause, frame step and slow motion down to 0.01x, with poses blended between the game's 60 Hz steps. Clothes, people and the stage slow and stop along with the fight.
- **Dolly paths:** keyframes on the timeline in three styles: Smooth, Linear and CoDMVM. A key can turn the camera further than half a circle, and the path goes the way you turned. Save, load, and export HLAE CamIO for After Effects or Blender.
- **Rage art and heat smash camera:** keep the normal fight camera through cinematics and big-hit zooms, or use a fixed classic 2D camera all fight.
- **Effects:** hide hit sparks and glows, cancel rage art screen effects, hide the HUD (and the game's FPS counter).
- **Visuals:** Filmtweaks-style colour, depth of field with autofocus, lens effects, fog in a colour you pick that covers the map's own haze and mist, sun and sky, your own lighting rig, and sliders for engine settings. Add your own in `tekken8mvm\dvars.ini`.
- **Recording:** frame-locked video through ffmpeg (H.264, H.265, ProRes or PNG). Smooth however slowly it renders.
- **Motion blur:** real blur from averaging 2 to 64 renders per frame, with a shutter angle.
- **Audio pass:** captures only the game's sound to a WAV at 1x, since frame-locked video has none.
- **Presets and shots:** save a look, or a whole shot with its camera path and speed ramp.
- **Skins:** choose pak mods per session. They're only in the game folder while the session runs.
- **Replay sharing:** export a replay to a file, send it to a friend, and they load it. No servers involved.
- **Replay vault:** snapshot your own saves on this PC, tagged with the game build, and restore them.

## Use
1. Run `dist\tekken8mvm.exe`. It needs [ffmpeg](https://ffmpeg.org) on your PATH for recording.
2. In Steam, choose **Steam > Go Offline**.
3. Click **Start cinematic session**. Keep the launcher open: it cleans up when the game closes.
4. In the game, open a replay: **Replay > My Replays & Tips**, then pick one.
5. In the replay's own menu (B), set the three tips (punishment, high moves, throws) to **Hide**, or the replay stops at every tip.
6. Press **Insert** for the editor.

To make a shot: pause, go to a moment on the ruler, press F5 and fly the camera, press K for a key, move along the timeline, and add more keys. Then press **Preview shot** to watch it and **Record shot** to record it.

| Key | Does |
|---|---|
| Insert | Editor |
| F5 | Free camera |
| F6 | Play dolly path |
| F7 | Record |
| F8 | Pause |
| `.` | Next frame |
| `[` `]` | Slower, faster |
| F9 | Hide the HUD |
| K | Add a dolly key (free camera on) |

Free camera: `WASD` fly, `Q` `E` down and up, hold right mouse to look, `Shift` faster, `Ctrl` slower, wheel sets speed, `Z` `C` roll, `R` level, `+` `-` zoom.

A paused editor stops the game's own menus too, including the one at the end of a replay. Unpause (F8) if a game menu stops responding.

## Sharing replays
In the editor's REPLAYS tab, pick a replay and press **Export**. The `.t8replay` file goes in `shared_replays` next to your recordings. Send it to a friend, who puts it in the same folder and presses **Load**. It then shows up in the game's **Replay > Downloaded Replays**. If that screen is open, leave it and come back.

- Both of you need the same game version. A replay made on an older version won't play.
- The file holds both players' names.
- Loading only changes the game's memory, so the replay is gone when you close the game, unless the game itself saves that group of replays, which happens when you download or delete a replay nearby. Your existing replays aren't touched.

The game keeps every replay decoded in fixed slots in memory, so sharing copies a slot. Nothing is decrypted by tekken8mvm and no save file is edited.

## Good to know
- **Rewinding needs a replay.** Practice mode has no saved points, so the ruler only goes forward there.
- **Windows Volume Mixer:** the audio pass captures the game after its slider. If the game is at zero there, the recording is silent, and the editor tells you.
- **The greenscreen and depth come from the game's depth buffer.** Anything that doesn't write depth, like sparks and glows, is painted over in the greenscreen and doesn't show in the depth. If the buffer can't be read, the editor says so, and the greenscreen video falls back on keying the dark background.
- **Rewinding changes the picture slightly compared to the first playthrough:** by less than 3 cm on a fighter's position, right after a jump. It's the same every time.
- **Clothes in very slow motion:** cloth only moves when the fight takes a step, so below about 0.1x you may see it update in small hops.
- **Plants still sway while paused** on maps with foliage. That wind isn't tied to the game's clock.
- **The first seconds of a replay can't be rewound to.** The game's first saved point lands about 4.5 s in, just after the first round's intro. Round buttons land just after each round's intro in the same way.

## Not verified
- **A rage art itself.** The filter was tested on the game's real mid-round cinematic camera moves and zooms, and on round intros, in replays that happened to have no rage art. Rage arts should behave the same, but this one case hasn't been watched.
- **The "No rage art screen effects" option** runs without trouble, but it hasn't been seen against a rage art for the same reason.
- **A friend's PC.** Export and load were tested on one PC and one Steam account, by loading a replay exported moments earlier.

## Bans and anti-cheat
Bandai Namco's stated ban reasons include unauthorized mods, and no mod can promise you're safe. tekken8mvm doesn't hide from anything. It stays off online play instead:
- A cinematic session only starts with Steam in Offline Mode.
- While the mod runs, it stops the game's outgoing internet connections. Even in Steam's Offline Mode the game keeps trying to reach servers, and the log lists each connection it stopped.
- If Steam goes online, the mod turns itself off until the game restarts.
- The mod and skins leave the game folder when the game closes, even after a crash. A normal launch makes sure they're gone first.

Don't play online with the mod or skins in the game folder.

## After a game patch
If the mod stops working, `tekken8mvm\tekken8mvm.log` in the game's `Win64` folder names the pattern that no longer matches. Put a new one in `tekken8mvm\signatures.ini` as `Name = AA BB ?? CC`; there's no rebuild needed. `tools\check_patterns.py` checks them against the exe. If only the rewinding patterns stop matching, the timeline falls back to counting steps and everything else keeps working.

## Build
Needs Visual Studio 2022 Build Tools. Run `build.cmd`. Tests: `build\tekken8mvm_tests.exe` and `tools\harness\run_all.ps1`. `tools\` also has the scripts used to test against the real game, with `test_commands = 1` in `tekken8mvm\tekken8mvm.ini`.

## Credits and license
GPL-3.0. Engine notes, patterns and the method for smooth motion come from the [Tekken Overlay rebuild](https://github.com/parsaes80/tekken-overlay-rebuild), based on KulaGGin's Tekken Overlay. OpenDojo's research notes helped with the game's subsystems.
Uses [Dear ImGui](https://github.com/ocornut/imgui) and [MinHook](https://github.com/TsudaKageyu/minhook). Written with Claude.
Contains no game files. Not affiliated with Bandai Namco.
