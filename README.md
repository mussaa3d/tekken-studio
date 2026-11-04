# T8Cine

Cinematic tools for Tekken 8 on PC, in the spirit of CoDMVM and HLAE. Offline only.

> **Status: not tested in the game yet.** The menu, recorder, launcher and maths are tested without the game. The game hooks aren't.

## What's in it
The editor (Insert) is laid out like IWXMVM: the game picture on the left, tool tabs on the right, playback controls and a keyframe timeline along the bottom.

- **Timeline:** camera keys, a timescale curve, the recording range, and the live tick and tick rate.
- **Speed ramps:** ease between timescales by dragging keys on the curve.
- **Free camera:** fly, look, roll and zoom, while the fight runs or is paused.
- **Attached camera:** fix the camera to a bone, orbit it, or watch it from wherever you fly.
- **Time control:** pause, frame step and slow motion down to 0.01x, with poses blended between the game's 60 Hz steps.
- **Dolly paths:** keyframes on the fight clock, smooth through every key. Save, load, and export HLAE CamIO for After Effects or Blender.
- **Rage art and heat smash camera:** keep the normal fight camera through cinematics, or use a fixed classic 2D camera all fight.
- **Visuals:** hide the HUD, remove hit effects and camera shakes, Filmtweaks-style colour, depth of field with autofocus, lens effects, fog, sun and sky, your own lighting rig, and sliders for engine settings. Add your own in `T8Cine\dvars.ini`.
- **Recording:** frame-locked video through ffmpeg (H.264, H.265, ProRes or PNG). Smooth however slowly it renders.
- **Streams:** several files at once that line up frame for frame: picture, greenscreen, stage only, fighters on black, depth.
- **Motion blur:** real blur from averaging 2 to 64 renders per frame, with a shutter angle.
- **Audio pass:** captures only the game's sound to a WAV at 1x, since frame-locked video has none.
- **Presets and shots:** save a look, or a whole shot with its camera path and speed ramp.
- **Skins:** choose pak mods per session. They're only in the game folder while the session runs.
- **Replay sharing (being built):** export your own gameplay to a file, send it to a friend, and they load it. No servers involved.
- **Replay vault:** snapshot your own saves on this PC, tagged with the game build, and restore them.

## Use
1. Run `dist\T8CineLauncher.exe`. It needs [ffmpeg](https://ffmpeg.org) on your PATH for recording.
2. In Steam, choose **Steam > Go Offline**.
3. Click **Start cinematic session**. Keep the launcher open: it cleans up when the game closes.
4. In the game, press **Insert** for the menu.

| Key | Does |
|---|---|
| Insert | Menu |
| F5 | Free camera |
| F6 | Play dolly path |
| F7 | Record |
| F8 | Pause |
| `.` | Next frame |
| `[` `]` | Slower, faster |
| F9 | Hide the HUD |
| K | Add a dolly key (free camera on) |

Free camera: `WASD` fly, `Q` `E` down and up, hold right mouse to look, `Shift` faster, `Ctrl` slower, wheel sets speed, `Z` `C` roll, `R` level, `+` `-` zoom.

## Replays
The game saves each replay as its own encrypted file. T8Cine's goal is to export one to a file you can send to someone, and to load a file
you received, without touching your real saves or Steam Cloud. This part isn't finished, because it needs the running game. Replays are input
logs, so both people need the same game build, and a patch can stop an old replay from playing.

## Bans and anti-cheat
Bandai Namco's stated ban reasons include unauthorized mods, and no mod can promise you're safe. T8Cine doesn't hide from anything. It stays
off online play instead:
- A cinematic session only starts with Steam in Offline Mode.
- If the game connects to the internet, the mod turns itself off until the game restarts.
- The mod and skins leave the game folder when the game closes. A normal launch makes sure they're gone first.

Don't play online with the mod or skins in the game folder.

## After a game patch
If the mod stops working, `T8Cine\t8cine.log` in the game's `Win64` folder names the pattern that no longer matches. Put a new one in
`T8Cine\signatures.ini` as `Name = AA BB ?? CC`; there's no rebuild needed. `tools\check_patterns.py` checks them against the exe.

## Build
Needs Visual Studio 2022 Build Tools. Run `build.cmd`. Tests: `build\t8cine_tests.exe` and `tools\harness\run_harness.ps1`.

## Credits and license
GPL-3.0. Engine notes, patterns and the method for smooth motion come from the
[Tekken Overlay rebuild](https://github.com/parsaes80/tekken-overlay-rebuild), based on KulaGGin's Tekken Overlay.
Uses [Dear ImGui](https://github.com/ocornut/imgui) and [MinHook](https://github.com/TsudaKageyu/minhook). Written with Claude.
Contains no game files. Not affiliated with Bandai Namco.
