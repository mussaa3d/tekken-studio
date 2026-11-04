# tekken8mvm mod log

Working notes. Facts here were read from the sources named, or measured where it says so. Measured on game version 3.02.02, exe TimeDateStamp 1787726436, Steam build 25221600.

## Target
- TEKKEN 8, Steam app 1778820. Unreal Engine 5.2. Executable `Polaris-Win64-Shipping.exe` in `Polaris/Binaries/Win64`.
- Saves: `%LOCALAPPDATA%\TEKKEN 8\Saved`. Pak mods go under `Polaris/Content/Paks/`.

## Route
Native C++ DLL loaded as `dinput8.dll` next to the exe, the slot opendojo and other Tekken 8 mods use. UE4SS would cover menus and Lua, but slow motion needs a detour on the game's tick and the recorder needs the swap chain, so native it is.

Engine patterns, offsets, the 60 Hz accumulator idea and the HUD word lists come from the Tekken Overlay rebuild (github.com/parsaes80/tekken-overlay-rebuild, GPL-3.0).

## Online rules
- No anti-cheat evasion of any kind. The mod stays off online play instead.
- The launcher only starts a cinematic session with Steam in Offline Mode, and removes the mod and skins when the game closes.
- The DLL does nothing without a fresh `session.lock` from the launcher, so a stray copy is a plain forwarder.
- The network block stops `connect`, `WSAConnect` and `ConnectEx` to non-local addresses. Measured: in Steam's Offline Mode the game still tries, repeatedly, 35.186.241.36:443 and addresses in 2.19.x and 2.20.x. They're logged as they're stopped.
- The guard turns all hooks off if `SteamUser()->BLoggedOn()` says Steam is online.

## Replay position and rewinding (measured)
- The battle subsystem comes from the registry (`0x18e0590`) through a keyed lookup (`0x18e0440`). `[battle+0x3C4]` is a frame count across the whole match. `[battle+0x300]` is the round. `[battle+0x1C]` is the remaining round time.
- When a replay opens, the game runs through it once and fills a list of saved points: `[manager+0x98]` then `+0xE0`/`+0xE8` is a vector of 0x1440-byte entries, with type at +0, snapshot index at +4, round at +8, frame at +0x14. Type 0 is every 301 frames, 3 a round start, 2 the match's end, 6 something else (a tip, by the look of it), 1 the very start.
- `0x19400F0(manager, kind, flag)` is the rewind: it reads the frame, then `0x1940140` picks the newest entry at least 120 frames older (`0x1939D40`, only in the current round for kind 0), copies it (`0x1929AF0`) and restores it (`0x193C300`). `0x193E3A0` is fast forward. Kind 4 is by round.
- The controller's input code is `0x5C10150`, reached inside the game's tick. Its bit 2 branch is the rewind button. After the jump it calls `vtable[0x578/8](controller, result, 0)` and clears `[controller+0x7C]`. Vtable 0x578 starts a 17-tick transition (`[controller+0x3C]`, `+0x70`, `+0x74`), and its first tick resets both players' input state.
- **Skipping that follow-up made the fight play out differently after a jump.** With it, positions match to all printed digits through any route (8 of 8 at clock 40, 4 of 4 at clock 85). See `tools/test_determinism.ps1`.
- Seeks that stepped forward without a jump landed differently from seeks that jumped, so every seek now jumps. Stepping one tick per frame from any landing gives the same result as itself, within 3 cm of a direct landing: the actor's location lags one step after a batch of fast steps.
- Timing: seeks land to the frame in 0.3 to 0.9 s, across rounds.

