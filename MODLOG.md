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
- No reflected function plays a replay by index. `PolarisUMGReplayList.NavigateToButton` called with a guessed argument crashed the game with a fatal error, so the screens were driven with key presses. Since 13 November they're driven by the calls their Blueprints make when clicked: see "The game's replay screens by direct calls" below.
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
- **The game's slow motion** is world time dilation (0.1 on some hits, 0.01 for 90 frames at a KO) plus two floats multiplied by GetGameSpeedTotal, at +0x1570 on the object the `GameSpeedOwner` signature finds. Holding those at 1 changes nothing you can see: the fight sim itself slows down, with PlayerMotionFrame moving 0.0098 per step (about one frame over the 90 steps), measured the same both ways. The option now reads those floats only as the signal and runs the steps at once.
- **Replay panels by direct call:** `SetVisibilityHistory`, `HpInfo`, `ComboInfo`, `DamageInfo`, `FrameInfo` and `FrameSimpleInfo` take (side, visible), and only work looked up on the live widget's own class (`WBP_UI_Practice_S2_C`, from `PolarisUMGPractice` objects). On the C++ class they fault, since only the Blueprint implements them. Hidden stays hidden through play and a round change. A hide does nothing unless the same call showed the panel first, because the game's own setting doesn't go through these functions, so each hide calls show then hide in one tick. Not offered: the version label (`WBP_UI_HUD_Version_C` is put back every tick during play), `SetVisibilityComboChallenge` (a placeholder panel that a hide didn't reliably clear), the tips (hiding `SetVisibilityTips` or calling `SetHiddenTips` leaves the replay paused on the invisible tip, so it looks stuck until K), Hit Properties (no function found).
- **In-battle status:** the text lives in `WBP_UI_Practice_Info_L_S2_C` and `Info_R_S2_C`, as `Border_Combo` (and `_P1`), `Border_Damage`, `Border_Counter`, `Border_Clean`, `Border_Punish` and `Border_Tornado`. The game shows them through `OnComboIn`, `OnCounterHitIn` and so on, and hiding one border with SetVisibility leaves the others. The practice widget holds the other panels too, but those are UserWidgets of their own, so the HUD hide can skip just these three classes.
- **The sky** (9 November, stage ST12): two `SM_SkyDome` meshes whose materials come down from `M_ENV_HDRI_Sky`, which reads a panorama from the texture parameter `HDR_Map`. `KismetRenderingLibrary.ImportFileAsTexture2D` reads an `.hdr` or `.png` from disk at runtime (1024x512 both came back as Texture2D). A dynamic instance made with `PrimitiveComponent.CreateDynamicMaterialInstance` and `SetTextureParameterValue("HDR_Map")` shows it. The stage's own tuning has to come out or it looks like a flat tan haze: `SkyTile_U`/`SkyTile_V` 1 (the stage had V 1.95), `Contrast` 1, `Saturation` 0, `Atomosphere` 0, `UVnoiseMult` 0, `CloudIntensity` 0, `SkyTint` white. `Intensity` (16000 here) scales brightness. The material's own `RotationAngle` did nothing visible, so the sky is turned by rotating the dome.
- **Turning the sun or the dome** does nothing until the component is made Movable (`SetMobility(2)`): the sun is Stationary and the domes Static, and `K2_SetWorldRotation` on them returned with the rotation unchanged. After that the shadows move. No frame rate cost measured (58 to 59 fps either way).
- **The sky light** is `SourceType` 1 (a cubemap made for the stage). Writing 0 (captured scene) and calling `RecaptureSky` lights the stage from the new sky: a dim picture darkened the stage. `SetSourceCubemapAngle` turns the stage's own cubemap with the sky.
- **Hiding a fighter:** `Actor.SetActorHiddenInGame` on the fighter hides the body, what it wears and its shadow, and showing it again works.
- **Look keys** reuse the preset writer: a key is the look's key=value text without `time.`, `rec.`, `stream.` and `camera.`, two keys blend number by number for sliders and colours, and the reader only applies what changed, so it can run every frame.
- **Seen once, not looked into:** `switch` (Play this replay) pressed its menu keys 0.6 s after starting, while the replay was still playing, as if the end dialog was already up, and timed out. The replay went back to an earlier round and nothing else changed.
- `PolarisLightOptionSubsystem.EffectBrightness` reads 1 for the Medium effect saturation, but writing it changes nothing on screen; it isn't what the renderer reads.

## 2.0: actors, the scene tab, picture sizes, ReShade (10 November, measured unless it says otherwise)

### Actors
- **A spawned `SkeletalMeshActor` goes through the bone hook.** `PerformAnimationProcessing` is called for its mesh component every frame, with no anim instance on it. The clone's `VisibilityBasedAnimTickOption` is set to 0 (always); it wasn't tried without. Whatever the hook writes into the component-space bones is what's drawn. So an actor's pose is written natively in the hook, with no engine call per bone: a take's two nearest frames blended, the same way the fighters' own poses are blended between steps.
- **A fighter's body is one mesh**, put together by the game when the fight loads (Jin and King both). It has the head, hair and clothes in it. The game gives that mesh a new name in every fight, so a take can't find it by name: a take made in one fight found nothing in the next, in the same replay. Takes go by the names of the mesh's bones in order (a hash of them), which was the same across three game sessions for Jin.
- **Of the 12 fighter meshes the pose blending follows, only the two bodies pass the bone hook.** The others (5 per fighter show up as components) never do, so a take holds one mesh per fighter. A clone of the body alone looks whole.
- A clone is the body's mesh asset (`SkinnedAsset`) set with `SetSkinnedAssetAndUpdate` on a `SkeletalMeshActor`'s component, with the fighter's own materials (`GetMaterial` and `SetMaterial` per slot, so it shares the fighter's material instances and the character light controller tints it too) and its lighting channels. `SetSkeletalMesh` isn't on this build.
- A take is recorded with the recording clock on at 60, so each rendered frame is exactly one game step and none is missed: 26 to 29 s gave 180 and 181 frames on two runs. The poses are the step's own (the blend's target), and where the fighter stood is the target `SetActorLocationAndRotation` was given, not what's on screen between steps.
- Placing: the mesh component against the fighter's actor is kept from the first frame, and the clone goes at `MeshRel * (Actor(f) * Actor(0)^-1 * Placement)`. With the placement equal to the first frame, the clone stood exactly on the hidden fighter through the take (same pose, same spot, compared side by side with the real one hidden and shown).
- **Cloth:** Catwalk cloth is a component on the fighter, so a clone has none. The body's cloth panels still draw, skinned to their bones: King's cape streams out in the pose it was skinned to, and Jin's jacket tail and strap sit differently from the real one's.
- Hiding the real fighter with `SetActorHiddenInGame` doesn't hide clones, which are actors of their own.
- Leaving a replay and starting another with two actors, three props, an effect and a light on the stage: no crash. Outside a fight nothing is asked of the engine. The next fight found destroys what's still alive and builds again.
- **At the first tick of a fight the stage isn't all there.** A mesh looked up by name wasn't loaded yet and was found 5.4 s later, so a prop or an effect that isn't found is looked for again every 120 ticks, 8 times. An actor is tried again every 120 ticks for as long as it's on.

