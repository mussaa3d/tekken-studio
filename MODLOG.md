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
- **Plants swayed while paused.** Their materials run on the world's real-time clock, which time dilation doesn't touch, and wind sources (`SetSpeed`, `SetStrength`) make no difference. The clock has no property, so it's found by looking in the world for the value `GetRealTimeSeconds` reports (+0x6B8 on this build), and each tick takes back the part of the frame the fight didn't get. Measured over 3 s paused with anti-aliasing off (`tools/test_stillness.ps1`): 15.1% of the picture changed with the clock running normally, 0.4% with it slowed.
- With TAA on (`r.AntiAliasingMethod` 2) about 8% of the picture still changes on a still scene, on the edges of masked leaves. `r.TemporalAASamples 1` and lower `r.TemporalAACurrentFrameWeight` didn't help.

- **Jackets, straps and capes are Catwalk cloth** (`CatwalkClothComponent`, a plugin), not UE cloth: fighter meshes have no UE cloth results at all. It ignores world time dilation and `p.ClothPhysics`, which explains every failed attempt before this. `CatwalkClothComponentBase` has `CwcTimeDilation` per component, `bEnableSimulation` and `ResetClothVertices`. Setting the dilation to the fight's speed each frame and resetting at the start of a seek's run-up: 3% of a paused greenscreen picture changing fell to 0.075%, and three rewinds to one frame differ by 0.00% (pixels off by more than 25/255). 13 pieces on the Jin vs Bryan stage.
- **At slow timescales cloth stuttered and came off the body.** Measured from a user's 720-frame capture and live at 0.023x. Very short solves break the solver itself: Jin's strap kinks sideways and floats, and panels shake. Frame-to-frame big-change pixels on Jin were 26 per 10000, against 10 with cloth at full speed, even though full-speed cloth moves far more. Saving slowed time and solving in slices of fight time (`ClothMinSolve`): 1/3840, 1/1920 and 1/960 s still kink or zig-zag; 1/480, 1/240, 1/120 and 1/60 all hang like the game's own, at about 10. 1/480 is the default because it gives the smallest hops, and it holds at 0.01x too. Not a perfect glide: it's a hop about every 5 frames at 0.023x and 12 at 0.01x. A real glide needs interpolating the solver's output.
- **Catwalk solves on the GPU, so there's no CPU result to interpolate.** The game ships `/Plugin/CatwalkCloth/Private/CwClothStepSimulation.usf`, `CwClothWriteBack.usf`, `CwClothUpdateDynamicData.usf`, `CwClothInternalSkinning.usf`, `CwClothUpdateRestStates.usf` and `CwClothInterCollision.usf`, under `Plugins/SkinHook`. `CatwalkClothComponentBase`'s parent is `ActorComponent`, so it isn't a mesh. Hashing 9 GB of writable memory around single solves of one piece found no moving vertex array at the fighters' height. It didn't cover write-combined memory, so that part is evidence, not proof. Interpolating would mean hooking D3D12 compute dispatches, finding the buffers `CwClothWriteBack` writes and blending them with our own pass, which breaks more easily on a game patch.
- **Why the hops show:** between solves cloth doesn't follow the body. At 0.01x with the default slices, frame-to-frame big-change pixels ran 8 to 9 with solves at 23 to 36 every 13th frame while the body moved, against 0 between solves and 1 to 8 at solves while it stood still.
- Tried against the hops, none good enough: `CatwalkCloth.RelativeDamping 1` (typical 20.2), `MaxAdaptiveIterationRatio 40` (17.9), `SubstepThreshold 0.0001` (17.3), all with 1/3840 solves, against 16.8 for 1/3840 alone and 11.2 for 1/480, and the strap still kinks. A solve of at least 1/480 on every frame (cloth running faster than the fight) was worse: typical 24.8 against 19.1 at 0.023x and 11.0 against 5.1 at 0.01x, with pulses of 55 to 63 that decay over about 10 frames. At 0.1x the two measure about the same (106 and 109).
- Ruled out for that stutter, measured: body pose and actor position (a per-frame trace shows them continuous through steps), blending the local bone pose too, skipping solves on step frames, the `Cancel*Threshold` limits set huge, exact frames, the FPS cap, `SubstepThreshold` and adaptive iterations. Nothing inside the cloth component's first 0xC00 bytes changes on steps. Rewinds still match after the fix: 0.2 to 0.7% between seeks. Reaching a moment by playing at different speeds differs by 15 to 26%, the same before and after, since cloth depends on its history.
- Fighter physics bones are on their own meshes (`_har`, `_fac`, `_bdf`), so pose blending now covers all 12 meshes the fighters own, not only the bodies.
- **The same moment gave different cloth each time.** Fixed fight frame, reached three ways: Bryan's hem and Jin's ribbon were in visibly different places, 14% of a greenscreen picture different. The solver's result depends on frame length. The engine's `bUseFixedFrameRate` with `FixedFrameRate` 60 gives exact frames; a seek's last 60 steps run at one per frame after `ForceClothNextUpdateTeleportAndReset`. Measured on the greenscreen look with AA off (`tools/test_cloth.ps1`), pixels off by more than 25/255: 0.32% between two seeks from different places. Stepping onto a tick at 60, 250 and 500 ms gaps: 0.9 to 2%.
- Raising the fighters' `CustomTimeDilation` took frames from 60 to 0.2 fps. Don't.
- Walking the object list from the camera hook during a level load crashed the game (access violation reading 0x7). Only do it in a running fight.
- `PolarisUMGReplayList.NavigateToButton`, `callall` on a default object and the like are test-only hazards: a wrong argument is a fatal error.