## Replay sharing (measured)
- All replays sit decoded in one 107 MB block of writable memory (112,873,472 bytes on this PC). First slot at +0x41590, 915,488 bytes each, used part 653,337 bytes. The tail of the block isn't all slots: overwriting "slot 122" hit something with pointers in it. Import uses the slot after the last used one, and refuses near the end.
- Slot header: +0 tag 14, +4 flags, +8 game version (30202 = 3.02.02), +12 data version, +16 time played, +24 time downloaded (non-zero means it's listed under Downloaded), +0x28 record size. Player names: UTF-8 at +81434 and +108282.
- Each `replay_game_no<K>1.sav` holds 5 slots. So a file isn't one replay, which is why file copying was never going to work.
- The game's list reads the slots directly, when you enter the Replay screen. A filled slot right after the last one shows up and plays. Tested through the UI: export, load, listed, played.
- Not tested: a different PC or Steam account. In memory it shouldn't matter, since the file encryption never comes into it.
- `tools/savetrace` shows no BCrypt use: the game has its own decryption, and tekken8mvm doesn't touch it.

## Depth buffer (measured)
- `ID3D12Device::CreateDepthStencilView` (table slot 21) ties a descriptor to a resource and `OMSetRenderTargets` (command list slot 46) says which is in use. The scene's depth is the picture-sized one bound most often in a frame. Here 1536x864, format 19 (R32G8X24), for a 1920x1080 picture, with the engine cycling through several resources of that size.
- It can be read as `R32_FLOAT_X8X24_TYPELESS` at Present with no barrier, on an RTX 2070. Not tried on AMD or Intel. Its contents match the picture on the same frame.
- Reversed depth with no far plane: distance in cm = 10 / value, and 0 means nothing was drawn. Hidden stage plus value 0 gives an exact key.
- This replaced the fog pair (white fog minus black fog), which had colour holes, took two renders per frame and followed a 16-pixel grid.

## Time (measured)
- **The fighters ran across the screen in slow motion after a rewind.** A seek runs up to 32 steps per frame, and the pose blend went from the last frame's pose, up to 32 steps old. The first slow step then played all of those within one tick. While seeking and settling, the blend now holds the newest pose.
- **The stage kept moving while paused.** Restoring a saved point puts global time dilation back to 1, and the mod only wrote it on a change. It's now written every tick unless it's meant to be 1. Llamas, the crowd and cloth then freeze.
- A saved point's transition moves the fight on by up to about 7 frames, and a round start's by about 177 (its intro). A seek inside those would overshoot, so seeks use a point at least 17 frames back and skip round starts. Round buttons use the round start and land after the intro.

- **Cloth ran at full speed while the fight was slowed or stopped.** Its solver settles once per simulated frame, whatever the time step. `SuspendClothingSimulation` on each mesh didn't stop it on every costume, and neither did `GlobalAnimRateScale`. `p.ClothPhysics 0` does, and cloth then holds its last pose. It's switched on only for frames where the fight took a step, below 0.75x, while paused and while recording. `p.RigidBodyNode.EnableSimulation` made no visible difference.
- Plant wind keeps moving while paused, even with the world's time dilation at 0.0001. Not fixed.

## Fog (measured)
- Past 60 m (`VolumetricFogDistance` 6000) only the plain height fog applies, and its `SkyAtmosphereAmbientContributionColorScale` of 1 feeds the sky's blue into it. That's the haze. Black needs that at 0 as well as `FogInscatteringColor`, `FogInscatteringLuminance`, the directional colours and the cubemap. No setter was found for the scale, so these are written directly and `SetFogDensity` to a different value and back makes the engine read them.
- `SetFogInscatteringColor` only changes the luminance field here; the older colour field stayed at 0.447.
- The white mist is Niagara particles on `NiagaraActor` and `PolarisNiagaraActor`. Those are what a fog colour hides.

## Replays in the game
- Offline, the only way into a replay is My Replays & Tips or Downloaded Replays, both with the tips UI. Tips pause the replay until set to Hide in its menu (B).
- Replays made on an older game version don't play at all.
- The list screen builds once on entry.

## Rendering facts (measured)
- The 59 fps counter in the corner is the game's own `PolarisUMGFpsDisplay` widget, not Steam's. It's hidden with the HUD.
- `r.VolumetricFog` is 0 by default, and plain exponential fog on these stages only blacks out the sky. Fog needs volumetric on. Volumetric fog takes its colour from the stage's lights; with albedo black and emissive of about `200000 * density` it's a chosen colour.
- Stage lights have intensities of 3e7 to 1e8. A spawned point light at 5e5 was invisible and looked like a bug. At 1e8 it lights the floor and fighters. A light added as a component of a fighter's actor stayed invisible (not fully proved why; a separate actor works).
- With stage components hidden and show flag word 4 bit 18 off, the background is about RGB (6,10,11), not exactly black. The greenscreen is a flood fill from the picture's edges under a threshold.
- The game's normal camera lens is 65 degrees. It zooms to 35 to 41 for big hits, and goes up to 100 with the camera close in for some moves, with no drama actor playing. Round intros and ends are drama. `DramaManager.GetCurrentDramaActor` stays non-null: the sequence player's `IsPlaying` is what counts. `BP_PolarisDramaActor_C.IsPlaying` crashes in the blueprint when no player exists.
- Destroying `PolarisEffectManager` or `PolarisDramaManager` crashes the game later, for instance when a rewind fires its delegates. Niagara components are hidden instead.

## Input (measured)
- The game reads its replay keys below window messages. `SendInput` with a scan code works; window messages don't. Rewinding doesn't need that now.
- The engine answers `WM_NCHITTEST` with title bar and maximize zones over parts of its picture, which swallowed clicks on the right side of the editor and brought up Windows' Snap menu. The editor returns `HTCLIENT` while open.
- Window messages arrive on the game thread and Present runs on another one, so messages are queued for the render thread. Calling ImGui's handler directly raced with it.

## Crashes found and fixed in the game
1. Walking GObjects during a level load: a slot's class pointer can be garbage (read of 0x4880). `ClassChainIsSound` checks under SEH.
2. Destroying the effect or drama manager (above).
3. Calling `IsPlaying` on the drama actor (above).

## Tested
In the real game, on replays, through real mouse and keyboard input where it says UI:
- Editor layout, tabs, ruler rewind, preview and record buttons, export and load (UI). Insert, F5, F6, F8, F9, `.`, `[`, `]`, K (real key events). F7 through its button's function.
- Timeline, seek accuracy, determinism.
- Free camera, attach orbit, FOV, dolly in all three styles with turns past 180, speed ramp, motion blur, streams at once with equal frame counts, green and blue screen, stage only, fighters on black, depth, round buttons (UI).
- Fog with colour, sun, spawned light, HUD hide, hit effect hiding, DOF, post process, camera filter on real mid-round cinematic moves and zooms, classic camera.
- Audio pass: real WASAPI capture. On this PC the game's Volume Mixer slider is at 0, which silences it, so it was verified at 2% and put back.
- Launcher: session start, block when online, normal game, cleanup after a forced kill, skin in and out.
- Unit tests (156 checks), `tools/harness/run_all.ps1`, all pass.

## Not verified
- A rage art itself: 4 replays had none that showed up in the scan. The filter caught the moves it had.
- "No rage art screen effects" against a rage art.
- Export and load between two PCs or accounts.

## Gotchas
1. Harness "hang" was the harness: it finished its frames before a late-starting recording could stop, and stopping needs a Present call. A running game never hits this.
2. Steam's manifest `BytesDownloaded` isn't updated during a download. `content_log.txt` has the rates.
3. Shipping builds strip `ShowFlag.*` console commands, so show flags are written to the viewport's bitfield.
4. Text-mode `ofstream` writes CRLF; read back in binary, the build ID in `vault.txt` kept a stray `\r`. A unit test caught it.
5. Restarting Steam with its offline flag set shows "Who's playing?", and picking the account signs in online and resets the flag. Use the Steam menu's Go Offline.
6. `tools/session.ps1 start` writes `test_commands` and `trace_saves` into `tekken8mvm\tekken8mvm.ini` in the game folder. The launcher keeps that file, so delete it after testing.
7. In the Replay screen the back key is K, and it can miss a press. Screenshot before clicking in the next screen: a blind click on the replay list opens a box with Delete Replay in it.
8. A mod-paused game also freezes the game's menus, since they run inside the tick.

## Restore
The launcher closes a session by removing `dinput8.dll` and `Paks/~tekken8mvm`. Manually: delete those two and `tekken8mvm/session.lock`.
Save backups are in `~/.universal-modder/backups/tekken8-saved/`: `um backup restore tekken8-saved --snapshot <full path>`.