### Props and effects
- `StaticMeshActor` spawned through `BeginDeferredActorSpawnFromClass`, its component made Movable, `SetStaticMesh`, `SetActorEnableCollision(false)`. 898 static meshes were loaded on Ancient Ruins and 265 Niagara systems. A mesh comes at the size it was modelled at: the alpaca is several metres tall at 1 and the stage uses it at about 0.25.
- **Loading by path works:** `KismetSystemLibrary.MakeSoftObjectPath` (FString in, the path out at +16), `Conv_SoftObjPathToSoftObjRef` (out at +32), `LoadAsset_Blocking` (out at +48). The alpaca's path loaded on the Pac-Man stage, where it wasn't loaded, and a path that doesn't exist came back empty with no trouble. The sizes of the pieces are read from the functions (`ParmsSize` at +0xB6), and nothing is called if they don't add up.
- An object's full path is its outers' names joined, the outermost being the package (`/Game/...`), then a dot.
- `NiagaraFunctionLibrary.SpawnSystemAtLocation`: world +0, system +8, location +16, rotation +40, scale +64, auto destroy +88, auto activate +89, pooling +90, return +96. Spawned with no auto destroy, the component's owner is not one of the fight's, so the Effects on black look is told about it.
- `NiagaraComponent.SetAgeUpdateMode(1)` and `SetDesiredAge` make an effect's age follow a number. Going back makes it start over and run up to the age. The same age twice gave 0.006% of the picture different at 0.5 s and 0.67% at 2.0 s on `NS_EF_ForceShockWaveSmokeXL_Floor`, against 1.9% with the effect on and off: its particles are random, so it's the same age but not the same sparks.
- `NS_EF_Ant_BodyAura_00` placed in the open showed nothing. Auras want something to sit on.

### Picture size
- **`r.SetRes 3840x2160w` gave 3840 x 1087** on two 1920 x 1080 monitors side by side (the second is 7 pixels lower): the engine keeps the size inside the desktop. Windows would too (max track size 3860 x 1107) unless the window answers `WM_GETMINMAXINFO` with more.
- **`SetWindowPos` on the game's window, with that answer, gives any size**, and the engine follows the window: the back buffer was 3840 x 2160, format 24, and the game ran at 59 fps. A crop of the frame at full size has real detail in it (the print on King's cape).
- Only a window can be any size. From borderless the mod asks for `r.SetRes <its size>w` first, waits for the title bar, sizes it, and goes back with `wf` afterwards: recorded at 2560 x 1440 and back to 0,0 to 1920,1080. Fullscreen takes the same path and wasn't run.
- On the second monitor (window at -1928,-24): 2560 x 1440, depth read from a 2048 x 1152 buffer, written at 1280 x 720 through ffmpeg's `scale` with lanczos, picture and depth both 31 frames, and the window back at -1928,-24.
- The recorder holds time until the back buffer is the size asked for (600 frames at most, then it stops with the reason), so the first frame is still the one asked for. A range of 29 to 30 s gave 62 frames at 4K and at the window's own size alike.

### Look keys
- A key made before a light, prop, effect or actor existed used to switch it off as soon as the clock moved, since a key meant the whole look. Keys now leave alone what they don't mention, and a slot that's switched off is written as off so a key can still switch it off. Presets and shots are still the whole look.
- Orbit yaw around a bone is counted past 180 now, like the free camera's, so two keys can hold a full turn.

