# T8Cine mod log

Working notes. Facts here were read from the sources named, or measured where it says so.

## Target
- TEKKEN 8, Steam app 1778820. Unreal Engine 5. Executable `Polaris-Win64-Shipping.exe` in `Polaris/Binaries/Win64`.
- Saves: `%LOCALAPPDATA%\TEKKEN 8\Saved`. Pak mods go under `Polaris/Content/Paks/`.

## Route
Native C++ DLL loaded as `dinput8.dll` next to the exe, the slot opendojo and other Tekken 8 mods use. UE4SS would cover
menus and Lua, but slow motion needs a detour on the game's tick and the recorder needs the swap chain, so native it is.

Engine patterns, offsets, the 60 Hz accumulator idea and the HUD word lists come from the Tekken Overlay rebuild
(github.com/parsaes80/tekken-overlay-rebuild, GPL-3.0). Its notes were checked against game build `1787726436`.

## Online rules
- No anti-cheat evasion of any kind. The mod stays off online play instead.
- The launcher only starts a cinematic session with Steam in Offline Mode, and removes the mod and skins when the game closes.
- The DLL does nothing without a fresh `session.lock` from the launcher, so a stray copy is a plain forwarder.
- The guard turns all hooks off if the game process holds a non-local TCP connection.

## Replays
The goal is sharing: export your own gameplay to a file, send it, and the other person loads it. No servers involved.

Measured on the saves already on this PC (`SaveGames\<steamid64>\`, read-only):
- 24 `replay_game_no<N>.sav` (N = 11, 21 ... 241), all exactly 1,536,096 bytes. So 1 replay = 1 file, in a fixed-size slot. Also `replay_sys1.sav` (2,098,672 bytes).
- Encrypted: entropy 8.00, no plain strings, sizes are multiples of 16. No repeated 16-byte blocks, so not plain ECB.
  XOR of two files is still random, so each file gets its own keystream or IV.
- `steam_autocloud.vdf` is there, so Steam Cloud syncs this folder.
- Not known yet: whether the key depends on the Steam account. That decides whether raw files work on another account.

Plan: export and import plain data at the point where the game has decrypted a replay. Export copies it out to a file. Import hands the
game that data in memory. No keys get extracted, the real saves and Steam Cloud stay untouched, and an account-bound key stops mattering.
`savetrace.cpp` logs the file and decrypt calls on first launch, to find that point.

Limits to expect:
- Replays are input logs, so they can stop playing after a patch (Zaplat Gaming, Sept 2026, on patch 3.02.01; Steam discussions). Files must carry
  the game build, and both people need the same build.
- A replay may contain player names or IDs. Check before sharing and say so in the export.

## Layout
Modelled on IWXMVM's source (github.com/reallyluckyy/IWXMVM): MenuBar, GameView, PrimaryTabs (DEMOS, CAMERA, VISUALS, RECORD), ControlBar, KeyframeEditor.
The game picture is a copy of the back buffer drawn scaled through ImGui. Its SRV forces alpha to 1, since the game's alpha would otherwise read as transparency.

## Streams and depth
With more than one pass per sample, time is held with tokens: the recorder grants one step's worth of game time after all passes for that sample are captured.
Settle frames cover the delay between asking for a look and seeing it. Depth is a guess until the game runs: white fog minus black fog should cancel the surface colour
(`colour*(1-f) + fog*f`), leaving f, which grows with distance. Tonemapping isn't linear, so expect a usable depth ramp and not a calibrated one.

## Guesses to prove in the game
- Reflection offsets for FField (`ChildProperties 0x50`, `Next 0x20`, `Name 0x28`, `Offset 0x4C`, bool at `0x79/0x7B`). Standard UE 5.3, not from the Tekken notes. Reads are SEH-guarded.
- That the engine applies `POV.PostProcessSettings` (depth of field, colour).
- `AddComponentByClass` for the lighting rig, and that fighters react to added lights.
- Greenscreen through fog needs a fog object on the stage. Otherwise the fallback paints pure black green.

## Tested with no game (measured 2026-11-04)
- `tools/harness/run_harness.ps1 -UiTest`: scripted mouse through the real editor, every tab drawn with all sections open. Tabs, key and curve dragging, deleting, range handles, ruler, pause, timescale to 0.01,
  zoom and pan, preset, shot and path round trips. 32 of 32 pass.
- `tools/harness/run_all.ps1`: two and four streams (equal frame counts, each in step), settle frames, motion blur at 360 and 180 degree shutters (correct steps, soft edges), depth's frame count,
  audio pass against a 440 Hz tone through real WASAPI process loopback (rms 2811). All pass.
- `t8cine_tests`: rotation maths, dolly spline (through every key, no kinks or flips), fighter-relative camera, scanner, ini, VDF parsing,
  session install and removal (restores another mod's dinput8.dll byte for byte), vault snapshot and restore.
- `tools/harness/run_harness.ps1` + `check_video.py`: D3D12 stand-in with the real DLL. 120 frames in, 120 frames out, the bar moves exactly
  1 px per frame, no menu pixels in the video. 5 of 5 runs passed after the harness fix below.

## Gotchas
1. Harness "hang" was the harness: it finished its frames before a late-starting recording could stop, and stopping needs a Present call.
   A running game never hits this. The harness now waits for the hooks first.
2. Steam's manifest `BytesDownloaded` isn't updated during a download. `content_log.txt` has the rates.
3. Shipping builds strip `ShowFlag.*` console commands, so show flags are written to the viewport's bitfield.
4. Text-mode `ofstream` writes CRLF; read back in binary, the build ID in `vault.txt` kept a stray `\r`. A unit test caught it.

## Not verified yet (needs the game)
- All game hooks, the camera override, FOV offset `+0x2300`, time dilation behaviour, HUD hiding, effect removal.
- Cinematic filter thresholds, and whether the game starts at all with Steam in Offline Mode.

## Restore
Launcher closes the session by removing `dinput8.dll` and `Paks/~T8Cine`. Manually: delete those two and `T8Cine/session.lock`.
Save backups: see the entry below once the first backup is taken.