## Changing replay (measured)
- No reflected function plays a replay by index. `PolarisUMGReplayList.NavigateToButton` called with a guessed argument crashed the game with a fatal error, so the screens are driven with key presses.
- The match-end saved point (type 2) leads to `WBP_UI_Dialog_C`: Play Again, Return to Replay List, Return to Main Menu, then a question that starts on No. `WBP_UI_Replay_List_C` is the list.
- Rows are newest slot first within a tab: downloaded slots 106 down to 102 were rows 1 to 5. Not by date. Only tested on that tab under "Sort by: New".
- At the match end the controller's transition flag stays set, which held a seek open forever until it got a limit.

## Fighter POV (measured)
- The head bone is `Head`. Its +X points up out of the skull and -Y out of the face, so the camera needs a turn of yaw -90, roll 90. Checked on Jin and Bryan.
- `HideBoneByName` on all the fighter's skinned meshes hides the head and hair: 6 meshes on Bryan.

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

- **The editor owns the keyboard.** Open, the window procedure swallows every key and the mod reads its own (Space, arrows, `,` `.`, Home, F-keys). Closed, everything goes to the game and only Insert is read; the free camera no longer flies or takes the mouse with the editor closed. Checked in a replay: B (the game's replay menu) did nothing with the editor open, and Escape paused the game with it closed.
- **Pak mods switch at runtime.** The pak manager's mount and unmount delegate handlers are found from their log strings (`Mounting pak file: %s`, `Unmounting pak file: %s`, UTF-16), and the object from the mount delegate, which keeps the object pointer just before the method pointer on the heap. Calls take an FString of the path as the engine mounted it, `../../../Polaris/Content/Paks/mods/mods/<name>.pak`. Unmount and mount both returned success and the game kept running. Loaded assets stay: UI textures survive switching replays, so a HUD mod only changes after a restart. Switched-off mods are listed in `pak_mods_off.txt` and unmounted on the first tick, before the first fight. The user's two mods made no visible difference in the test replay either way (SSNTB guide mounts under `Tekken8Polaris/`, which isn't this game's folder), so a visible change is unconfirmed.
- **Hit labels** ("Counter" and the like) are drawn by `WBP_UI_Practice_Info_*`, the same widgets as the replay's frame data panel; `WBP_UI_GameMessage_C` and `WBP_UI_Practice_History*` (the input list) are not them. Widget.IsVisible reports true for many off-screen widgets, so finding them took hiding candidates one at a time and recording.
- **Effects on black:** hit sparks are pooled Niagara components with no owner, or on `PolarisNiagaraActor`; the stage's clouds and mist are 108 Niagara components on plain `NiagaraActor`. Bryan's weapon comes through too, so it's a particle system as well.
- The replay switcher sometimes lands its Yes while the end-replay question is still opening and stops there. It now confirms again when the question is still up after 2.5 s. Not re-run since.

- **TekkenOverlay's display settings and simplified stages, rebuilt.** TekkenOverlay ships no source, only a closed `Tekken8Overlay.dll`; its feature list came from the class names in it (DisplaySettings, PerformanceStagesGateway, FOVChanger, EngineFlags, NetworkLag and so on). Network lag changes online play and was left out, frame data wasn't wanted.
- **The fighters' light is mostly not lights.** 147 light components on Ancient Ruins: 6 spot lights on the fighters' lighting channel with no owner, 9 spot lights on `BP_CharacterLightController_C` (3 of them on the stage's channel), the rest stage lights. Hiding the spots or recolouring them changed Jin's jacket by under 1 (red minus green). The controller drives the fighters' materials: writing `CharacterMainLightColorScaleExt` (FLinearColor) and calling its `UpdateCharactermainLightColorScale` and `UpdateCharacterMaterialMPC` tinted only the fighters (red minus green 17 to 48); 0 darkened Jin from 125 to 104 with the sky unchanged. Writing `CharacterMainLightColorScale` or the env scale without those calls did nothing. `LightingChannels` is on LightComponent, not LightComponentBase.
- **Plain stage crashed the game when switched off,** an access violation in SetMaterial: the stage's own materials, once swapped for WorldGridMaterial, were garbage collected, and putting them back used freed memory. They're now held in the engine's root set (bit 30 of the FUObjectItem flags) until they're back. Survived a 75 s hold and switching off.
- **Aspect ratio** comes from changing `AspectRatio` (+0x48) and `bConstrainAspectRatio` (+0x4C) in the FMinimalViewInfo the projection hook already sees. 2.39 letterboxes the picture and the recording.
- PostProcessSettings `IndirectLightingIntensity` was tried for a bounce-light slider: 0.5, 1 and 2 looked the same and 0 turned the picture white, so it isn't in.

- **A and D reached the game with the editor open** because it reads held keys through DirectInput (`GetDeviceState`, 256 bytes), not window messages. The keyboard's GetDeviceState and GetDeviceData are hooked, and the game's imports of GetAsyncKeyState and GetKeyState are patched, all answering "nothing pressed" while the editor is open. Inline-hooking the user32 functions was dropped.
- **GetAsyncKeyState inside the game's process returned nothing** in tests on 8 November, while another process saw the same key. The editor's keys now also follow WM_KEYDOWN and WM_KEYUP from the window procedure.
- **The true start of a replay:** the seek to frame 0 used to run through the intro. It now ends as soon as the jump is taken, so the intro plays at normal speed (clock 1.1 to 6.3 in 5 s).
- **The replay's display settings** (15 rows under B) have no reflected setter found. `PolarisUMGPractice.SetVisibility*` crashed the game with a null read when called from outside. A memory diff around flipping a row found nothing that held up. So the tool drives the menu: rows are 34 apart from y 137 in the 1456x819 frame, a row's click opens its list with choices 34 apart from the row, and a choice is taken by moving onto it and tapping for 40 ms (0 ms and 120 ms close the list without choosing).
- **The game's slow motion** is world time dilation (0.1 on some hits, 0.01 for 90 frames at a KO) plus two floats multiplied by GetGameSpeedTotal, at +0x1570 on the object the `GameSpeedOwner` signature finds. Holding both at 1 keeps them there through a KO, measured. The fighters still hold their pose, so that part is the fight's own.
- `PolarisLightOptionSubsystem.EffectBrightness` reads 1 for the Medium effect saturation, but writing it changes nothing on screen; it isn't what the renderer reads.