### ReShade
- Not installed on this PC, so nothing here is measured against it. Read from its 6.8 headers: an add-on registers with `ReShadeRegisterAddon(module, api version)` and events with `ReShadeRegisterEvent`, both found by looking through the process's modules for the exports. `reshade_present` comes after ReShade's effects and its own menu, and its drawing for the frame is still in its immediate command list then, so that's flushed before the frame is copied.
- The mod doesn't use `reshade.hpp`'s own `register_addon`, which wants ReShade's Dear ImGui to be the same version as the mod's.
- Tested against a stand-in (`tools/harness/fake_reshade.cpp`) built on the same headers, so its objects have the real layout: it refuses API 20 and 19 and takes 18, lists two techniques and six values (one filled in by ReShade itself and one in an effect that's off, which the mod leaves out), and the harness calls its present once a frame.

### Gotchas
9. A fighter's body mesh has a different name in every fight (above). Anything kept between fights has to go by something else.
10. The compiler stops at 128 nested blocks, and a long `else if` chain counts as nesting (C1061). The test commands are two functions for that.
11. A test that sets the recorder's range and leaves it on starts a recording by itself the next time the clock passes through the range.
12. The replay's own pause (K) and its tips stop the fight under the mod: seeks then end with "the fight isn't moving" and the replay switcher gives up with "The game's screens didn't come up". Press K with the editor closed.
13. Clicks on the game's menus from a script need the pointer there first: click twice, a second apart. The first only moves the highlight.

### Tested in 2.0
In the real game, on two replays (Jin vs Bryan on Ancient Ruins, King vs Nina on the Pac-Man stage), through test commands:
- Takes of Jin and King, actors placed, turned and sized, two actors on one take at different speeds, hold, loop and disappear, an actor against the real fighter at the same moment, across a game restart from the take file and a shot.
- The greenscreen, stage only and depth looks with actors, and a two-stream recording with an actor in both.
- Props by name and by path, three lights with a spot's direction and cone, effects on the timeline at set ages.
- Look keys moving an actor and a prop, a preset and a shot saved, reset and loaded, undo of a move with the editor open.
- Recording at 3840 x 2160, at 3840 x 2160 written as 1920 x 1080 in two streams, at 2560 x 1440 on the second monitor, and from borderless.
- Leaving a replay with everything on stage, and starting another.
- Unit tests (228 checks), the editor's own test (33 checks, 44 with the stand-in ReShade), `tools/harness/run_all.ps1` with its new cases I to L, all pass.

### Not verified in 2.0
- The real ReShade.
- The SCENE tab's and the picture sizes' controls clicked by hand in the game.
- Characters whose body is more than one mesh, a take on another outfit, and a take on the other player in a mirror match.
- Fullscreen as the starting point for a bigger render, sizes above 3840 x 2160, monitors of different sizes or scaling.
- Effects by path, and meshes from a pak mod.

## 2.0: the editor's look, after Iris Studio

The editor is restyled after Iris Studio's (github.com/yerbisonline/IrisStudio, GPL-3.0, release 1.0.1). Nothing it does changed.

- **Colours:** Iris Studio's Mono palette (background 1C1C1C, surface 242424, raised 2E2E2E, border 383838, text D6D6D6, muted 8C8C8C, accent CFCFCF), mapped onto ImGui's colours the way its `UI/Theme.cpp` maps them. ImGui's own style, with the padding set from the font's size.
- **Fonts:** Rubik Regular and SemiBold, with Font Awesome 6 Free Solid merged in at two thirds of the size, built into the DLL as resources (`src/mod/fonts.rc`). Player names in other scripts still come from the Windows fonts.
- **Tabs:** Replays, Camera, Look (was VISUALS), Scene and Record, each an icon over its name, with a line under the open one.
- **Rows:** a control's name is on its left, in a column two fifths of the panel wide, and the control fills the rest (`Row` in `menu.cpp`). Sections are bars with an icon.
- **Picture:** 70% of the width, under a strip with a camera dropdown (Game, Free, Dolly, Attached), with a hint strip along its bottom.
- **Playback bar:** frame back, play or pause and frame on as icons, the time in bold as minutes and seconds with the replay's length when the game gives it, then the timescale and the round buttons.
- **Timeline:** lane names with icons on the left, a ruler in minutes and seconds with small marks, the recording range as a red band when armed, actor bars blue, effect bars amber.

### Tested
- Unit tests (228 checks), the editor's own test with the stand-in ReShade (44 checks), `tools/harness/run_all.ps1`, and the screenshots of every tab in the test program.
- In the real game: the editor opened in a replay, the Replays and Look tabs clicked with the mouse, the time shown as 0:21.03 / 3:32.77.

### Not verified
- Each of the restyled controls clicked by hand in the real game.
- Iris Studio's key diamond beside each control isn't copied: a look key holds every value at once.
## 2.0: fixes and a check before publishing

- **Focus distance:** dragging it turns Autofocus off. Autofocus wrote the distance every frame, so the slider did nothing while it was on.
- **Fog start distance:** also sets the volumetric fog's own start distance (`VolumetricFogStartDistance`), which is the fog you see. The plain fog's `StartDistance` alone barely shows on these stages.
- **The editor's top right** reads "tekken8mvm by mussaa3d 2.0".

### Security
Presets, shots, camera paths, actor takes and `.t8replay` files can be passed between people, so what they hold is no longer trusted:
- Names that become file names (presets, shots, paths, takes, replays, the sky picture) must be plain names: no folder, no drive, no `..`, no network share (`mod::SafeFileName` in `src/mod/log.h`). Before this a shared preset could name a sky picture or take on another machine's share, and Windows would have sent it your sign-in hash.
- The sky picture is only read from the `hdri` folder, not from a full path.
- A `.t8replay` file's header is checked for a sane size before memory is set aside for it (a made-up size could ask for 4 GB and crash the game), and the replay inside must carry the game's own slot header before it's copied into the game's replay list.
- The shader compiler (`d3dcompiler_47.dll`) is loaded from Windows' own folder only, not from the game folder.
- Snapshot names in the tool can't hold a folder.
- Checked and found fine: no network code in the mod or the tool (the mod only blocks the game's connections when that's on); no bot token, key, user name or full path of the developer's PC in the source, the DLL, the tool or the installer; the test command channel stays off unless `test_commands = 1` is in `tekken8mvm.ini`; the installer needs no admin rights, installs under the user's own AppData, and checks the ffmpeg download against a fixed SHA-256; ffmpeg's command line is built from the tool's own settings and a timestamp, not from presets; ReShade values are bounded; actor take files are read with limits on every count.

### Tested
- Unit tests (239 checks), the editor's own test with the stand-in ReShade (44 checks), `tools/harness/run_all.ps1` (every recording, size, depth, audio and ReShade case).
- In the real game (Tekken 8 3.02.02, a downloaded replay): `VolumetricFogStartDistance` found; thick white fog covered the fighters at start distance 0 and left them and the near floor clear at 1500, with the far trees still in fog. Depth of field with Autofocus Off: focus 150 blurred everything, 3000 made the mountain sharp and the fighters soft. A sky picture given as a network path was refused without being opened.

### Not verified
- Dragging the Focus distance slider by hand in the game (the slider's value was set through the test channel; the switch to Autofocus Off is one line in the editor).
- A replay file exported, sent and loaded on a second PC after the header check.

## 2.1: presets, older replays, the 3.03.01 update and early access

### What changed
- HUD presets: a dropdown in HUD and screen. The match HUD is `WBP_UI_HUD_S2`, which holds the health bars (`HUD_Gauge_S2`); those hold the names, portraits and badges (`HUD_Player_S2`). The gold frame is `HUD_GaugeCustom` and the clock is `HUD_Timer`. A preset names the widget classes it keeps (the parents too), and every other HUD widget is hidden one by one, so Fragmovie leaves only the bars, the timer and the hit combo and damage text.
- Graphics presets: a dropdown in Quality from All low to Extreme. It sets the Quality group and the engine's bloom, motion blur, depth of field quality and resolution scale.
- Older replays: a replay slot holds its game version twice, at +8 and +12, and the game checks them before it plays ("The game version has been updated. Any replays saved in the previous version can no longer be viewed"). A background thread marks every older replay with the newest version seen. That version is remembered in `replay_version.txt`, in case the list holds only older replays. Importing a `.t8replay` from an older version marks it the same way. This can be switched off in the Replays tab, or with `older_replays = 0` in tekken8mvm.ini.
- Early access: `src/common/licence.h` checks a key (ECDSA P-256 over SHA-256, through Windows' BCrypt) against the public key built into the tool. The launcher shows a key screen until a valid key is saved in `%APPDATA%\tekken8mvm\early_access.key`. The mod stays off without one. The bot in `bot/` signs keys with the private key, which stays in `bot/secrets` and is never committed or shipped.
- Stage only now has nothing of the fight left in it. Things that were still there:
  - Roger's sign and a hat that's been knocked off. The game shows these again every frame, so hiding them didn't stick. Hidden components are now also set to draw only in scene captures.
  - Hit effects from Unreal's pool of reused particle systems, which belong to the world settings.
  - The afterimages a fast move leaves (`PolarisTraceMeshActor`).
  - Character props the game spawns on their own: `SK_CH_`/`SM_CH_` meshes, and the scenery a rage art brings in.
  The greenscreen and hit-effects looks use the same rule, so these now count as part of the fight there too.
- Recording with several streams changes the look several times a frame. Working out each component's side again every time made a five-stream recording crawl along at 0.5 frames a second. The answer is now cached per component (and cleared at each new fight), which brings it to 2.3 frames a second.
- Early access by Discord role instead of keys (`src/common/discordlink.h`). **Link Discord** in the tool opens Discord's sign-in page (OAuth2 implicit grant, scopes `identify guilds.members.read`, return address `http://127.0.0.1:53682/discord`, registered in the developer portal). A small page on that address passes the token to the tool with `fetch`, after clearing it from the address bar so it doesn't stay in the browser history. The tool asks Discord every minute whether the person is in the server with the Early Access role, and writes each good answer to `%APPDATA%\tekken8mvm\discord_link.bin`, sealed with DPAPI for this Windows user. The mod can't ask Discord itself (it blocks the game's internet), so it reads that file every minute and turns off when the last good answer is over 3 minutes old. Owner keys from the old system still unlock both.
- The bot (now on a Raspberry Pi as a user service) no longer hands out keys. Patreon's or Ko-fi's own Discord connection gives the role. New: `#get-access`, `/donate`, `/give` (role plus installer), `/downloads`; `/download` sends the newest GitHub release. `bot/post_guide.py` posts a nine-step guide with GIFs, one channel per step.
- Updates from GitHub (`src/launcher/updater.h`). Releases go to the public repo `mussaa3d/tekken8mvm-releases`, installers only, each with a `.sha256` (`tools/publish_release.ps1`). Once unlocked, the tool asks GitHub for the latest release; if it's newer, **Update now** downloads it, checks the SHA-256, runs it with `/SILENT` and closes. The installer starts the tool again after a silent install.
- A logo in the style of the Tekken 8 logo (drawn, not traced): `installer/logo/make_svg.py` and `render.py` make the app icon, the installer's pictures, and the PNGs for GitHub and the bot's avatar.
- Version 2.1.

### Tested
- Link Discord, on this PC with a fresh profile: the browser came back saying Linked, Discord confirmed the role and the tool unlocked; after the fix the address bar showed no token.
- Updating: a test copy labelled 1.0 showed "tekken8mvm 2.1 is out (you have 1.0)"; Update now closed it, installed 2.1 with no setup pages and opened 2.1 again within about 5 seconds.
- Free camera while making the guide: flying, looking, camera keys and the dolly path all worked once the replay was running. A Tips replay that the game itself has paused (it does after a jump in time) doesn't tick, so the free camera doesn't move until the game's own play key resumes it.
- Unit tests: 273 checks, including the Discord answer reading, the sealed file, version comparison and the GitHub reply. Before that: 249 checks, including 10 for the key check (a real key that has run out, a changed name or date, a changed signature, half a key, junk and a very long key). `tools/harness/run_all.ps1`: no FAIL; ReShade editor test 44/0; audio PASS.
- The launcher with no key shows the key screen. With a key it opens as before.
- In the real game, on 3.03.01 with Steam offline: every hook was found, and the online guard reported Steam offline. Each HUD preset was checked on a replay. Fragmovie kept the bars, the timer and the combo and damage text, and hid Counter and Punish. Broadcast kept the names, the frame and the round wins. Clean and Combo text only did as named, and The game's own brought it all back.
- In the real game, five streams in one pass (picture with an HDRI sky, greenscreen, stage only, depth, hit effects only) for a whole round: 2148 frames each. Stage only had no sign, hats or hit effects; the hit-effects stream had the sparks, rings and dust.
- In the real game, older replays: a 3.2.2 replay was refused by the game, then played after the version was marked. A 3.2.2 replay and one of the user's own March replays played after the automatic marking, with no test command. 105 older replays were marked.

### Not verified
- Whether an older replay plays out exactly as the original match. It's the recorded inputs played through 3.03.01's moves, so a move a patch changed would make it differ, and nothing here can detect that.
- The graphics presets in the real game. They set the same sliders as before, which were tested earlier.
- Linking an account without the Early Access role, and the mod turning itself off in the game when the tool is closed.
- Patreon's and Ko-fi's role connections: they're set up on their sites, not here.
- In the editor, Space did nothing when sent by a script after a button had been clicked; the play button works. A real keyboard wasn't tried.
- If the game saves a group of replays while they're marked (it does when you download or delete one), it writes the new version into those files.

## 2.1: the game's replay screens by direct calls (13 November, measured on 3.03.01)

### What changed
- **Play this replay makes the game's own calls**, not key presses: from inside a replay, seek to the end, Return to Replay List, Yes, the replay's tab, its row, Play Replay, LEFT for the tips. 15 s from inside a replay, 9 s from the list, with the game in the background. If the replay's box is already open on the chosen replay it presses Play there.
- **Export takes the replay that's playing** (Export this replay in the Replays tab), or the one picked in the list when none is. With no name the file is named after the date and players, like `2026-03-23 03-24-33 <P1> vs <P2>.t8replay`. File names are UTF-8 now: an Arabic player name used to go through the PC's code page.
- **Export Replay in the game's own list:** the box a row opens gets Export Replay in place of the greyed-out Show P2 Profile. It saves that replay, then says Exported, with a message over the game for 5 s. A switch in the Replays tab turns it off.
- The ImGui ID clash between the Here buttons of Light 1 and Actor 1 (both drawn under `PushID(i)`) is gone: each Scene group has its own ID range.

### How the screens work (measured)
- `src/mod/events.cpp` hooks `ProcessEvent` once and passes events on; the event tracer uses it too (`events 1 word,word` picks the functions logged with their parameters). Native code tells the screens what to show through Blueprint events, which go through it with their parameters. Blueprint calls into native code don't.
- The list (`WBP_UI_Replay_List_C`, C++ `PolarisUMGReplayList`) is filled in one go when it opens or changes tab: `ClearButton`, then per row `AddButton`, `SetReplayCommonInfo(Index, data_no, favorite, date_text @0x10, ...)`, `SetReplayPlayerInfo(Index, side, 3 textures, player_name_text @0x20, ...)`. All 100 rows at once, not only the visible ones. `date_text` is local time to the second, `03/23/2026 03:24:33`, and the slot's time at +16 is UTC seconds, so a row matches a slot exactly by date. Names don't always match byte for byte (`G U S T A V O`), so they only break ties.
- Rows aren't in slot order: slot 34 was row 93 under Sort by New. The old key-press switcher counted slots, which would have opened the wrong replay.
- `InvokeButtonDecideCallback(ID)` opens a row's box, and its ID is the row's `data_no` (row + 1), not the row. 93 opened row 92. `InvokeTabDecideCallback(ID)`: 0 Online, 1 Tekken Ball, 2 Downloaded, 3 My Replays & Tips.
- The box (`WBP_UI_Replay_DIalog_C`, C++ `PolarisUMGReplayDialog`): `SetReplayCommonInfo(date_text @0, ...)`, `ClearButtons`, `AddButton(Text, is_raw_text, is_enable)` x4, `SelectMenu(0)`, `PlayAnimIn`. Buttons 0 to 3: Play Replay, Show P1 Profile (`TEXT_000_UI_ONLINEREPLAY_00x`, greyed offline), Show P2 Profile (`TEXT_000_UI_ONLINEREPLAY_004`, greyed), Delete Replay. `DecideMenu(ID)` comes back when the game takes a button.
- Question boxes (`WBP_UI_Dialog_C`, C++ `PolarisUMGDialog`): `SetDescription`, `UpdateButton(ID, Text @0x8, is_raw_text @0x18, is_enable @0x19, ...)`, `SelectMenu`, `PlayAnimIn`. Texts are keys: the replay's end menu is `TEXT_000_UI_MYREPLAY_021` to `023` (Play Again, Return to Replay List, Return to Main Menu), the question after it `TEXT_000_UI_COMMON_006`/`007` (Yes, No, starting on No), the tips side `LEFT`/`RIGHT` as raw text, the offline notice's OK `TEXT_000_UI_COMMON_008`.
- **A box acts on the button its cursor is on, whatever ID a decide names.** `InvokeDecideCallback(1)` on the end menu, with the cursor on 0, played the replay again, twice. `InvokeSelectCallback(id)` moves the cursor (the box answers `SelectMenu(id)`), then the decide does what it says. The switcher only decides once the box reports its cursor on that button. A decide 19 ms after the box appeared did nothing useful either; the switcher waits 1 s after each screen appears.
- Every `InvokeDecideCallback` thunk (list tabs, the box, question boxes share code) reads the ID and calls `+0x2E7A700(widget, id)`, which calls the box's callback (a TFunction at +0x290). For the replay box that callback stores the ID at `[scene+0xD4]`, and the scene's update (`+0x5FC90F0`) checks it against its own button list at `[scene+0x2C8]` (24-byte entries, enabled flag at +0x10): an ID past the end is dropped, and a cursor request past the end is clamped to the last button (Delete Replay). So a fifth button can't be added; a greyed-out one is reused.
- Renaming a greyed-out button with `UpdateButton(..., is_enable 1)` isn't enough: it stays unclickable and greyish, and a click on it is taken as Confirm on the chosen button, which played the replay. The box's buttons are made again with its own `ClearButtons` and `AddButton` (ours enabled), then renamed with `UpdateButton` (AddButton only takes text keys: raw text came out as OK), then `SelectMenu` puts the cursor back. Hovering it then gives `SelectMenu(2)` and a click calls `InvokeDecideCallback(2)`, which the hook on `+0x2E7A700` takes for Export before the game's callback sees it.
- `+0x2E7A700` is found from the reflected `InvokeDecideCallback`'s native pointer (UFunction +0xD8): the last direct call in its thunk, walked with MinHook's instruction decoder, and only taken if it starts `40 53 48 83 EC 20 48 8B 99`. Once at startup it wasn't there yet (the decoder came back with nothing), so it's tried every 2 s until it is.
- The open replay: when the box gets `DecideMenu(0)` (Play Replay) its date and players are kept, and when the next fight is found that's the replay playing. The list coming up clears it. Opened by hand or by the switcher, the same.
- The playing replay isn't copied anywhere: no record of it was in writable memory outside the block of slots. Its slot's address was in three places, not followed up, since the box's events already say which replay it is.

### Tested
- In the game, on 3.03.01, Steam offline: Play from the list (slot 34, row 93) and from inside a replay (36 to 35), both landing on the right replay. The in-game Export Replay: hover, click, file written (653,337 bytes of slot), button says Exported, the message showed. Export this replay in the editor during a replay, with an Arabic name in the file name. Play with the chosen replay's box already open pressed Play in it. Actor 1's Here hovered with every Scene group open: no ID warning.

### Not verified
- Downloaded Replays through the switcher (tab 2): the tab call is the same, but no downloaded replay was played this way.
- A list sorted any other way than New, and the game in another language (the date and the keys are what's matched, so neither should matter).
- A replay that a tip has paused: the seek to the end can't move it, and the switcher now stops with a message saying to press K instead of waiting out its time.

## 2.1: early access taken out (15 November)
- The Discord early-access gate is gone: `src/common/discordlink.h`, `src/common/licence.h`, the launcher's sign-in screen and the mod's every-minute check. The tool and the mod run for anyone with the installer. The bot in `bot/` left the repo too.
- The updater now asks the `mussaa3d/tekken8mvm` repo itself for its latest release, and `tools/publish_release.ps1` publishes there. While the repo is private GitHub answers 404 and the tool offers no update.
- Built; unit tests 245 checks, 0 failed (the key and Discord tests went with the code they tested). Not run in the game after this change.

## 0.1 beta: first public numbering (15 November)
- The first release under the new repo is called **v0.1 beta (early access)**: version 0.1 in the exe, the installer (`tekken8mvm-0.1-setup.exe`), the editor's menu bar and the launcher's title. Earlier numbers (1.0 to 2.1) were private builds.
- Releases are not marked as GitHub pre-releases, because the updater asks for `/releases/latest`, which skips those. The title says beta instead.

## 0.1 beta: the dolly path in the picture, after IWXMVM (15 November, measured on 3.03.01)
- Read IWXMVM (github.com/reallyluckyy/IWXMVM) for how its dollycam is edited: the campath drawn in the world with a camera model per node (`Graphics.cpp`), a translate gizmo (local or global axes) and a rotate gizmo with three rings driven by mouse travel, a double-click on the node to cycle them (`GameView.cpp`), and a keyframe editor with a value per channel and arrows to the key before and after the current tick (`KeyframeEditor.cpp`).
- Here it's drawn with ImGui over the picture instead of D3D meshes: `cine::ProjectPoint` takes a world point to the picture with Unreal's horizontal field of view, and the editor draws the path (sampled the way the dolly flies it, in the chosen style), a little camera with the key's lens at each key, a white dot where the dolly is at this moment, and the selected key's handles. Shown when the camera isn't the dolly itself, and never in a recording. View > Camera path in the picture turns it off (`camera.show_path`).
- Handles: arrows along the key's own ahead, right and up, or the stage's X, Y and Z, which move it by as much as the mouse went along the arrow on the picture; rings for pitch, yaw and roll, which turn it by the mouse's travel, as IWXMVM's do. Tool buttons at the picture's top left, with **From the view**.
- Camera tab > Camera keys: arrows to the key before and after this moment, and every number of the picked key (time, place, pitch, yaw, roll, field of view), with From the view, Go there and Delete. The timeline, the picture and the panel share one selection.
- Measured in the game (Jin vs Bryan replay, free camera at -560 -110 3180 looking 28 degrees down, FOV 75): the fighters' reported places project onto the fighters in the picture, and each key's little camera sits where its numbers put it. Dragging the up arrow raised a key from Z 2860 to 2916.6; dragging the yaw ring turned it from 90 to 114; with the replay playing at 0.25x, dragging the Y arrow moved it from -350 to -299.6 and the path redrew as it went. Pressing F5 in the middle of Preview shot gave a free camera at the path's pose at that moment.
- `tools/drive.ps1 drag` holds the left button from one point to another, for testing handles. Unit tests 258 checks, 0 failed (projection and the quaternion turn added).
## 0.1 beta: shared replays load on their own (18 November, measured on 3.03.01)
- Every `.t8replay` in `shared_replays` is put in an empty slot by the mod's 3-second thread once the replay block is found and its used-slot count has held for three checks (the game fills the slots over a few seconds after the block appears: 15 then 90 older replays were marked 3 s apart). Each file is tried once per session, again if it changes.
- `Import` now skips a file whose fight (date played and both names) is already in a slot, so a file loaded before and kept by the game's own saving, or an export of your own replay, isn't added twice. The manual Load button uses the same check.
- `auto_load_replays` in tekken8mvm.ini, on by default, set from the launcher's Replays tab. The launcher's **Add replay files...** (and dropping files on its window) copies files into the shared folder, the same folder the mod uses (`output_dir` from the ini, or Videos\tekken8mvm).
- Measured: with an export of a replay already in the list and a copy of it with the date moved 3 days, the copy went into slot 110 twelve seconds after the block was found and showed as the first row of Downloaded Replays (dated 03/20/2026); the plain export was skipped.

## 0.1 beta: tekken-studio file names and a portable zip (18 November)
- The GitHub repo is now `mussaa3d/tekken-studio` (GitHub forwards the old name), and the updater asks it for releases. The program is `tekken-studio.exe`, the installer `tekken-studio-<version>-setup.exe`, and new installs go in `%LOCALAPPDATA%\Programs\tekken-studio`. Installing over an older version keeps its folder and deletes the old `tekken8mvm.exe` and shortcuts.
- Internal names stay as they were, so settings and recordings carry over: the `tekken8mvm` folder in the game's Win64 folder, `tekken8mvm.ini`, `Videos\tekken8mvm`, the mod's marker and its ReShade add-on name.
- `tools/make_portable.ps1` (run by `installer\build_installer.cmd`) makes `tekken-studio-<version>-portable.zip`: the program, `mod\dinput8.dll`, the README and the licences, with forward slashes in the zip's paths. Unzipped to a temp folder, it started and showed its window. `publish_release.ps1` uploads it too, and replaces the files of a release that's already there.

## 0.1 beta: any picture shape with the fighters in place, own replays saved as files (18 November, measured on 3.03.01)
- The game widens its own camera's view for the window's shape: with the view at 65 degrees in 16:9, the projection got 79.8017 at 21:9 and anything wider, 71.2586 at 2:1, 76.5027 at 2.2:1, 62.6994 at 1.7:1, and 59.6567 at 16:10 and anything narrower (portrait too). In tangent that's the 16:9 width times the aspect over 16:9, held between 1.6 and 21:9 (2.3333). It goes by the window, not by the shape the view asks for: a 1920 x 1080 window with the view held at 21:9 still got 65. The axis rule passed to the projection is 1 (keep the width), and with the shape held the engine takes the field of view across the width (21:9 and 32:9 with no correction zoomed in by 1.000 and 1.5).
- `cine::FovAt16x9` takes the game's widening off (its own camera only: the studio's cameras' field of view is meant for 16:9) and `cine::FovForAspect` widens or narrows the 16:9 view for the chosen shape, keeping its height (fit 0) or its width (fit 1). Unit tests 279 checks, 0 failed.
- Measured on recordings of a paused replay, matching the middle of each against 16:9 by zoom and shift: 2520 x 1080 (21:9) 1.000, 5040 x 2160 0.998, 3840 x 1080 (32:9) 0.996, 2580 x 1080 (2.39) 1.004, 1080 x 1920 (9:16) 0.998, 1080 x 1728 (10:16) 1.002, all with no shift. Before the game's widening was taken off, 21:9 was 0.760 and 9:16 1.112.
- The game draws its HUD in a 16:9 band across the middle of a portrait picture: hide it in the Look tab for a clean portrait video.
- Fixed a black recording: the recorder mapped its readback buffer up to the row pitch times the height, which runs past the buffer when a row isn't a multiple of 256 bytes (2520 or 5040 pixels wide). The map failed and the frame stayed black, but only when the buffer had just been made for that size, so it came and went. The range now ends at the last row's own width, and a failed read stops the recording with an error instead of writing black frames. Before the fix, 2520 x 1080 recordings came out black in three of five tries; after it, the first one of a fresh session was fine.
- The free camera at the game camera's pose, field of view 65: 21:9 1.002, 9:16 0.998, 32:9 1.002 against its own 16:9. Its field of view isn't widened by the game, so nothing comes off first. The key markers drawn over the picture stayed on the same spot (next to Steve's head) at 16:9 and 21:9: the drawing now takes the field of view the projection really got.
- A preset saved before this has no `scene.keep_fighters` and loads with it off, so a saved 2.39 look keeps its crop.
- Own replays: `sharing::AutoExportTick` (the 3-second thread) writes every replay in the list that isn't a downloaded one to `my_replays`, once the list has held for three checks, skipping fights already in the folder (date and both names). First session: 100 files of 653 KB in under a second; the next session wrote none. The files first carried the version the older-replays pass had stamped on them (30301); the real version is now kept when a slot is stamped (`g_realVersions`, by fight) and written to the file: 20901 and 30000 for February and March matches. Not seen: a replay added to the list while a session runs (no match was played to the end in one). `auto_export_replays` in tekken8mvm.ini, on by default, set from the launcher's Replays tab and the editor's.

## 0.1 beta: the HUD takes the picture's shape (19 November, measured on 3.03.01)
- Every HUD screen on the window (`WBP_UI_HUD_S2`, `WBP_UI_Replay`, `WBP_UI_Practice_S2`, `WBP_UI_GameMessage`, `WBP_UI_HUD_ControlGuide`) is a root CanvasPanel holding a ScaleBox set to scale to fit (stretch 2), anchored to the whole window, around a canvas of boxes exactly 3840 x 2160 (the gauges, the zone, the information, the replay and practice controls); the timer (`WBP_UI_HUD_Timer`) is a small part placed from the design's centre, 1020 above it. Their UI scale is 0.5 at 1080p. The button bar at the bottom (`WBP_UI_Guide_Button`) and `WBP_UI_InformationBar` belong to the game instance and aren't added to the window the usual way: new anchors on their ScaleBox didn't move them in a letterbox, even after asking them to lay out again.
- `src/mod/hudframe.cpp`: *In the picture* anchors each screen on the window to the picture (`SetAnchorsInViewport`), and draws the two bars at the picture's size with a render transform about the window's centre. *Laid out for the shape* also resizes the 3840 x 2160 boxes to the shape (3840 x 3840/aspect for portrait, 2160 x aspect x 2160 for wide) and moves the small centre-placed parts in the outer thirds with their edge; the bars go to the bottom of the picture. All of it is put back when the option or the frame goes back to the game's own.
- Seen on screen in a replay at 9:16 and 32:9 in a 1920 x 1080 window, and in recordings at 1080 x 1920 and 3840 x 1080: in portrait the gauges and timer at the top, the wins and the button bar at the bottom, the input lists down the sides; in 32:9 the input lists and wins at the frame's edges. `hudtree [words|chain words]` logs the HUD's widget trees, `widgetvis <class> <0|1>` hides or shows widgets.
- A small part in the middle third stays in the middle (in portrait, the frame data box and the version badge).

## 0.1 beta: the replay saver, for normal and online play (18 November, measured on 3.03.01, offline)
- `src/saver/saver.cpp` builds `dist/replay-saver/dinput8.dll` (1.6 MB, imports only KERNEL32): the dinput8 forwarders, `sharing.cpp` and a thread that runs `AutoExportTick` every 5 seconds, inside `__try` so a fault while reading stops the saver, not the game. No MinHook, no hooks, no memory writes (it never marks older replays, so files keep their real version), no network block. Its marker is `TEKKEN-STUDIO-REPLAY-SAVER`, not the studio's, so `InstallMod` sets it aside for a session like another mod's DLL and `RemoveMod` puts it back. `sharing.cpp` no longer reads the recorder: its folder comes from `SetOutputDirSource`.
- Launcher: *Save my replays while I play, online too* in the Replays tab (`InstallReplaySaver` / `RemoveReplaySaver`), a line on the Play tab while it's in, the installed copy updated when the launcher starts, and taken out on uninstall. It isn't listed among other mods.
- Measured with Steam in Offline Mode, the game started from Steam with no session: the saver logged its folder at start, found the replay block once the title screen was passed, and wrote 100 files to Videos\tekken8mvm\my_replays (the February ones as version 20901). A cinematic session from the launcher then had the studio's mod (5.5 MB) in place and the saver as `dinput8.dll.tekken8mvm-backup`; when the game closed the saver was back as `dinput8.dll`.
- Not tested online: that's the user's to do.

## 0.1 beta: Tekken Studio for TTT2 on BlueSpark (18 November, measured on BlueSpark release 23, 4.7.4)
- BlueSpark's play test has no source for its own code, only `rex\prebuilt\lib\ttt2_ours.lib`, which its install links with the recompiled game into `ttt2.exe`. `ttt2/build-studio.ps1` reads that link line from `build.ninja`, compiles `ttt2/src` with the same clang, and links a second exe, `rex\out\build\tekken-studio\ttt2-studio.exe`. Nothing of BlueSpark's is changed. `run-studio.ps1` starts it with BlueSpark's switches minus the online ones, plus `--ttt2_update_notice=false`.
- A recompiled function `sub_X` is a weak alias of `__imp__sub_X`, so a strong `sub_X` in the studio replaces it. It must not be one of the 591 the library already replaces.
- BlueSpark already records replays (`.ttt2replay` in `rex\userdata\replays`) and has a viewer, so none was written. Its functions are called by their C++ names (`ttt2/src/bluespark.h`).
- Camera: `[0x82E6B05C]` +724 camera object, +436 manager, +316 slot in use, `[manager+340+slot*4]` block, +16 the record: eye (+0), target (+16), up (+32), field of view in degrees (+56), big-endian floats. Y is up, units about a millimetre; the fight camera sits about 4600 back at 50 degrees. `sub_82541878` writes it, 14 times per fight frame. Writing the record after that call moves the whole picture, in Arcade, Practice, Versus and inside a replay. The effects' copy at 0x82DE69A0 and the ALE camera (`sub_82BAD5D8`) don't move the picture.
- Time: BlueSpark's training hold stops the fight but answers late, and the picture is presented about 160 times a second, so toggling it per picture ran no frames. The studio keeps a budget instead: each tick adds elapsed time times speed, takes off the frames that ran (camera writes / 14), and holds while the budget is under one frame. Measured in Practice: 14.00, 7.00, 3.35 and 0.69 writes per 60 Hz tick at 1x, 0.5x, 0.25x and 0.05x; 0 paused; one step is 14 writes. The hold only works with BlueSpark's pages on, so the studio always runs with them.
- Not working: the hold isn't consulted while a replay plays, so pause, slow motion and steps do nothing there (BlueSpark's A and LB are the replay controls). A held frame doesn't recompute its view, so a paused fight creeps at 0.05x while the camera moves (not measured). The projection for the path in the picture isn't calibrated. No recording, no timeline seeking.
- A replay recorded with `--ttt2_test_quick_fights` only plays back with the same switch.
- Speed, measured 19 November against a stopwatch in a Practice fight (window in front): BlueSpark's own `ttt2.exe` and the studio both show 55 to 59 different pictures a second; BlueSpark's frame counter runs 59.6 to 60.2 a second; the studio's clock ran 1.00, 0.50, 0.25 and 0.05 seconds a second at 1x, 0.5x, 0.25x and 0.05x and 0 paused (camera writes 834, 415, 208, 41 and 0 a second). One earlier session had everything (BlueSpark's own counter too) at about a tenth of that; it didn't come back after a restart, and the cause wasn't found.
- Paused, a camera move now runs one fight frame per move (a jump ran exactly 1), so the picture follows the camera.
- Replays: BlueSpark's replay plays inside Practice with the recorded pads fed in (`Ttt2ReplayPad`), and its viewer has a hold of its own (`Ttt2ReplayHolding`) with no call to set it from outside. In a replay, both kinds of `Ttt2TrainingHoldRequest` did nothing (the frame count went on, `Ttt2TrainingHoldHolding` stayed false), and J, K, U, I, O and Space from the keyboard didn't reach the viewer.
- Time in a replay (19 November): the game's call stack at the camera write, read off the guest stack in the hook (each frame's back chain, the return address 8 under it): 82541878 < 82545EF8 < 82547DE0 < 82542338 < 822D36C8* < 825DEBD0* < 825DCE80 < 8262E418 < 825E04F0 < 825E0AE0 < 825EEBD0* < 825EF0B8* < 82583288 < 825834A8 < 823359F8 (* replaced by BlueSpark's library). 825DCE80, 8262E418, 825E04F0 and 825E0AE0 each run once a frame. Leaving any one of them out held the replay's frame count and the camera writes at +0 for 3 s, and the replay went on from the same frame at 60 a second, still a replay; with 825E04F0 or 825E0AE0 left out the fighters weren't drawn meanwhile, with the inner two the picture stayed whole. `skip.cpp` replaces `sub_825DCE80` and asks `ReplayFrameAllowed()` once a frame.
- Measured in a replay against a stopwatch: 60.04, 29.83, 15.01, 5.94 and 2.96 replay frames a second at 1x, 0.5x, 0.25x, 0.1x and 0.05x; 0.00 paused; one step +1 frame, three +3; 60.18 playing again. Paused, one camera jump ran 1 frame and the picture showed the new angle. BlueSpark's log for that run: "played to the match's end at frame 10985 of 10986; 148 of 184 checks differed, the first at frame 1800", word for word what the morning's run of the same replay said before any of this existed, so the holds changed nothing it checks. (That replay was recorded with `--ttt2_test_quick_fights`; a replay of an ordinary match hasn't been through it.)
- `ttt2/tools/callers.py` lists who calls a recompiled function from the generated sources (it stops where the calls go through pointers, three levels up from the camera write); the `stack` command logs the game's stack at the next camera write.
- Launcher: a TTT2 tab (folder in `launcher.ini` as `ttt2_playtest`, release check, Install, Simple, Studio). Clicking **Open the studio** started `ttt2-studio.exe` with `start-mode.txt` = studio. The installer and the portable zip carry `ttt2\build-studio.ps1`, `run-studio.ps1` and `src`.
- Test tools: `ttt2/tools/drive-ttt2.ps1` (picture, keys, the studio's command file), `to-fight.ps1`, `to-replay.ps1`, `test-time.ps1`, `whois.py`.
