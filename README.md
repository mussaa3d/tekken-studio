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
- **Streams:** several files at once that line up frame for frame: picture, greenscreen (green or blue), stage only, fighters on black, depth, effects on black. Every look also shows on screen, so you can see it before recording.
- **Depth:** the game's real depth buffer as a smooth grey picture with clean silhouettes, near things white and far things dark. Near, far and curve sliders set how it falls off.
- **Speed ramps:** ease between timescales by dragging keys on the curve.
- **Free camera:** fly, look, roll and zoom, while the fight runs or is paused.
- **Attached camera:** fix the camera to a bone, orbit it, or watch it from wherever you fly.
- **Fighter POV:** look out of either fighter's eyes (CAMERA > Attach to fighter > Style: Fighter POV). Their head is hidden so it doesn't fill the picture, and you can keep the horizon level.
- **Lighting, stage and frame** (VISUALS tab), after [TekkenOverlay](https://github.com/TekkenOverlay/TekkenOverlay)'s display settings and simplified stages, rebuilt here:
  - **Lighting:** switch the stage's lights off (the sun included), take the key light off the fighters, or colour the light on them. The fighters' light is mostly a colour the stage's `BP_CharacterLightController` puts on their materials, so the tint goes through that and touches only the fighters.
  - **Quality:** global illumination (none, Lumen, screen space), reflections, their quality, shadow quality and anti-aliasing. The x beside one puts the game's own back.
  - **Stage:** a plain stage, every stage mesh in the engine's grey material, and no camera shake.
  - **Frame:** the game's own shape, fill the screen with no black bars, or a fixed aspect ratio such as 2.39:1, which the recording keeps.
- **Replay display (VISUALS tab):** the game's own display settings for replays from inside the editor: command history, HUD, health info, in-battle status (the combo counter), attack and frame info, hit properties (the High, Mid and Low text), the tips and version info. Pick what you want and press **Apply to the replay**: the tool opens the replay's menu and sets them, with your mouse moving by itself for a few seconds.
- **Undo:** Ctrl+Z and Ctrl+Y undo and redo your settings, camera path and speed ramp, up to 64 steps. **File > Reset the mod** puts every setting back to how it starts.
- **From the camera:** the depth stream now starts at the camera (Depth near 0), autofocus has **From the camera**, and **Fog starts at the camera** thickens the fog only with distance from you.
- **Rule of thirds:** guides over the picture (View menu), never in a recording.
- **No slow motion on big hits and KOs** (Time group) holds the game's own slowdowns at normal speed. At a KO the fighters still hold their pose for a moment, because that's part of the fight, but the world and effects keep moving.
- **Change replay from inside a replay:** in the REPLAYS tab, pick one and press **Play this replay**, or double-click it. It takes about 20 seconds.
- **Time control:** pause, frame step and slow motion down to 0.01x, with poses blended between the game's 60 Hz steps. Clothes, people, plants and the stage slow and stop along with the fight: at 0.01x a jacket strap moves at 0.01 speed. In very deep slow motion cloth moves in small hops rather than a perfect glide (about 5 a second at 0.023x), because its solver comes apart in slices shorter than 1/480 s of fight time. Rewinding to a moment gives the same clothes every time.
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

## Install
Run `tekken8mvm-1.0-setup.exe`. It installs for your Windows account only, so it doesn't ask for administrator rights, in `%LOCALAPPDATA%\Programs\tekken8mvm`, with a Start menu shortcut. Its one dependency is [ffmpeg](https://ffmpeg.org), for recording. The installer downloads ffmpeg 9.0.2 (about 110 MB) from the [GyanD builds](https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2) on GitHub and checks its SHA-256 before using it. Untick that to use an ffmpeg already on your PATH. Nothing else is needed: the tool carries its own runtime, and DirectX 12 comes with Windows.

The installer isn't signed, so Windows SmartScreen may say it's from an unknown publisher (More info > Run anyway). Uninstalling (Windows Settings > Apps) takes the mod out of the game folder first, and leaves your recordings, skins and saved replays.

## Use
1. Start **tekken8mvm** from the Start menu (or run `dist\tekken8mvm.exe` from a build).
2. In Steam, choose **Steam > Go Offline**.
3. Click **Start cinematic session**. Keep the launcher open: it cleans up when the game closes.
4. In the game, open a replay: **Replay > My Replays & Tips**, then pick one.
5. In the replay's own menu (B), set the three tips (punishment, high moves, throws) to **Hide**, or the replay stops at every tip.
6. Press **Insert** for the editor.

To make a shot: pause, go to a moment on the ruler, press F5 and fly the camera, press K for a key, move along the timeline, and add more keys. Then press **Preview shot** to watch it and **Record shot** to record it.

While the editor is open it has the keyboard, and the game gets none of it. With the editor closed every key goes to the game (Escape opens its menu and so on), and Insert is the only key the mod listens for. The free camera flies only from the editor. Closing the editor also lifts its pause (unless it's recording), since the game's own controls can't move a game the editor holds still.

| Key (editor open) | Does |
|---|---|
| Insert | Open or close the editor |
| Space or F8 | Play and pause |
| Left, Right | Back and forward 1 s (Shift 5 s, Ctrl 1 frame) |
| `,` `.` | Back and forward 1 frame |
| Up, Down, or `[` `]` | Faster, slower |
| Home | The very start of the replay, intro included |
| Ctrl+Z, Ctrl+Y | Undo, redo |
| F4 | Free camera looks with the mouse or the arrow keys |
| F5 | Free camera |
| F6 | Play dolly path |
| F7 | Record |
| F9 | Hide the HUD |
| K | Add a dolly key (free camera on) |

With the free camera looking with the arrow keys (F4), the arrows look around, and `,` `.` move along the timeline. The editor's **Keys** menu lists all of these.

Free camera: `WASD` fly, `Q` `E` down and up, hold right mouse over the picture to look, `Shift` faster, `Ctrl` slower, `F4` switches looking between the mouse and the arrow keys, wheel or `Z` `C` roll, `R` level, `Ctrl`+wheel or `+` `-` zoom, `Shift`+wheel sets speed.

**View > Hide hit labels and frame data** hides the words that pop up on hits (Counter and so on) and the frame data panel, which the game draws as one widget. The effects, health bars and input list stay.

**Mods** (REPLAYS tab) lists your own pak mods and switches them off and on while the game runs, through the game's own pak manager. Whatever is already loaded, like HUD pictures, stays until the game loads it again. A mod you switch off stays off the next time the game starts, until you switch it back on there.

The **Effects on black** stream keeps only the fight's particle effects (sparks, trails, smoke, and anything a fighter carries as an effect, like Bryan's weapon) on black, with the stage and fighters hidden. Turn on the depth stream next to it for a matching depth pass. Effects behind a fighter show through, since the fighter isn't there to hide them.

A paused editor stops the game's own menus too, including the one at the end of a replay. Unpause (Space or F8 in the editor) if a game menu stops responding.

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
- **Fights run at 60 fps** at 1x and slower, because cloth only comes out the same with every frame the same length. Above 1x the frame rate is free again.
- **Your game settings are kept apart.** Graphics and resolution set during a cinematic session stay with cinematic sessions, and normal play keeps its own. The launcher swaps them, so the first session starts from what you already had. If you start the game from Steam, you get your normal settings.
- **A rewind takes a second or two longer** than it used to, because the last second plays at normal pace for the cloth to settle.
- **Changing replay** presses keys in the game's own menus, so leave the keyboard alone and the game in front until it's done. It needs the game's list sorted by New. Replays from older game versions are refused, because the game won't play them.
- **Leaf edges shimmer a little while paused.** That's the game's anti-aliasing, not movement: the plants themselves are still.
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
Needs Visual Studio 2022 Build Tools. Run `build.cmd`. `installer\build_installer.cmd` builds the tool and then the installer into `release\`, and needs [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`). Tests: `build\tekken8mvm_tests.exe` and `tools\harness\run_all.ps1`. `tools\` also has the scripts used to test against the real game, with `test_commands = 1` in `tekken8mvm\tekken8mvm.ini`.

## Credits and license
GPL-3.0. Engine notes, patterns and the method for smooth motion come from the [Tekken Overlay rebuild](https://github.com/parsaes80/tekken-overlay-rebuild), based on KulaGGin's Tekken Overlay. OpenDojo's research notes helped with the game's subsystems.
Uses [Dear ImGui](https://github.com/ocornut/imgui) and [MinHook](https://github.com/TsudaKageyu/minhook). Written with Claude.
Contains no game files. Not affiliated with Bandai Namco.
